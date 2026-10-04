/*
 * File data: read, write, and the "last writer closed" hook.
 */
#include "internal.h"

/* NTFS time is in 100ns units */
#define ONE_SECOND 10000000LL

#ifndef IO_REPARSE_TAG_DEDUP
#define IO_REPARSE_TAG_DEDUP const_cpu_to_le32(0x80000013)
#endif

/*
 * Inodes whose mtime update was deferred by write throttling, flushed by
 * n4m_close_write(). Small on purpose: when full we just stop throttling.
 * Called with the volume locked.
 */
#define PENDING_MAX (int)(sizeof(((struct n4m_volume *)0)->pending) / 8)

static bool pending_add(struct n4m_volume *v, u64 ino)
{
	int i, free_slot = -1;

	for (i = 0; i < PENDING_MAX; i++) {
		if (v->pending[i] == ino)
			return true;
		if (!v->pending[i] && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return false;
	v->pending[free_slot] = ino;
	return true;
}

static bool pending_take(struct n4m_volume *v, u64 ino)
{
	bool found = false;
	int i;

	for (i = 0; i < PENDING_MAX; i++) {
		if (v->pending[i] == ino) {
			v->pending[i] = 0;
			found = true;
		}
	}
	return found;
}

/*
 * Files whose real data lives somewhere we can not decode yet. Reading the
 * plain $DATA stream of those would silently return zeros, so refuse.
 */
static int check_readable(ntfs_inode *ni)
{
	le32 tag = 0;

	if (!(ni->flags & FILE_ATTR_REPARSE_POINT))
		return 0;
	n4m_reparse_tag(ni, &tag);
	if (tag == IO_REPARSE_TAG_WOF || tag == IO_REPARSE_TAG_DEDUP)
		return ENOTSUP;
	if (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT ||
			tag == IO_REPARSE_TAG_LX_SYMLINK)
		return EINVAL;
	return 0;
}

int n4m_read(n4m_volume *v, uint64_t ino, uint64_t off, size_t len,
		void *buf, size_t *got)
{
	ntfs_inode *ni;
	ntfs_attr *na = NULL;
	size_t total = 0;
	int err = 0;

	*got = 0;
	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		goto out;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY &&
			!(ni->flags & FILE_ATTR_REPARSE_POINT)) {
		err = EISDIR;
		goto close;
	}
	err = check_readable(ni);
	if (err)
		goto close;
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		err = n4m_errno();
		goto close;
	}
	if ((s64)off >= na->data_size)
		goto done;
	if ((s64)(off + len) > na->data_size)
		len = (size_t)(na->data_size - (s64)off);
	while (total < len) {
		s64 r = ntfs_attr_pread(na, (s64)(off + total),
				(s64)(len - total), (u8 *)buf + total);

		if (r <= 0) {
			err = r < 0 ? n4m_errno() : EIO;
			break;
		}
		total += (size_t)r;
	}
done:
	/*
	 * Relaxed atime like Linux and ntfs-3g: only bump it when it is
	 * older than the last change, so reads do not cause writes.
	 */
	if (!err && !v->readonly &&
			sle64_to_cpu(ni->last_access_time) <=
			sle64_to_cpu(ni->last_data_change_time))
		n4m_touch(v, ni, NTFS_UPDATE_ATIME);
	*got = total;
close:
	if (na)
		ntfs_attr_close(na);
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
out:
	UNLOCK(v);
	return err;
}

int n4m_write(n4m_volume *v, uint64_t ino, uint64_t off, size_t len,
		const void *buf, size_t *written)
{
	ntfs_inode *ni;
	ntfs_attr *na = NULL;
	size_t total = 0;
	int err = 0;

	*written = 0;
	if (v->readonly)
		return EROFS;
	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		goto out;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		err = EISDIR;
		goto close;
	}
	if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		/* links, compressed system files, cloud placeholders */
		err = ENOTSUP;
		goto close;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		err = n4m_errno();
		goto close;
	}
	while (total < len) {
		s64 w = ntfs_attr_pwrite(na, (s64)(off + total),
				(s64)(len - total),
				(const u8 *)buf + total);

		if (w <= 0) {
			err = w < 0 ? n4m_errno() : EIO;
			break;
		}
		total += (size_t)w;
	}
	if (total) {
		s64 now = sle64_to_cpu(ntfs_current_time());

		n4m_set_archive(ni);
		/*
		 * Updating mtime rewrites the MFT record and the parent's
		 * index entry. During a big copy do that at most once a
		 * second, the final value is set in n4m_close_write().
		 */
		if (now - sle64_to_cpu(ni->last_data_change_time) >= ONE_SECOND
				|| !pending_add(v, ni->mft_no))
			n4m_touch(v, ni, NTFS_UPDATE_MCTIME);
	}
	*written = total;
	if (total)
		err = 0;	/* short write, report what we got */
close:
	if (na)
		ntfs_attr_close(na);
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
out:
	UNLOCK(v);
	return err;
}

int n4m_close_write(n4m_volume *v, uint64_t ino)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	bool touch;
	int err = 0;

	if (v->readonly)
		return 0;
	LOCK(v);
	touch = pending_take(v, ino);
	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		goto out;
	if (!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) &&
			!(ni->flags & FILE_ATTR_REPARSE_POINT)) {
		na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (na) {
			/* compress the last partial block of compressed files */
			if ((na->data_flags & ATTR_COMPRESSION_MASK) &&
					ntfs_attr_pclose(na))
				err = n4m_errno();
			ntfs_attr_close(na);
		}
	}
	if (touch)
		n4m_touch(v, ni, NTFS_UPDATE_MCTIME);
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
out:
	UNLOCK(v);
	return err;
}
