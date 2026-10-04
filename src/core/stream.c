/*
 * Named data streams (NTFS "alternate data streams").
 *
 * Used to keep Mac metadata (the AppleDouble data macOS would otherwise
 * write as a separate "._name" file) inside the file it belongs to, where
 * Windows does not show it and it moves along with renames.
 */
#include "internal.h"

static int stream_name(const char *stream, ntfschar **u, int *ulen)
{
	int err = n4m_utf8_to_utf16(stream, strlen(stream), u, ulen);

	if (!err && (*ulen == 0 || *ulen > 255)) {
		free(*u);
		*u = NULL;
		return EINVAL;
	}
	return err;
}

/*
 * Opens a named stream. With create, a missing stream is added first.
 * On success both *nip and *nap are open and must be closed by the caller.
 */
static int stream_open(struct n4m_volume *v, uint64_t ino, const char *stream,
		bool create, ntfs_inode **nip, ntfs_attr **nap)
{
	ntfschar *u = NULL;
	ntfs_inode *ni;
	ntfs_attr *na;
	int ulen = 0, err;

	*nip = NULL;
	*nap = NULL;
	err = stream_name(stream, &u, &ulen);
	if (err)
		return err;
	ni = n4m_iopen(v, ino, &err);
	if (!ni) {
		free(u);
		return err;
	}
	errno = 0;
	na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
	if (!na && errno == ENOENT && create) {
		if (v->readonly) {
			err = EROFS;
		} else if (ntfs_attr_add(ni, AT_DATA, u, (u8)ulen, NULL, 0)) {
			err = n4m_errno();
		} else {
			na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
			if (!na)
				err = n4m_errno();
			else
				n4m_touch(v, ni, NTFS_UPDATE_CTIME);
		}
	} else if (!na) {
		err = n4m_errno();
	}
	free(u);
	if (!na) {
		ntfs_inode_close(ni);
		return err ? err : ENOENT;
	}
	*nip = ni;
	*nap = na;
	return 0;
}

static int stream_close(ntfs_inode *ni, ntfs_attr *na, int err)
{
	if (na)
		ntfs_attr_close(na);
	if (ni && ntfs_inode_close(ni) && !err)
		err = n4m_errno();
	return err;
}

int n4m_stream_size(n4m_volume *v, uint64_t ino, const char *stream,
		uint64_t *size)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	int err;

	LOCK(v);
	err = stream_open(v, ino, stream, false, &ni, &na);
	if (!err) {
		*size = (uint64_t)na->data_size;
		err = stream_close(ni, na, 0);
	}
	UNLOCK(v);
	return err;
}

int n4m_stream_read(n4m_volume *v, uint64_t ino, const char *stream,
		uint64_t off, size_t len, void *buf, size_t *got)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	size_t total = 0;
	int err;

	*got = 0;
	LOCK(v);
	err = stream_open(v, ino, stream, false, &ni, &na);
	if (err)
		goto out;
	if ((s64)off < na->data_size) {
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
	}
	*got = total;
	err = stream_close(ni, na, err);
out:
	UNLOCK(v);
	return err;
}

int n4m_stream_write(n4m_volume *v, uint64_t ino, const char *stream,
		uint64_t off, size_t len, const void *buf, size_t *written)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	size_t total = 0;
	int err;

	*written = 0;
	if (v->readonly)
		return EROFS;
	LOCK(v);
	err = stream_open(v, ino, stream, true, &ni, &na);
	if (err)
		goto out;
	while (total < len) {
		s64 w = ntfs_attr_pwrite(na, (s64)(off + total),
				(s64)(len - total), (const u8 *)buf + total);

		if (w <= 0) {
			err = w < 0 ? n4m_errno() : EIO;
			break;
		}
		total += (size_t)w;
	}
	if (total) {
		err = 0;
		n4m_touch(v, ni, NTFS_UPDATE_CTIME);
	}
	*written = total;
	err = stream_close(ni, na, err);
out:
	UNLOCK(v);
	return err;
}

int n4m_stream_truncate(n4m_volume *v, uint64_t ino, const char *stream,
		uint64_t size)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	int err;

	if (v->readonly)
		return EROFS;
	LOCK(v);
	err = stream_open(v, ino, stream, true, &ni, &na);
	if (!err) {
		if (na->data_size != (s64)size &&
				ntfs_attr_truncate(na, (s64)size))
			err = n4m_errno();
		if (!err)
			n4m_touch(v, ni, NTFS_UPDATE_CTIME);
		err = stream_close(ni, na, err);
	}
	UNLOCK(v);
	return err;
}

int n4m_stream_remove(n4m_volume *v, uint64_t ino, const char *stream)
{
	ntfschar *u = NULL;
	ntfs_inode *ni;
	int ulen = 0, err;

	if (v->readonly)
		return EROFS;
	err = stream_name(stream, &u, &ulen);
	if (err)
		return err;
	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (ni) {
		errno = 0;
		if (!ntfs_attr_exist(ni, AT_DATA, u, (u32)ulen))
			err = ENOENT;
		else if (ntfs_attr_remove(ni, AT_DATA, u, (u32)ulen))
			err = n4m_errno();
		else
			n4m_touch(v, ni, NTFS_UPDATE_CTIME);
		if (ntfs_inode_close(ni) && !err)
			err = n4m_errno();
	}
	UNLOCK(v);
	free(u);
	return err;
}
