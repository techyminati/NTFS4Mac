/*
 * Block device layer.
 *
 * libntfs-3g reads and writes at arbitrary byte offsets, but raw disks
 * (/dev/rdiskN, FSKit block resources) only accept whole sectors. This file
 * turns libntfs-3g's byte I/O into sector aligned I/O on an n4m_blockdev and
 * keeps a small write-through block cache in front of it, because the
 * library re-reads the same MFT records and index blocks constantly.
 *
 * The cache is write-through, so cached blocks always equal what is on disk
 * and large transfers can bypass it without coherency problems.
 */
#include <fcntl.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

#define CACHE_BLOCK	4096u		/* cache granularity */
#define CACHE_BLOCKS	4096u		/* 16 MiB */
#define CACHE_BUCKETS	8192u
#define CACHE_IO_MAX	(128u * 1024u)	/* bigger transfers bypass cache */

struct cblock {
	uint64_t blkno;
	struct cblock *hnext;		/* hash chain */
	struct cblock *prev, *next;	/* LRU list, head is most recent */
	bool used;
	uint8_t *data;
};

struct n4m_cache {
	uint32_t bsize;
	uint32_t nblocks;
	struct cblock *blocks;
	struct cblock **hash;
	struct cblock *lru_head, *lru_tail;
	uint8_t *mem;
};

struct devpriv {
	struct n4m_volume *v;
	s64 pos;
};

#define DEVPRIV(dev) ((struct devpriv *)(dev)->d_private)

/* ---- cache ------------------------------------------------------------ */

static struct n4m_cache *bc_new(uint32_t sector_size)
{
	struct n4m_cache *c = calloc(1, sizeof(*c));
	uint32_t i;

	if (!c)
		return NULL;
	c->bsize = sector_size > CACHE_BLOCK ? sector_size : CACHE_BLOCK;
	c->nblocks = (uint32_t)((uint64_t)CACHE_BLOCKS * CACHE_BLOCK / c->bsize);
	c->blocks = calloc(c->nblocks, sizeof(*c->blocks));
	c->hash = calloc(CACHE_BUCKETS, sizeof(*c->hash));
	c->mem = malloc((size_t)c->nblocks * c->bsize);
	if (!c->blocks || !c->hash || !c->mem) {
		free(c->blocks);
		free(c->hash);
		free(c->mem);
		free(c);
		return NULL;
	}
	for (i = 0; i < c->nblocks; i++) {
		struct cblock *b = &c->blocks[i];

		b->data = c->mem + (size_t)i * c->bsize;
		b->prev = i ? &c->blocks[i - 1] : NULL;
		b->next = i + 1 < c->nblocks ? &c->blocks[i + 1] : NULL;
	}
	c->lru_head = &c->blocks[0];
	c->lru_tail = &c->blocks[c->nblocks - 1];
	return c;
}

static void bc_free(struct n4m_cache *c)
{
	if (!c)
		return;
	free(c->blocks);
	free(c->hash);
	free(c->mem);
	free(c);
}

static inline uint32_t hslot(uint64_t blkno)
{
	return (uint32_t)((blkno * 0x9E3779B97F4A7C15ULL) >> 40) % CACHE_BUCKETS;
}

static void lru_unlink(struct n4m_cache *c, struct cblock *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		c->lru_head = b->next;
	if (b->next)
		b->next->prev = b->prev;
	else
		c->lru_tail = b->prev;
	b->prev = b->next = NULL;
}

static void lru_push_front(struct n4m_cache *c, struct cblock *b)
{
	b->prev = NULL;
	b->next = c->lru_head;
	if (c->lru_head)
		c->lru_head->prev = b;
	c->lru_head = b;
	if (!c->lru_tail)
		c->lru_tail = b;
}

static struct cblock *bc_find(struct n4m_cache *c, uint64_t blkno)
{
	struct cblock *b;

	for (b = c->hash[hslot(blkno)]; b; b = b->hnext)
		if (b->blkno == blkno)
			return b;
	return NULL;
}

static void hash_remove(struct n4m_cache *c, struct cblock *b)
{
	struct cblock **pp = &c->hash[hslot(b->blkno)];

	while (*pp && *pp != b)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = b->hnext;
	b->hnext = NULL;
	b->used = false;
}

/* Takes the least recently used block and binds it to blkno. */
static struct cblock *bc_claim(struct n4m_cache *c, uint64_t blkno)
{
	struct cblock *b = c->lru_tail;

	if (b->used)
		hash_remove(c, b);
	b->blkno = blkno;
	b->used = true;
	b->hnext = c->hash[hslot(blkno)];
	c->hash[hslot(blkno)] = b;
	lru_unlink(c, b);
	lru_push_front(c, b);
	return b;
}

static void bc_touch(struct n4m_cache *c, struct cblock *b)
{
	if (c->lru_head != b) {
		lru_unlink(c, b);
		lru_push_front(c, b);
	}
}

/* After a write hit the disk, refresh any cached copy of those bytes. */
static void bc_update(struct n4m_cache *c, uint64_t off, const void *buf,
		size_t len)
{
	uint64_t blk, first, last;

	if (!c || !len)
		return;
	first = off / c->bsize;
	last = (off + len - 1) / c->bsize;
	for (blk = first; blk <= last; blk++) {
		struct cblock *b = bc_find(c, blk);
		uint64_t bstart = blk * c->bsize;
		uint64_t s = off > bstart ? off : bstart;
		uint64_t e = off + len < bstart + c->bsize ?
				off + len : bstart + c->bsize;

		if (b)
			memcpy(b->data + (s - bstart),
				(const uint8_t *)buf + (s - off), e - s);
	}
}

/* Forgets cached copies of a range whose write failed half way. */
static void bc_invalidate(struct n4m_cache *c, uint64_t off, size_t len)
{
	uint64_t blk, first, last;

	if (!c || !len)
		return;
	first = off / c->bsize;
	last = (off + len - 1) / c->bsize;
	for (blk = first; blk <= last; blk++) {
		struct cblock *b = bc_find(c, blk);

		if (b)
			hash_remove(c, b);
	}
}

/* ---- aligned access to the backing device ---------------------------- */

static int raw_read(struct n4m_volume *v, void *buf, uint64_t off,
		size_t len)
{
	return v->bdev.read(v->bdev.ctx, buf, off, len);
}

static int raw_write(struct n4m_volume *v, const void *buf, uint64_t off,
		size_t len)
{
	return v->bdev.write(v->bdev.ctx, buf, off, len);
}

/*
 * Reads a sector aligned range through the cache. Only whole cache blocks
 * that lie fully inside the device are cached; anything else goes direct.
 */
static int cached_read(struct n4m_volume *v, uint8_t *buf, uint64_t off,
		size_t len)
{
	struct n4m_cache *c = v->cache;
	uint64_t end = off + len;
	int err;

	while (off < end) {
		uint64_t blk = off / c->bsize;
		uint64_t bstart = blk * c->bsize;
		uint64_t bend = bstart + c->bsize;
		size_t n = (size_t)((end < bend ? end : bend) - off);
		struct cblock *b;

		if (bend > v->bdev.size) {
			/* partial block at the end of the device */
			err = raw_read(v, buf, off, n);
			if (err)
				return err;
		} else {
			b = bc_find(c, blk);
			if (b) {
				bc_touch(c, b);
			} else {
				b = bc_claim(c, blk);
				err = raw_read(v, b->data, bstart, c->bsize);
				if (err) {
					hash_remove(c, b);
					return err;
				}
			}
			memcpy(buf, b->data + (off - bstart), n);
		}
		buf += n;
		off += n;
	}
	return 0;
}

static int aligned_read(struct n4m_volume *v, void *buf, uint64_t off,
		size_t len)
{
	if (v->cache && len <= CACHE_IO_MAX)
		return cached_read(v, buf, off, len);
	return raw_read(v, buf, off, len);
}

static int aligned_write(struct n4m_volume *v, const void *buf, uint64_t off,
		size_t len)
{
	int err = raw_write(v, buf, off, len);

	if (!err)
		bc_update(v->cache, off, buf, len);
	else
		bc_invalidate(v->cache, off, len);
	return err;
}

/* Byte granular read on top of sector aligned I/O. */
static int byte_read(struct n4m_volume *v, uint8_t *buf, uint64_t off,
		size_t len)
{
	uint32_t ss = v->bdev.sector_size;
	uint8_t *bounce = NULL;
	int err = 0;

	while (len) {
		uint64_t soff = off & ~(uint64_t)(ss - 1);
		size_t head = (size_t)(off - soff);

		if (head || len < ss) {
			size_t n = ss - head < len ? ss - head : len;

			if (!bounce && !(bounce = malloc(ss)))
				return ENOMEM;
			err = aligned_read(v, bounce, soff, ss);
			if (err)
				break;
			memcpy(buf, bounce + head, n);
			buf += n;
			off += n;
			len -= n;
		} else {
			size_t n = len & ~(size_t)(ss - 1);

			err = aligned_read(v, buf, off, n);
			if (err)
				break;
			buf += n;
			off += n;
			len -= n;
		}
	}
	free(bounce);
	return err;
}

/* Byte granular write, read-modify-write for partial sectors. */
static int byte_write(struct n4m_volume *v, const uint8_t *buf, uint64_t off,
		size_t len)
{
	uint32_t ss = v->bdev.sector_size;
	uint8_t *bounce = NULL;
	int err = 0;

	while (len) {
		uint64_t soff = off & ~(uint64_t)(ss - 1);
		size_t head = (size_t)(off - soff);

		if (head || len < ss) {
			size_t n = ss - head < len ? ss - head : len;

			if (!bounce && !(bounce = malloc(ss)))
				return ENOMEM;
			err = aligned_read(v, bounce, soff, ss);
			if (err)
				break;
			memcpy(bounce + head, buf, n);
			err = aligned_write(v, bounce, soff, ss);
			if (err)
				break;
			buf += n;
			off += n;
			len -= n;
		} else {
			size_t n = len & ~(size_t)(ss - 1);

			err = aligned_write(v, buf, off, n);
			if (err)
				break;
			buf += n;
			off += n;
			len -= n;
		}
	}
	free(bounce);
	return err;
}

/* ---- ntfs_device_operations ------------------------------------------ */

static int dev_open(struct ntfs_device *dev, int flags)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;

	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}
	if ((flags & O_ACCMODE) != O_RDONLY && v->bdev.readonly) {
		errno = EROFS;
		return -1;
	}
	if ((flags & O_ACCMODE) == O_RDONLY)
		NDevSetReadOnly(dev);
	else
		NDevClearReadOnly(dev);
	if (!v->cache)
		v->cache = bc_new(v->bdev.sector_size);
	DEVPRIV(dev)->pos = 0;
	NDevSetBlock(dev);
	NDevSetOpen(dev);
	return 0;
}

static int dev_sync(struct ntfs_device *dev)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;
	int err;

	if (NDevReadOnly(dev))
		return 0;
	err = v->bdev.flush ? v->bdev.flush(v->bdev.ctx) : 0;
	if (err) {
		errno = err;
		return -1;
	}
	NDevClearDirty(dev);
	return 0;
}

static int dev_close(struct ntfs_device *dev)
{
	if (!NDevOpen(dev)) {
		errno = EBADF;
		return -1;
	}
	if (NDevDirty(dev) && dev_sync(dev))
		return -1;
	NDevClearOpen(dev);
	return 0;
}

static s64 dev_seek(struct ntfs_device *dev, s64 offset, int whence)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;
	s64 pos;

	switch (whence) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = DEVPRIV(dev)->pos + offset;
		break;
	case SEEK_END:
		pos = (s64)v->bdev.size + offset;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if (pos < 0 || (u64)pos > v->bdev.size) {
		errno = EINVAL;
		return -1;
	}
	DEVPRIV(dev)->pos = pos;
	return pos;
}

static s64 dev_pread(struct ntfs_device *dev, void *buf, s64 count,
		s64 offset)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;
	int err;

	if (count < 0 || offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if ((u64)offset >= v->bdev.size)
		return 0;
	if ((u64)(offset + count) > v->bdev.size)
		count = (s64)(v->bdev.size - (u64)offset);
	if (!count)
		return 0;
	err = byte_read(v, buf, (uint64_t)offset, (size_t)count);
	if (err) {
		errno = err;
		return -1;
	}
	return count;
}

static s64 dev_pwrite(struct ntfs_device *dev, const void *buf, s64 count,
		s64 offset)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;
	int err;

	if (NDevReadOnly(dev)) {
		errno = EROFS;
		return -1;
	}
	if (count < 0 || offset < 0 ||
			(u64)(offset + count) > v->bdev.size) {
		errno = ENOSPC;
		return -1;
	}
	if (!count)
		return 0;
	NDevSetDirty(dev);
	err = byte_write(v, buf, (uint64_t)offset, (size_t)count);
	if (err) {
		errno = err;
		return -1;
	}
	return count;
}

static s64 dev_read(struct ntfs_device *dev, void *buf, s64 count)
{
	s64 n = dev_pread(dev, buf, count, DEVPRIV(dev)->pos);

	if (n > 0)
		DEVPRIV(dev)->pos += n;
	return n;
}

static s64 dev_write(struct ntfs_device *dev, const void *buf, s64 count)
{
	s64 n = dev_pwrite(dev, buf, count, DEVPRIV(dev)->pos);

	if (n > 0)
		DEVPRIV(dev)->pos += n;
	return n;
}

static int dev_stat(struct ntfs_device *dev, struct stat *st)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;

	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFBLK | 0600;
	st->st_size = (off_t)v->bdev.size;
	st->st_blksize = v->bdev.sector_size;
	return 0;
}

static int dev_ioctl(struct ntfs_device *dev, unsigned long request,
		void *argp)
{
	struct n4m_volume *v = DEVPRIV(dev)->v;

	switch (request) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)argp = v->bdev.sector_size;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)argp = v->bdev.size / v->bdev.sector_size;
		return 0;
	default:
		errno = ENOTTY;
		return -1;
	}
}

static struct ntfs_device_operations n4m_dev_ops = {
	.open	= dev_open,
	.close	= dev_close,
	.seek	= dev_seek,
	.read	= dev_read,
	.write	= dev_write,
	.pread	= dev_pread,
	.pwrite	= dev_pwrite,
	.sync	= dev_sync,
	.stat	= dev_stat,
	.ioctl	= dev_ioctl,
};

struct ntfs_device *n4m_device_new(struct n4m_volume *v)
{
	struct devpriv *p = calloc(1, sizeof(*p));
	struct ntfs_device *dev;

	if (!p)
		return NULL;
	p->v = v;
	dev = ntfs_device_alloc("ntfs4mac", 0, &n4m_dev_ops, p);
	if (!dev)
		free(p);
	return dev;
}

void n4m_device_free_cache(struct n4m_volume *v)
{
	bc_free(v->cache);
	v->cache = NULL;
}

/* ---- path based block device (images, /dev/diskN, /dev/rdiskN) ------- */

struct pathdev {
	int fd;
	bool is_file;
};

static int path_read(void *ctx, void *buf, uint64_t off, size_t len)
{
	struct pathdev *p = ctx;

	while (len) {
		ssize_t n = pread(p->fd, buf, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (n == 0)
			return EIO;
		buf = (uint8_t *)buf + n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int path_write(void *ctx, const void *buf, uint64_t off, size_t len)
{
	struct pathdev *p = ctx;

	while (len) {
		ssize_t n = pwrite(p->fd, buf, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (n == 0)
			return EIO;
		buf = (const uint8_t *)buf + n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int path_flush(void *ctx)
{
	struct pathdev *p = ctx;

	/* F_FULLFSYNC asks the drive to flush its own cache as well */
	if (fcntl(p->fd, F_FULLFSYNC) == 0)
		return 0;
	if (!p->is_file && ioctl(p->fd, DKIOCSYNCHRONIZE) == 0)
		return 0;
	return fsync(p->fd) ? errno : 0;
}

static void path_close(void *ctx)
{
	struct pathdev *p = ctx;

	close(p->fd);
	free(p);
}

int n4m_blockdev_open_path(const char *path, bool readonly, n4m_blockdev *out)
{
	struct pathdev *p;
	struct stat st;
	int fd, err;

	fd = open(path, readonly ? O_RDONLY : O_RDWR);
	if (fd < 0 && !readonly && (errno == EACCES || errno == EROFS)) {
		readonly = true;
		fd = open(path, O_RDONLY);
	}
	if (fd < 0)
		return errno;
	if (fstat(fd, &st)) {
		err = errno;
		close(fd);
		return err;
	}
	p = calloc(1, sizeof(*p));
	if (!p) {
		close(fd);
		return ENOMEM;
	}
	p->fd = fd;
	memset(out, 0, sizeof(*out));
	if (S_ISREG(st.st_mode)) {
		p->is_file = true;
		out->sector_size = 512;
		out->size = (uint64_t)st.st_size & ~511ULL;
	} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		uint32_t bs = 0;
		uint64_t count = 0;

		if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) ||
				ioctl(fd, DKIOCGETBLOCKCOUNT, &count) || !bs) {
			err = errno ? errno : ENOTSUP;
			close(fd);
			free(p);
			return err;
		}
		out->sector_size = bs;
		out->size = count * bs;
	} else {
		close(fd);
		free(p);
		return ENODEV;
	}
	out->ctx = p;
	out->readonly = readonly;
	out->read = path_read;
	out->write = path_write;
	out->flush = path_flush;
	out->close = path_close;
	return 0;
}
