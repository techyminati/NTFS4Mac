/*
 * NFSv3 procedures (RFC 1813), each mapped onto an engine call.
 *
 * Mac metadata: NFSv3 has no extended attributes, so macOS stores them in
 * a "._name" AppleDouble file next to "name". Instead of littering NTFS
 * drives with those, "._name" is served as a virtual file whose bytes live
 * in an NTFS alternate data stream of "name". Windows never sees it as a
 * separate file and it follows the file around on renames. A real "._name"
 * file (made by some other tool, or when "name" does not exist) still works
 * as a normal file.
 */
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "nfs.h"

#define FH_MAGIC	0x4e344d31u	/* "N4M1": a file or folder */
#define FH_MAGIC_AD	0x4e344d41u	/* "N4MA": Mac metadata of a file */
#define AD_FILEID	(1ULL << 63)	/* fileid space for virtual files */
#define AD		N4M_APPLEDOUBLE_STREAM

enum {
	NF3REG = 1, NF3DIR, NF3BLK, NF3CHR, NF3LNK, NF3SOCK, NF3FIFO,
};

enum { UNSTABLE = 0, DATA_SYNC = 1, FILE_SYNC = 2 };
enum { UNCHECKED = 0, GUARDED = 1, EXCLUSIVE = 2 };
enum { DONT_CHANGE = 0, SET_TO_SERVER_TIME = 1, SET_TO_CLIENT_TIME = 2 };

#define ACCESS3_READ	0x01
#define ACCESS3_LOOKUP	0x02
#define ACCESS3_MODIFY	0x04
#define ACCESS3_EXTEND	0x08
#define ACCESS3_DELETE	0x10
#define ACCESS3_EXECUTE	0x20

#define FSF3_LINK	0x01
#define FSF3_SYMLINK	0x02
#define FSF3_HOMOGENEOUS 0x08
#define FSF3_CANSETTIME	0x10

/* A decoded file handle */
struct fh {
	uint64_t ref;	/* MFT reference (of the base file for ad) */
	bool ad;	/* virtual "._name" metadata file */
};

static inline uint64_t ino_of(uint64_t ref)
{
	return ref & 0x0000ffffffffffffULL;
}

uint32_t nfs_errno_to_stat(int err)
{
	switch (err) {
	case 0:			return NFS3_OK;
	case EPERM:		return NFS3ERR_PERM;
	case ENOENT:		return NFS3ERR_NOENT;
	case ENXIO:		return NFS3ERR_NXIO;
	case EACCES:		return NFS3ERR_ACCES;
	case EEXIST:		return NFS3ERR_EXIST;
	case EXDEV:		return NFS3ERR_XDEV;
	case ENODEV:		return NFS3ERR_NODEV;
	case ENOTDIR:		return NFS3ERR_NOTDIR;
	case EISDIR:		return NFS3ERR_ISDIR;
	case EINVAL:		return NFS3ERR_INVAL;
	case EILSEQ:		return NFS3ERR_INVAL;
	case EFBIG:		return NFS3ERR_FBIG;
	case ENOSPC:		return NFS3ERR_NOSPC;
	case EROFS:		return NFS3ERR_ROFS;
	case EMLINK:		return NFS3ERR_MLINK;
	case ENAMETOOLONG:	return NFS3ERR_NAMETOOLONG;
	case ENOTEMPTY:		return NFS3ERR_NOTEMPTY;
	case EDQUOT:		return NFS3ERR_DQUOT;
	case ESTALE:		return NFS3ERR_STALE;
	case ENOTSUP:		return NFS3ERR_NOTSUPP;
	default:		return NFS3ERR_IO;
	}
}

/* ---- handles ---------------------------------------------------------- */

static void put_fh_magic(struct nfs_server *s, struct xdr_out *out,
		uint32_t magic, uint64_t ref)
{
	xdr_put_u32(out, NFS_FHSIZE);
	xdr_put_u32(out, magic);
	xdr_put_u32(out, (uint32_t)s->fsid);
	xdr_put_u64(out, ref);
}

void nfs_put_fh(struct nfs_server *s, struct xdr_out *out, uint64_t ref)
{
	put_fh_magic(s, out, FH_MAGIC, ref);
}

static void put_fh(struct nfs_server *s, struct xdr_out *out,
		const struct fh *fh)
{
	put_fh_magic(s, out, fh->ad ? FH_MAGIC_AD : FH_MAGIC, fh->ref);
}

/* Reads any file handle. Returns an nfsstat3. */
static uint32_t get_fh_any(struct nfs_server *s, struct xdr_in *in,
		struct fh *fh)
{
	size_t len;
	const uint8_t *p = xdr_get_opaque(in, 64, &len);
	struct xdr_in h;
	uint32_t magic;

	fh->ref = 0;
	fh->ad = false;
	if (!p || len != NFS_FHSIZE)
		return NFS3ERR_BADHANDLE;
	h = (struct xdr_in){ .p = p, .len = len };
	magic = xdr_get_u32(&h);
	if (magic != FH_MAGIC && magic != FH_MAGIC_AD)
		return NFS3ERR_BADHANDLE;
	if (xdr_get_u32(&h) != (uint32_t)s->fsid)
		return NFS3ERR_STALE;
	fh->ref = xdr_get_u64(&h);
	fh->ad = magic == FH_MAGIC_AD;
	return NFS3_OK;
}

/* getattr that also checks the handle's sequence number */
static int getattr_ref(struct nfs_server *s, uint64_t ref, n4m_attr *a)
{
	int err = n4m_getattr(s->vol, ino_of(ref), a);

	if (!err && a->ref != ref)
		err = ESTALE;	/* the MFT record got reused */
	return err;
}

/*
 * Reads a handle that has to be a real file or folder (mostly folders).
 * The engine works on MFT record numbers only, so check here that the
 * record still is the one the handle was made for: after a delete the
 * record can be reused, and a stale handle must not act on the new item.
 */
static uint32_t get_fh(struct nfs_server *s, struct xdr_in *in, uint64_t *ref)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);
	n4m_attr a;

	*ref = fh.ref;
	if (st == NFS3_OK && fh.ad)
		st = NFS3ERR_NOTDIR;
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(getattr_ref(s, fh.ref, &a));
	return st;
}

/* ---- attributes ------------------------------------------------------- */

/* Attributes of the virtual "._name" file of base. */
static void ad_fill(const n4m_attr *base, uint64_t size, n4m_attr *a)
{
	*a = *base;
	a->type = N4M_TYPE_FILE;
	a->mode = 0644;
	a->nlink = 1;
	a->flags = 0;
	a->rdev = 0;
	a->size = size;
	a->alloc_size = (size + 4095) & ~4095ULL;
	a->ino = AD_FILEID | base->ino;
}

static int attr_of(struct nfs_server *s, const struct fh *fh, n4m_attr *a)
{
	n4m_attr base;
	uint64_t size;
	int err;

	if (!fh->ad)
		return getattr_ref(s, fh->ref, a);
	err = getattr_ref(s, fh->ref, &base);
	if (err)
		return err;
	err = n4m_stream_size(s->vol, base.ino, AD, &size);
	if (err)
		return err == ENOENT ? ESTALE : err;
	ad_fill(&base, size, a);
	return 0;
}

static void put_time(struct xdr_out *out, struct timespec ts)
{
	if (ts.tv_sec < 0)
		ts.tv_sec = ts.tv_nsec = 0;
	if (ts.tv_sec > 0xffffffffLL)
		ts.tv_sec = 0xffffffffLL;
	xdr_put_u32(out, (uint32_t)ts.tv_sec);
	xdr_put_u32(out, (uint32_t)ts.tv_nsec);
}

static void put_fattr(struct nfs_server *s, struct xdr_out *out,
		const n4m_attr *a)
{
	uint32_t type, mode = a->mode;

	switch (a->type) {
	case N4M_TYPE_DIR:	type = NF3DIR; break;
	case N4M_TYPE_SYMLINK:	type = NF3LNK; break;
	case N4M_TYPE_BLK:	type = NF3BLK; break;
	case N4M_TYPE_CHR:	type = NF3CHR; break;
	case N4M_TYPE_SOCK:	type = NF3SOCK; break;
	case N4M_TYPE_FIFO:	type = NF3FIFO; break;
	default:		type = NF3REG; break;
	}
	/* Windows read-only files show up without write permission */
	if (s->readonly || (a->flags & UF_IMMUTABLE))
		mode &= ~0222u;
	xdr_put_u32(out, type);
	xdr_put_u32(out, mode);
	xdr_put_u32(out, a->nlink);
	xdr_put_u32(out, s->uid);
	xdr_put_u32(out, s->gid);
	xdr_put_u64(out, a->size);
	xdr_put_u64(out, a->alloc_size);
	xdr_put_u32(out, (uint32_t)major((dev_t)a->rdev));
	xdr_put_u32(out, (uint32_t)minor((dev_t)a->rdev));
	xdr_put_u64(out, s->fsid);
	xdr_put_u64(out, a->ino);
	put_time(out, a->atime);
	put_time(out, a->mtime);
	put_time(out, a->ctime);
}

static void put_post_attr(struct nfs_server *s, struct xdr_out *out,
		const n4m_attr *a)
{
	xdr_put_bool(out, a != NULL);
	if (a)
		put_fattr(s, out, a);
}

/* post_op_attr for a handle, fh may be NULL */
static void put_post_attr_fh(struct nfs_server *s, struct xdr_out *out,
		const struct fh *fh)
{
	n4m_attr a;

	put_post_attr(s, out, fh && fh->ref && !attr_of(s, fh, &a) ? &a :
			NULL);
}

static void put_post_attr_ref(struct nfs_server *s, struct xdr_out *out,
		uint64_t ref)
{
	struct fh fh = { .ref = ref };

	put_post_attr_fh(s, out, ref ? &fh : NULL);
}

struct pre_attr {
	bool valid;
	uint64_t size;
	struct timespec mtime, ctime;
};

static void pre_from(const n4m_attr *a, struct pre_attr *p)
{
	p->valid = true;
	p->size = a->size;
	p->mtime = a->mtime;
	p->ctime = a->ctime;
}

static void get_pre(struct nfs_server *s, uint64_t ref, struct pre_attr *p)
{
	n4m_attr a;

	p->valid = false;
	if (ref && !getattr_ref(s, ref, &a))
		pre_from(&a, p);
}

static void put_wcc_fh(struct nfs_server *s, struct xdr_out *out,
		const struct pre_attr *pre, const struct fh *fh)
{
	xdr_put_bool(out, pre && pre->valid);
	if (pre && pre->valid) {
		xdr_put_u64(out, pre->size);
		put_time(out, pre->mtime);
		put_time(out, pre->ctime);
	}
	put_post_attr_fh(s, out, fh);
}

static void put_wcc(struct nfs_server *s, struct xdr_out *out,
		const struct pre_attr *pre, uint64_t ref)
{
	struct fh fh = { .ref = ref };

	put_wcc_fh(s, out, pre, ref ? &fh : NULL);
}

/* ---- AppleDouble names ------------------------------------------------ */

/* "._foo" -> "foo", NULL when it is not an AppleDouble name */
static const char *ad_base(const char *name)
{
	if (name[0] != '.' || name[1] != '_' || !name[2])
		return NULL;
	if (!strcmp(name + 2, ".") || !strcmp(name + 2, ".."))
		return NULL;
	return name + 2;
}

/*
 * Resolves "._foo" in dir to the file foo. With need_stream it also has
 * to have Mac metadata stored already. Returns an errno.
 */
static int ad_lookup(struct nfs_server *s, uint64_t dir, const char *name,
		n4m_attr *base, bool need_stream, uint64_t *size)
{
	const char *b = ad_base(name);
	uint64_t sz = 0;
	int err;

	if (!b)
		return ENOENT;
	err = n4m_lookup(s->vol, ino_of(dir), b, base);
	if (err)
		return err;
	if (base->type != N4M_TYPE_FILE && base->type != N4M_TYPE_DIR)
		return ENOENT;
	err = n4m_stream_size(s->vol, base->ino, AD, &sz);
	if (err && (need_stream || err != ENOENT))
		return err;
	if (size)
		*size = err ? 0 : sz;
	return 0;
}

/* ---- sattr3 ----------------------------------------------------------- */

struct sattr {
	bool set_mode, set_uid, set_gid, set_size;
	uint32_t mode, uid, gid;
	uint64_t size;
	uint32_t atime_how, mtime_how;
	struct timespec atime, mtime;
};

static void get_time(struct xdr_in *in, struct timespec *ts)
{
	ts->tv_sec = xdr_get_u32(in);
	ts->tv_nsec = xdr_get_u32(in);
	if (ts->tv_nsec >= 1000000000)
		ts->tv_nsec = 999999999;
}

static void get_sattr(struct xdr_in *in, struct sattr *sa)
{
	memset(sa, 0, sizeof(*sa));
	if ((sa->set_mode = xdr_get_bool(in)))
		sa->mode = xdr_get_u32(in);
	if ((sa->set_uid = xdr_get_bool(in)))
		sa->uid = xdr_get_u32(in);
	if ((sa->set_gid = xdr_get_bool(in)))
		sa->gid = xdr_get_u32(in);
	if ((sa->set_size = xdr_get_bool(in)))
		sa->size = xdr_get_u64(in);
	sa->atime_how = xdr_get_u32(in);
	if (sa->atime_how == SET_TO_CLIENT_TIME)
		get_time(in, &sa->atime);
	sa->mtime_how = xdr_get_u32(in);
	if (sa->mtime_how == SET_TO_CLIENT_TIME)
		get_time(in, &sa->mtime);
}

/* Applies an sattr3 to an item. a holds its current attributes. */
static int apply_sattr(struct nfs_server *s, const n4m_attr *a,
		const struct sattr *sa)
{
	n4m_setattr_req r = { 0 };

	if (sa->set_size) {
		if (a->type == N4M_TYPE_DIR)
			return EISDIR;
		r.mask |= N4M_SET_SIZE;
		r.size = sa->size;
	}
	/*
	 * NTFS has no Unix modes. The owner write bit maps to the Windows
	 * read-only attribute on files, everything else is accepted as is.
	 */
	if (sa->set_mode && a->type == N4M_TYPE_FILE) {
		uint32_t flags = a->flags & ~(uint32_t)UF_IMMUTABLE;

		if (!(sa->mode & 0200))
			flags |= UF_IMMUTABLE;
		if (flags != a->flags) {
			r.mask |= N4M_SET_FLAGS;
			r.flags = flags;
		}
	}
	if (sa->atime_how == SET_TO_SERVER_TIME)
		r.mask |= N4M_SET_ATIME_NOW;
	else if (sa->atime_how == SET_TO_CLIENT_TIME) {
		r.mask |= N4M_SET_ATIME;
		r.atime = sa->atime;
	}
	if (sa->mtime_how == SET_TO_SERVER_TIME)
		r.mask |= N4M_SET_MTIME_NOW;
	else if (sa->mtime_how == SET_TO_CLIENT_TIME) {
		r.mask |= N4M_SET_MTIME;
		r.mtime = sa->mtime;
	}
	if (!r.mask)
		return 0;
	/* clearing read-only must happen before a truncate */
	if ((r.mask & N4M_SET_FLAGS) && (r.mask & N4M_SET_SIZE) &&
			!(r.flags & UF_IMMUTABLE)) {
		n4m_setattr_req f = { .mask = N4M_SET_FLAGS, .flags = r.flags };
		int err = n4m_setattr(s->vol, a->ino, &f, NULL);

		if (err)
			return err;
		r.mask &= ~(uint32_t)N4M_SET_FLAGS;
	}
	return n4m_setattr(s->vol, a->ino, &r, NULL);
}

/* ---- procedures ------------------------------------------------------- */

#define ARGS_OK(in, out) do { \
	if ((in)->err) { \
		xdr_put_u32(out, RPC_GARBAGE_ARGS); \
		return; \
	} \
	xdr_put_u32(out, RPC_SUCCESS); \
} while (0)

static void p_getattr(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);
	n4m_attr a;

	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(attr_of(s, &fh, &a));
	xdr_put_u32(out, st);
	if (st == NFS3_OK)
		put_fattr(s, out, &a);
}

static void p_setattr(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);
	struct sattr sa;
	struct timespec guard = { 0 };
	bool check;
	struct pre_attr pre = { 0 };
	n4m_attr a;

	get_sattr(in, &sa);
	if ((check = xdr_get_bool(in)))
		get_time(in, &guard);
	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(attr_of(s, &fh, &a));
	if (st == NFS3_OK) {
		pre_from(&a, &pre);
		if (check && (guard.tv_sec != a.ctime.tv_sec ||
				guard.tv_nsec != a.ctime.tv_nsec))
			st = NFS3ERR_NOT_SYNC;
		else if (fh.ad)
			/* only the size matters for metadata files */
			st = nfs_errno_to_stat(sa.set_size ?
				n4m_stream_truncate(s->vol, ino_of(fh.ref),
				AD, sa.size) : 0);
		else
			st = nfs_errno_to_stat(apply_sattr(s, &a, &sa));
	}
	xdr_put_u32(out, st);
	put_wcc_fh(s, out, &pre, &fh);
}

static void p_lookup(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t dir;
	uint32_t st = get_fh(s, in, &dir);
	char name[NFS_MAXNAME + 1];
	bool name_ok = xdr_get_string(in, name, sizeof(name));
	struct fh fh = { 0 };
	n4m_attr a, base;
	uint64_t size;

	ARGS_OK(in, out);
	if (st == NFS3_OK && !name_ok)
		st = NFS3ERR_NOENT;
	if (st == NFS3_OK && ad_base(name) &&
			!ad_lookup(s, dir, name, &base, true, &size)) {
		fh = (struct fh){ .ref = base.ref, .ad = true };
		ad_fill(&base, size, &a);
	} else if (st == NFS3_OK) {
		st = nfs_errno_to_stat(n4m_lookup(s->vol, ino_of(dir), name,
				&a));
		fh.ref = a.ref;
	}
	xdr_put_u32(out, st);
	if (st == NFS3_OK) {
		put_fh(s, out, &fh);
		put_post_attr(s, out, &a);
	}
	put_post_attr_ref(s, out, st == NFS3ERR_BADHANDLE ? 0 : dir);
}

static void p_access(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);
	uint32_t want = xdr_get_u32(in), grant = 0;
	n4m_attr a;

	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(attr_of(s, &fh, &a));
	xdr_put_u32(out, st);
	if (st != NFS3_OK) {
		put_post_attr(s, out, NULL);
		return;
	}
	grant = want;
	if (s->readonly)
		grant &= ~(uint32_t)(ACCESS3_MODIFY | ACCESS3_EXTEND |
				ACCESS3_DELETE);
	if (a.type != N4M_TYPE_DIR) {
		if (a.flags & UF_IMMUTABLE)
			grant &= ~(uint32_t)(ACCESS3_MODIFY | ACCESS3_EXTEND);
		if (!(a.mode & 0111))
			grant &= ~(uint32_t)ACCESS3_EXECUTE;
	}
	put_post_attr(s, out, &a);
	xdr_put_u32(out, grant);
}

static void p_readlink(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t ref;
	uint32_t st = get_fh(s, in, &ref);
	char path[NFS_MAXPATH + 1];
	size_t len = 0;

	ARGS_OK(in, out);
	if (st == NFS3ERR_NOTDIR)
		st = NFS3ERR_INVAL;
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(n4m_readlink(s->vol, ino_of(ref), path,
				sizeof(path), &len));
	if (st == NFS3_OK && len > NFS_MAXPATH)
		st = NFS3ERR_NAMETOOLONG;
	xdr_put_u32(out, st);
	put_post_attr_ref(s, out, st == NFS3ERR_BADHANDLE ? 0 : ref);
	if (st == NFS3_OK)
		xdr_put_string(out, path);
}

static void p_read(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint64_t off;
	uint32_t count, st = get_fh_any(s, in, &fh);
	size_t got = 0, st_at, count_at, len_at;
	n4m_attr a;
	int err;

	off = xdr_get_u64(in);
	count = xdr_get_u32(in);
	ARGS_OK(in, out);
	if (count > NFS_MAXDATA)
		count = NFS_MAXDATA;
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(attr_of(s, &fh, &a));
	if (st == NFS3_OK && a.type == N4M_TYPE_DIR)
		st = NFS3ERR_ISDIR;
	if (st != NFS3_OK) {
		xdr_put_u32(out, st);
		put_post_attr(s, out, st == NFS3ERR_ISDIR ? &a : NULL);
		return;
	}
	st_at = out->len;
	xdr_put_u32(out, NFS3_OK);
	put_post_attr(s, out, &a);
	count_at = out->len;
	xdr_put_u32(out, 0);		/* count */
	xdr_put_bool(out, false);	/* eof */
	len_at = out->len;
	xdr_put_u32(out, 0);		/* data length */
	if (!xdr_reserve(out, (size_t)count + 4))
		return;
	if (fh.ad)
		err = n4m_stream_read(s->vol, ino_of(fh.ref), AD, off, count,
				out->p + out->len, &got);
	else
		err = n4m_read(s->vol, a.ino, off, count, out->p + out->len,
				&got);
	if (err && !got) {
		/* rewrite as an error reply */
		out->len = st_at;
		xdr_put_u32(out, nfs_errno_to_stat(err));
		put_post_attr(s, out, &a);
		return;
	}
	out->len += got;
	while (out->len & 3)
		out->p[out->len++] = 0;
	xdr_set_u32(out, count_at, (uint32_t)got);
	xdr_set_u32(out, count_at + 4, off + got >= a.size);
	xdr_set_u32(out, len_at, (uint32_t)got);
}

static void p_write(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint64_t off;
	uint32_t count, stable, st = get_fh_any(s, in, &fh);
	const uint8_t *data;
	size_t len = 0, written = 0;
	struct pre_attr pre = { 0 };
	n4m_attr a;

	off = xdr_get_u64(in);
	count = xdr_get_u32(in);
	stable = xdr_get_u32(in);
	data = xdr_get_opaque(in, NFS_MAXDATA, &len);
	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(attr_of(s, &fh, &a));
	if (st == NFS3_OK) {
		pre_from(&a, &pre);
		if (len < count)
			count = (uint32_t)len;
		if (fh.ad)
			st = nfs_errno_to_stat(n4m_stream_write(s->vol,
				ino_of(fh.ref), AD, off, count, data,
				&written));
		else
			st = nfs_errno_to_stat(n4m_write(s->vol, a.ino, off,
				count, data, &written));
		if (written)
			s->dirty = true;
		if (st == NFS3_OK && stable != UNSTABLE) {
			if (!fh.ad)
				n4m_close_write(s->vol, a.ino);
			if (n4m_sync(s->vol))
				st = NFS3ERR_IO;
		}
	}
	xdr_put_u32(out, st);
	put_wcc_fh(s, out, &pre, &fh);
	if (st == NFS3_OK) {
		xdr_put_u32(out, (uint32_t)written);
		xdr_put_u32(out, stable == UNSTABLE ? UNSTABLE : FILE_SYNC);
		xdr_put_fixed(out, s->writeverf, 8);
	}
}

/* Reply for CREATE, MKDIR and SYMLINK */
static void put_create_res(struct nfs_server *s, struct xdr_out *out,
		uint32_t st, const struct fh *fh, const n4m_attr *a,
		const struct pre_attr *pre, uint64_t dir)
{
	xdr_put_u32(out, st);
	if (st == NFS3_OK) {
		xdr_put_bool(out, true);
		put_fh(s, out, fh);
		put_post_attr(s, out, a);
	}
	put_wcc(s, out, pre, dir);
}

/* CREATE of "._foo" while foo exists: make the metadata stream. */
static int create_ad(struct nfs_server *s, uint64_t dir, const char *name,
		uint32_t how, const struct sattr *sa, struct fh *fh,
		n4m_attr *a)
{
	n4m_attr base;
	uint64_t size = 0;
	bool exists;
	int err;

	err = ad_lookup(s, dir, name, &base, false, &size);
	if (err)
		return err;
	exists = !n4m_stream_size(s->vol, base.ino, AD, &size);
	if (exists && how != UNCHECKED)
		return EEXIST;
	if (!exists || (how == UNCHECKED && sa->set_size)) {
		size = how == UNCHECKED && sa->set_size ? sa->size : 0;
		err = n4m_stream_truncate(s->vol, base.ino, AD, size);
		if (err)
			return err;
	}
	*fh = (struct fh){ .ref = base.ref, .ad = true };
	ad_fill(&base, size, a);
	return 0;
}

static void p_create(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t dir;
	uint32_t st = get_fh(s, in, &dir), how;
	char name[NFS_MAXNAME + 1];
	bool name_ok = xdr_get_string(in, name, sizeof(name));
	struct sattr sa = { 0 };
	uint8_t verf[8] = { 0 };
	struct pre_attr pre = { 0 };
	struct fh fh = { 0 };
	struct timespec vt;
	n4m_attr a;
	int err;

	how = xdr_get_u32(in);
	if (how == EXCLUSIVE)
		xdr_get_fixed(in, verf, 8);
	else
		get_sattr(in, &sa);
	ARGS_OK(in, out);
	if (st == NFS3_OK && !name_ok)
		st = NFS3ERR_INVAL;
	if (st != NFS3_OK) {
		put_create_res(s, out, st, NULL, NULL, NULL, 0);
		return;
	}
	get_pre(s, dir, &pre);

	if (ad_base(name)) {
		err = create_ad(s, dir, name, how, &sa, &fh, &a);
		if (err != ENOENT) {
			put_create_res(s, out, nfs_errno_to_stat(err), &fh,
				&a, &pre, dir);
			return;
		}
		/* foo does not exist: fall back to a real "._foo" file */
	}

	/* the exclusive create verifier is kept in the access time */
	vt.tv_sec = (time_t)((uint32_t)verf[0] << 24 | (uint32_t)verf[1] << 16 |
			(uint32_t)verf[2] << 8 | verf[3]);
	vt.tv_nsec = (long)(((uint32_t)verf[4] << 24 | (uint32_t)verf[5] << 16 |
			(uint32_t)verf[6] << 8 | verf[7]) % 1000000000u);

	err = n4m_lookup(s->vol, ino_of(dir), name, &a);
	if (!err) {
		if (how == GUARDED)
			err = EEXIST;
		else if (how == EXCLUSIVE)
			err = (a.atime.tv_sec == vt.tv_sec &&
				a.atime.tv_nsec / 100 == vt.tv_nsec / 100) ?
				0 : EEXIST;
		else if (a.type != N4M_TYPE_FILE)
			err = EEXIST;
		else if (sa.set_size)
			err = apply_sattr(s, &a, &sa);
	} else if (err == ENOENT) {
		err = n4m_create(s->vol, ino_of(dir), name, N4M_TYPE_FILE,
				sa.set_mode ? sa.mode : 0644, &a);
		if (!err && how == EXCLUSIVE) {
			n4m_setattr_req r = { .mask = N4M_SET_ATIME,
				.atime = vt };

			err = n4m_setattr(s->vol, a.ino, &r, NULL);
		} else if (!err) {
			sa.set_uid = sa.set_gid = false;
			err = apply_sattr(s, &a, &sa);
		}
	}
	if (!err)
		err = n4m_getattr(s->vol, a.ino, &a);
	fh.ref = a.ref;
	put_create_res(s, out, nfs_errno_to_stat(err), &fh, &a, &pre, dir);
}

static void p_mkdir(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t dir;
	uint32_t st = get_fh(s, in, &dir);
	char name[NFS_MAXNAME + 1];
	bool name_ok = xdr_get_string(in, name, sizeof(name));
	struct sattr sa;
	struct pre_attr pre = { 0 };
	struct fh fh = { 0 };
	n4m_attr a;
	int err;

	get_sattr(in, &sa);
	ARGS_OK(in, out);
	if (st == NFS3_OK && !name_ok)
		st = NFS3ERR_INVAL;
	if (st != NFS3_OK) {
		put_create_res(s, out, st, NULL, NULL, NULL, 0);
		return;
	}
	get_pre(s, dir, &pre);
	err = n4m_create(s->vol, ino_of(dir), name, N4M_TYPE_DIR, 0755, &a);
	if (!err && (sa.atime_how || sa.mtime_how)) {
		sa.set_mode = sa.set_size = false;
		apply_sattr(s, &a, &sa);
		n4m_getattr(s->vol, a.ino, &a);
	}
	fh.ref = a.ref;
	put_create_res(s, out, nfs_errno_to_stat(err), &fh, &a, &pre, dir);
}

static void p_symlink(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t dir;
	uint32_t st = get_fh(s, in, &dir);
	char name[NFS_MAXNAME + 1], target[NFS_MAXPATH + 1];
	bool name_ok = xdr_get_string(in, name, sizeof(name));
	bool target_ok;
	struct sattr sa;
	struct pre_attr pre = { 0 };
	struct fh fh = { 0 };
	n4m_attr a;
	int err;

	get_sattr(in, &sa);
	target_ok = xdr_get_string(in, target, sizeof(target));
	ARGS_OK(in, out);
	if (st == NFS3_OK && (!name_ok || !target_ok))
		st = NFS3ERR_INVAL;
	if (st != NFS3_OK) {
		put_create_res(s, out, st, NULL, NULL, NULL, 0);
		return;
	}
	get_pre(s, dir, &pre);
	err = n4m_symlink(s->vol, ino_of(dir), name, target, &a);
	fh.ref = a.ref;
	put_create_res(s, out, nfs_errno_to_stat(err), &fh, &a, &pre, dir);
}

static void p_mknod(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t dir;
	uint32_t st = get_fh(s, in, &dir);
	char name[NFS_MAXNAME + 1];

	/* the rest of the arguments do not matter, we do not support it */
	xdr_get_string(in, name, sizeof(name));
	xdr_put_u32(out, RPC_SUCCESS);
	put_create_res(s, out, st == NFS3_OK ? NFS3ERR_NOTSUPP : st, NULL,
			NULL, NULL, st == NFS3_OK ? dir : 0);
}

static void p_remove(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out, bool isdir)
{
	uint64_t dir;
	uint32_t st = get_fh(s, in, &dir);
	char name[NFS_MAXNAME + 1];
	bool name_ok = xdr_get_string(in, name, sizeof(name));
	struct pre_attr pre = { 0 };
	n4m_attr base;

	ARGS_OK(in, out);
	if (st == NFS3_OK && !name_ok)
		st = NFS3ERR_NOENT;
	if (st == NFS3_OK) {
		get_pre(s, dir, &pre);
		if (!isdir && ad_base(name) &&
				!ad_lookup(s, dir, name, &base, true, NULL))
			st = nfs_errno_to_stat(n4m_stream_remove(s->vol,
				base.ino, AD));
		else
			st = nfs_errno_to_stat(n4m_remove(s->vol, ino_of(dir),
				name, isdir));
	}
	xdr_put_u32(out, st);
	put_wcc(s, out, &pre, st == NFS3ERR_BADHANDLE ? 0 : dir);
}

/* Moves the metadata of one file onto another (mv ._a ._b). */
static int move_ad(struct nfs_server *s, const n4m_attr *from,
		const n4m_attr *to)
{
	uint8_t *buf;
	uint64_t size, off = 0;
	int err;

	if (from->ino == to->ino)
		return 0;
	err = n4m_stream_size(s->vol, from->ino, AD, &size);
	if (err)
		return err;
	buf = malloc(1 << 20);
	if (!buf)
		return ENOMEM;
	err = n4m_stream_truncate(s->vol, to->ino, AD, 0);
	while (!err && off < size) {
		size_t got = 0, put = 0;

		err = n4m_stream_read(s->vol, from->ino, AD, off, 1 << 20,
				buf, &got);
		if (!err && !got)
			err = EIO;
		if (!err)
			err = n4m_stream_write(s->vol, to->ino, AD, off, got,
				buf, &put);
		if (!err && put != got)
			err = ENOSPC;	/* never delete the source then */
		off += got;
	}
	free(buf);
	if (!err)
		err = n4m_stream_remove(s->vol, from->ino, AD);
	return err;
}

static void p_rename(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	uint64_t from, to;
	char fname[NFS_MAXNAME + 1], tname[NFS_MAXNAME + 1];
	uint32_t st = get_fh(s, in, &from);
	bool fok = xdr_get_string(in, fname, sizeof(fname));
	uint32_t st2 = get_fh(s, in, &to);
	bool tok = xdr_get_string(in, tname, sizeof(tname));
	struct pre_attr pf = { 0 }, pt = { 0 };
	n4m_attr fb, tb;

	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = st2;
	if (st == NFS3_OK && (!fok || !tok))
		st = NFS3ERR_INVAL;
	if (st == NFS3_OK) {
		get_pre(s, from, &pf);
		get_pre(s, to, &pt);
		if (ad_base(fname) &&
				!ad_lookup(s, from, fname, &fb, true, NULL)) {
			/* source is metadata stored in a stream */
			if (ad_base(tname) &&
					!ad_lookup(s, to, tname, &tb, false, NULL))
				st = nfs_errno_to_stat(move_ad(s, &fb, &tb));
			else
				/* mv falls back to copy and delete */
				st = NFS3ERR_XDEV;
		} else {
			st = nfs_errno_to_stat(n4m_rename(s->vol, ino_of(from),
				fname, ino_of(to), tname));
		}
	}
	xdr_put_u32(out, st);
	put_wcc(s, out, &pf, from);
	put_wcc(s, out, &pt, to);
}

static void p_link(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh file;
	uint64_t dir;
	char name[NFS_MAXNAME + 1];
	uint32_t st = get_fh_any(s, in, &file);
	uint32_t st2 = get_fh(s, in, &dir);
	bool name_ok = xdr_get_string(in, name, sizeof(name));
	struct pre_attr pre = { 0 };
	n4m_attr a;

	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = st2;
	if (st == NFS3_OK && !name_ok)
		st = NFS3ERR_INVAL;
	if (st == NFS3_OK && file.ad)
		st = NFS3ERR_NOTSUPP;
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(getattr_ref(s, file.ref, &a));
	if (st == NFS3_OK) {
		get_pre(s, dir, &pre);
		st = nfs_errno_to_stat(n4m_link(s->vol, a.ino, ino_of(dir),
				name, NULL));
	}
	xdr_put_u32(out, st);
	put_post_attr_fh(s, out, &file);
	put_wcc(s, out, &pre, dir);
}

/*
 * READDIR and READDIRPLUS
 *
 * NTFS keeps a folder as a B+ tree, so positions inside it move around
 * when files are added or removed. Handing those positions to the client
 * as cookies (what ntfs-3g does) can skip or repeat entries when a folder
 * changes between two pages of a listing. Instead the first page takes a
 * snapshot of the whole folder and every following page is served from
 * it: the cookie is simply an index into the snapshot and the cookie
 * verifier names the snapshot. A new listing always starts a new one.
 */
#define SNAP_SLOTS	8
#define SNAP_MAX_ENTRIES (4u * 1024 * 1024)

struct snap_ent {
	char *name;
	uint32_t len;
	uint64_t ino;
};

struct snapshot {
	uint64_t id;		/* the cookie verifier, 0 = free slot */
	uint64_t dir;		/* MFT reference of the folder */
	struct snap_ent *ent;
	size_t count, cap;
	uint64_t used;		/* for LRU */
};

static struct snapshot snaps[SNAP_SLOTS];
static uint64_t snap_clock;

static void snap_free(struct snapshot *sn)
{
	size_t i;

	for (i = 0; i < sn->count; i++)
		free(sn->ent[i].name);
	free(sn->ent);
	memset(sn, 0, sizeof(*sn));
}

static int snap_add(void *ctx, const char *name, size_t namelen,
		uint64_t ino, int type, uint64_t next_cookie,
		const n4m_attr *attr)
{
	struct snapshot *sn = ctx;
	struct snap_ent *e;

	(void)type; (void)next_cookie; (void)attr;
	if (sn->count == SNAP_MAX_ENTRIES)
		return 1;
	if (sn->count == sn->cap) {
		size_t cap = sn->cap ? sn->cap * 2 : 256;
		struct snap_ent *p = realloc(sn->ent, cap * sizeof(*p));

		if (!p)
			return 1;
		sn->ent = p;
		sn->cap = cap;
	}
	e = &sn->ent[sn->count];
	e->name = malloc(namelen + 1);
	if (!e->name)
		return 1;
	memcpy(e->name, name, namelen);
	e->name[namelen] = 0;
	e->len = (uint32_t)namelen;
	e->ino = ino;
	sn->count++;
	return 0;
}

/* Finds the snapshot for (dir, verifier), or takes a fresh one. */
static int snap_get(struct nfs_server *s, uint64_t dir, uint64_t cookie,
		uint64_t verf, struct snapshot **out)
{
	struct snapshot *sn = NULL, *lru = &snaps[0];
	bool eof = false;
	int i, err;

	if (cookie && verf) {
		for (i = 0; i < SNAP_SLOTS; i++)
			if (snaps[i].id == verf && snaps[i].dir == dir) {
				sn = &snaps[i];
				break;
			}
	}
	if (!sn) {
		/* new listing, or an old snapshot we already dropped */
		for (i = 0; i < SNAP_SLOTS; i++) {
			if (!snaps[i].id) {
				lru = &snaps[i];
				break;
			}
			if (snaps[i].used < lru->used)
				lru = &snaps[i];
		}
		snap_free(lru);
		sn = lru;
		err = n4m_readdir(s->vol, ino_of(dir), 0,
				N4M_READDIR_HIDE_PROTECTED, snap_add, sn, &eof);
		if (err) {
			snap_free(sn);
			return err;
		}
		if (!eof && sn->count < SNAP_MAX_ENTRIES) {
			snap_free(sn);
			return ENOMEM;
		}
		sn->dir = dir;
		do
			arc4random_buf(&sn->id, sizeof(sn->id));
		while (!sn->id);
	}
	sn->used = ++snap_clock;
	*out = sn;
	return 0;
}

static size_t pad4(size_t n)
{
	return (n + 3) & ~(size_t)3;
}

static void p_readdir(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out, bool plus)
{
	uint64_t dir, cookie, verf;
	uint8_t vbuf[8];
	uint32_t dircount, maxcount, st = get_fh(s, in, &dir);
	struct snapshot *sn = NULL;
	size_t start, limit, dirused = 0, i;
	n4m_attr a;
	bool have_a = false;
	int err, entries = 0;

	cookie = xdr_get_u64(in);
	xdr_get_fixed(in, vbuf, 8);
	dircount = xdr_get_u32(in);
	maxcount = plus ? xdr_get_u32(in) : dircount;
	ARGS_OK(in, out);
	memcpy(&verf, vbuf, 8);
	if (st == NFS3_OK) {
		st = nfs_errno_to_stat(getattr_ref(s, dir, &a));
		have_a = st == NFS3_OK;
	}
	if (have_a && a.type != N4M_TYPE_DIR)
		st = NFS3ERR_NOTDIR;
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(snap_get(s, dir, cookie, verf, &sn));
	if (st != NFS3_OK) {
		xdr_put_u32(out, st);
		put_post_attr(s, out, have_a ? &a : NULL);
		return;
	}
	if (maxcount > NFS_MAXDATA)
		maxcount = NFS_MAXDATA;
	if (dircount > NFS_MAXDATA)
		dircount = NFS_MAXDATA;
	start = out->len;
	/* maxcount covers the whole reply body, measured from the status */
	limit = start + maxcount;
	xdr_put_u32(out, NFS3_OK);
	put_post_attr(s, out, &a);
	xdr_put_fixed(out, &sn->id, 8);

	for (i = (size_t)cookie; i < sn->count; i++) {
		struct snap_ent *e = &sn->ent[i];
		size_t base = 4 + 8 + 4 + pad4(e->len) + 8;
		size_t need = base + (plus ? 4 + 84 + 4 + 4 + NFS_FHSIZE : 0);
		n4m_attr ea;

		/* keep 8 bytes for the end of list marker and eof */
		if (out->len + need + 8 > limit ||
				(plus && dirused + base > dircount))
			break;
		if (plus) {
			err = n4m_getattr(s->vol, e->ino, &ea);
			if (err == ENOENT || err == ESTALE)
				continue;	/* deleted since the snapshot */
		}
		xdr_put_bool(out, true);
		xdr_put_u64(out, e->ino);
		xdr_put_opaque(out, e->name, e->len);
		xdr_put_u64(out, (uint64_t)i + 1);
		if (plus) {
			put_post_attr(s, out, err ? NULL : &ea);
			xdr_put_bool(out, !err);
			if (!err)
				nfs_put_fh(s, out, ea.ref);
		}
		dirused += base;
		entries++;
	}
	if (!entries && i < sn->count) {
		/* not even one entry fit */
		out->len = start;
		xdr_put_u32(out, NFS3ERR_TOOSMALL);
		put_post_attr(s, out, &a);
		return;
	}
	xdr_put_bool(out, false);	/* no more entries */
	xdr_put_bool(out, i >= sn->count);
}

static void p_fsstat(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);
	n4m_volinfo vi;

	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(n4m_volinfo_get(s->vol, &vi));
	xdr_put_u32(out, st);
	put_post_attr_fh(s, out, st == NFS3_OK ? &fh : NULL);
	if (st != NFS3_OK)
		return;
	xdr_put_u64(out, vi.total_clusters * vi.cluster_size);
	xdr_put_u64(out, vi.free_clusters * vi.cluster_size);
	xdr_put_u64(out, vi.free_clusters * vi.cluster_size);
	xdr_put_u64(out, vi.total_inodes);
	xdr_put_u64(out, vi.free_inodes);
	xdr_put_u64(out, vi.free_inodes);
	xdr_put_u32(out, 0);	/* invarsec */
}

static void p_fsinfo(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);

	ARGS_OK(in, out);
	xdr_put_u32(out, st);
	put_post_attr_fh(s, out, st == NFS3_OK ? &fh : NULL);
	if (st != NFS3_OK)
		return;
	xdr_put_u32(out, NFS_MAXDATA);	/* rtmax */
	xdr_put_u32(out, NFS_MAXDATA);	/* rtpref */
	xdr_put_u32(out, 4096);		/* rtmult */
	xdr_put_u32(out, NFS_MAXDATA);	/* wtmax */
	xdr_put_u32(out, NFS_MAXDATA);	/* wtpref */
	xdr_put_u32(out, 4096);		/* wtmult */
	xdr_put_u32(out, 64 * 1024);	/* dtpref */
	xdr_put_u64(out, 0x7fffffffffffffffULL);
	xdr_put_u32(out, 0);		/* time_delta: 100ns */
	xdr_put_u32(out, 100);
	xdr_put_u32(out, FSF3_LINK | FSF3_SYMLINK | FSF3_HOMOGENEOUS |
			FSF3_CANSETTIME);
}

static void p_pathconf(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);

	ARGS_OK(in, out);
	xdr_put_u32(out, st);
	put_post_attr_fh(s, out, st == NFS3_OK ? &fh : NULL);
	if (st != NFS3_OK)
		return;
	xdr_put_u32(out, 1024);		/* linkmax, NTFS allows 1024 links */
	xdr_put_u32(out, 255);		/* name_max */
	xdr_put_bool(out, true);	/* no_trunc */
	xdr_put_bool(out, true);	/* chown_restricted */
	xdr_put_bool(out, true);	/* case_insensitive, like Windows */
	xdr_put_bool(out, true);	/* case_preserving */
}

static void p_commit(struct nfs_server *s, struct xdr_in *in,
		struct xdr_out *out)
{
	struct fh fh;
	uint32_t st = get_fh_any(s, in, &fh);

	n4m_attr a;

	xdr_get_u64(in);	/* offset */
	xdr_get_u32(in);	/* count */
	ARGS_OK(in, out);
	if (st == NFS3_OK)
		st = nfs_errno_to_stat(attr_of(s, &fh, &a));
	if (st == NFS3_OK && !fh.ad)
		st = nfs_errno_to_stat(n4m_close_write(s->vol,
				ino_of(fh.ref)));
	/*
	 * Data already went to the disk on WRITE. The drive cache is
	 * flushed by the server loop shortly after (and on unmount), doing
	 * a full flush per file would make copying many files very slow.
	 */
	s->dirty = true;
	xdr_put_u32(out, st);
	put_wcc_fh(s, out, NULL, st == NFS3ERR_BADHANDLE ? NULL : &fh);
	if (st == NFS3_OK)
		xdr_put_fixed(out, s->writeverf, 8);
}

void nfs3_dispatch(struct nfs_server *s, uint32_t proc, struct xdr_in *in,
		struct xdr_out *out)
{
	s->requests++;
	switch (proc) {
	case 0:	 xdr_put_u32(out, RPC_SUCCESS); break;	/* NULL */
	case 1:	 p_getattr(s, in, out); break;
	case 2:	 p_setattr(s, in, out); break;
	case 3:	 p_lookup(s, in, out); break;
	case 4:	 p_access(s, in, out); break;
	case 5:	 p_readlink(s, in, out); break;
	case 6:	 p_read(s, in, out); break;
	case 7:	 p_write(s, in, out); break;
	case 8:	 p_create(s, in, out); break;
	case 9:	 p_mkdir(s, in, out); break;
	case 10: p_symlink(s, in, out); break;
	case 11: p_mknod(s, in, out); break;
	case 12: p_remove(s, in, out, false); break;
	case 13: p_remove(s, in, out, true); break;
	case 14: p_rename(s, in, out); break;
	case 15: p_link(s, in, out); break;
	case 16: p_readdir(s, in, out, false); break;
	case 17: p_readdir(s, in, out, true); break;
	case 18: p_fsstat(s, in, out); break;
	case 19: p_fsinfo(s, in, out); break;
	case 20: p_pathconf(s, in, out); break;
	case 21: p_commit(s, in, out); break;
	default: xdr_put_u32(out, RPC_PROC_UNAVAIL); break;
	}
}
