/*
 * Creating, linking, removing and renaming.
 *
 * NTFS has no native rename, a rename is "add the new name, then remove
 * the old one" (the same way ntfs-3g does it). When the destination
 * already exists it is first parked under a temporary hidden name and only
 * deleted once the new name is in place, so a failure half way never loses
 * the file that was being replaced.
 */
#include <stdio.h>
#include <sys/stat.h>

#include "internal.h"

/* Same rule ntfs-3g uses to decide a name should be hidden on Windows */
static bool is_dot_name(const ntfschar *u, int len)
{
	return len > 1 && u[0] == const_cpu_to_le16('.') &&
		u[1] != const_cpu_to_le16('.');
}

/* Encodes a name for storing: NFC like Windows, plus character mapping */
static int store_name(const char *name, ntfschar **u, int *ulen)
{
	size_t len = strlen(name);
	char *nfc;
	int err = n4m_check_name(name, len);

	if (err)
		return err;
	nfc = n4m_normalize(name, len, 'C');
	err = n4m_name_to_ntfs(nfc ? nfc : name, nfc ? strlen(nfc) : len,
			u, ulen);
	free(nfc);
	return err;
}

static ntfs_inode *open_dir(struct n4m_volume *v, u64 dir, int *err)
{
	ntfs_inode *ni = n4m_iopen(v, dir, err);

	if (ni && !(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		ntfs_inode_close(ni);
		*err = ENOTDIR;
		return NULL;
	}
	if (ni && (ni->flags & FILE_ATTR_REPARSE_POINT)) {
		/* a directory symlink or junction is not a real directory */
		if (n4m_classify(ni, NULL) == N4M_TYPE_SYMLINK) {
			ntfs_inode_close(ni);
			*err = ENOTDIR;
			return NULL;
		}
	}
	return ni;
}

static int fill_by_ino(struct n4m_volume *v, u64 ino, n4m_attr *attr)
{
	ntfs_inode *ni;
	int err = 0;

	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		return err;
	err = n4m_fill_attr(v, ni, attr);
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
	return err;
}

/* Looks name up in dir. On success *matched is the stored-ish name. */
static int lookup_in(struct n4m_volume *v, u64 dir, const char *name,
		u64 *ino, ntfschar **matched, int *mlen)
{
	ntfs_inode *dir_ni;
	u64 mref = 0;
	int err = 0;

	dir_ni = open_dir(v, dir, &err);
	if (!dir_ni)
		return err;
	err = n4m_lookup_ni(v, dir_ni, name, &mref, matched, mlen);
	if (ntfs_inode_close(dir_ni) && !err)
		err = n4m_errno();
	if (!err)
		*ino = MREF(mref);
	else if (matched && *matched) {
		free(*matched);
		*matched = NULL;
	}
	return err;
}

/*
 * Adds a name for ino in dir. hidden: -1 keeps whatever ntfs-3g decides
 * (dot names get hidden), 0 or 1 forces the Windows hidden attribute.
 */
static int do_link(struct n4m_volume *v, u64 ino, u64 dir,
		const ntfschar *u, int ulen, int hidden)
{
	ntfs_inode *ni, *dir_ni;
	int err = 0;

	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		return err;
	dir_ni = open_dir(v, dir, &err);
	if (!dir_ni) {
		ntfs_inode_close(ni);
		return err;
	}
	errno = 0;
	if (ntfs_link(ni, dir_ni, u, (u8)ulen)) {
		err = n4m_errno();
	} else {
		if (hidden >= 0) {
			le32 old = ni->flags;

			if (hidden)
				ni->flags |= FILE_ATTR_HIDDEN;
			else
				ni->flags &= ~FILE_ATTR_HIDDEN;
			if (old != ni->flags) {
				NInoFileNameSetDirty(ni);
				NInoSetDirty(ni);
			}
		}
		n4m_set_archive(ni);
		n4m_touch(v, ni, NTFS_UPDATE_CTIME);
		n4m_touch(v, dir_ni, NTFS_UPDATE_MCTIME);
	}
	/*
	 * Close the directory first, ni syncs its names into the parent
	 * index on close and the new entry has to be on disk by then.
	 */
	if (ntfs_inode_close(dir_ni) && !err)
		err = n4m_errno();
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
	return err;
}

/* Removes one name of ino from dir. Frees the inode with its last name. */
static int do_unlink(struct n4m_volume *v, u64 dir, const ntfschar *u,
		int ulen, u64 ino)
{
	ntfs_inode *ni, *dir_ni;
	int err = 0;

	dir_ni = open_dir(v, dir, &err);
	if (!dir_ni)
		return err;
	ni = n4m_iopen(v, ino, &err);
	if (!ni) {
		ntfs_inode_close(dir_ni);
		return err;
	}
	errno = 0;
	/* ntfs_delete closes both inodes, even when it fails */
	if (ntfs_delete(v->vol, NULL, ni, dir_ni, u, (u8)ulen))
		err = n4m_errno();
	return err;
}

int n4m_create(n4m_volume *v, uint64_t dir, const char *name, int type,
		uint32_t mode, n4m_attr *attr)
{
	ntfs_inode *dir_ni, *ni;
	ntfschar *u = NULL;
	u64 mref, ino = 0;
	int ulen = 0, err;

	(void)mode;
	if (v->readonly)
		return EROFS;
	if (type != N4M_TYPE_FILE && type != N4M_TYPE_DIR)
		return ENOTSUP;
	err = store_name(name, &u, &ulen);
	if (err)
		return err;
	LOCK(v);
	dir_ni = open_dir(v, dir, &err);
	if (!dir_ni)
		goto out;
	if (dir_ni->mft_no == FILE_Extend) {
		err = EPERM;
		goto close_dir;
	}
	err = n4m_lookup_ni(v, dir_ni, name, &mref, NULL, NULL);
	if (!err) {
		err = EEXIST;
		goto close_dir;
	}
	if (err != ENOENT)
		goto close_dir;
	errno = 0;
	ni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen,
			type == N4M_TYPE_DIR ? S_IFDIR : S_IFREG);
	if (!ni) {
		err = n4m_errno();
		goto close_dir;
	}
	err = 0;
	n4m_set_archive(ni);
	ino = ni->mft_no;
	/* closing ni touches dir_ni's index, so close it through dir_ni */
	if (ntfs_inode_close_in_dir(ni, dir_ni))
		err = n4m_errno();
	n4m_touch(v, dir_ni, NTFS_UPDATE_MCTIME);
close_dir:
	if (ntfs_inode_close(dir_ni) && !err)
		err = n4m_errno();
	if (!err && attr)
		err = fill_by_ino(v, ino, attr);
out:
	UNLOCK(v);
	free(u);
	return err;
}

int n4m_link(n4m_volume *v, uint64_t ino, uint64_t dir, const char *name,
		n4m_attr *attr)
{
	ntfs_inode *ni;
	ntfschar *u = NULL;
	u64 existing;
	int ulen = 0, err, hidden;

	if (v->readonly)
		return EROFS;
	err = store_name(name, &u, &ulen);
	if (err)
		return err;
	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		goto out;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		ntfs_inode_close(ni);
		err = EPERM;
		goto out;
	}
	hidden = is_dot_name(u, ulen) ? 1 :
		(ni->flags & FILE_ATTR_HIDDEN) != 0;
	ntfs_inode_close(ni);
	err = lookup_in(v, dir, name, &existing, NULL, NULL);
	if (!err) {
		err = EEXIST;
		goto out;
	}
	if (err != ENOENT)
		goto out;
	err = do_link(v, ino, dir, u, ulen, hidden);
	if (!err && attr)
		err = fill_by_ino(v, ino, attr);
out:
	UNLOCK(v);
	free(u);
	return err;
}

int n4m_remove(n4m_volume *v, uint64_t dir, const char *name, bool isdir)
{
	ntfs_inode *ni;
	ntfschar *matched = NULL;
	u64 ino = 0;
	int mlen = 0, err, type;

	if (v->readonly)
		return EROFS;
	LOCK(v);
	err = lookup_in(v, dir, name, &ino, &matched, &mlen);
	if (err)
		goto out;
	if (ino == MREF(dir)) {
		err = EINVAL;
		goto out;
	}
	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		goto out;
	type = n4m_classify(ni, NULL);
	if (isdir && type != N4M_TYPE_DIR)
		err = ENOTDIR;
	else if (!isdir && type == N4M_TYPE_DIR)
		err = EPERM;
	else if (isdir && ntfs_check_empty_dir(ni))
		err = errno == ENOTEMPTY ? ENOTEMPTY : n4m_errno();
	ntfs_inode_close(ni);
	if (err)
		goto out;
	err = do_unlink(v, dir, matched, mlen, ino);
out:
	UNLOCK(v);
	free(matched);
	return err;
}

/* true if anc is dir or one of its parents */
static bool is_ancestor(struct n4m_volume *v, u64 anc, u64 dir)
{
	u64 cur = dir;
	int depth;

	for (depth = 0; depth < 1024; depth++) {
		ntfs_inode *ni;
		int err = 0;

		if (cur == anc)
			return true;
		if (cur == FILE_root)
			return false;
		ni = n4m_iopen(v, cur, &err);
		if (!ni)
			return false;
		cur = n4m_parent_of(ni);
		ntfs_inode_close(ni);
	}
	return true;	/* loop in the tree, refuse */
}

static int make_tmp_name(struct n4m_volume *v, u64 ino, ntfschar **u,
		int *ulen)
{
	char buf[64];

	snprintf(buf, sizeof(buf), ".ntfs4mac-rename-%llu-%llu",
		(unsigned long long)++v->tmp_seq, (unsigned long long)ino);
	return n4m_utf8_to_utf16(buf, strlen(buf), u, ulen);
}

static bool same_ignoring_case(struct n4m_volume *v, const ntfschar *a,
		int alen, const ntfschar *b, int blen)
{
	return ntfs_names_are_equal(a, (size_t)alen, b, (size_t)blen,
			IGNORE_CASE, v->vol->upcase, v->vol->upcase_len);
}

int n4m_rename(n4m_volume *v, uint64_t fromdir, const char *fromname,
		uint64_t todir, const char *toname)
{
	ntfschar *uto = NULL, *smatch = NULL, *dmatch = NULL, *tmp = NULL;
	int utolen = 0, smlen = 0, dmlen = 0, tmplen = 0;
	ntfs_inode *ni;
	u64 src = 0, dst = 0;
	int err, derr, stype, dtype, hidden;
	bool old_hidden;

	if (v->readonly)
		return EROFS;
	err = store_name(toname, &uto, &utolen);
	if (err)
		return err;
	LOCK(v);
	err = lookup_in(v, fromdir, fromname, &src, &smatch, &smlen);
	if (err)
		goto out;
	ni = n4m_iopen(v, src, &err);
	if (!ni)
		goto out;
	stype = n4m_classify(ni, NULL);
	old_hidden = (ni->flags & FILE_ATTR_HIDDEN) != 0;
	ntfs_inode_close(ni);

	/* keep a hidden flag set on Windows, unless it came from a dot */
	hidden = is_dot_name(uto, utolen) ? 1 :
		is_dot_name(smatch, smlen) ? 0 : old_hidden;

	derr = lookup_in(v, todir, toname, &dst, &dmatch, &dmlen);
	if (derr && derr != ENOENT) {
		err = derr;
		goto out;
	}
	if (stype == N4M_TYPE_DIR && MREF(todir) != MREF(fromdir) &&
			is_ancestor(v, src, MREF(todir))) {
		err = EINVAL;
		goto out;
	}

	if (!derr && dst == src) {
		/* both names lead to the same file */
		if (MREF(fromdir) != MREF(todir) ||
				!same_ignoring_case(v, smatch, smlen, uto, utolen))
			goto out;	/* two hard links: nothing to do */
		if (smlen == utolen && !memcmp(smatch, uto,
				(size_t)utolen * sizeof(ntfschar)))
			goto out;	/* exactly the same name */
		/*
		 * Case (or Unicode form) only change. The index treats both
		 * names as equal, so go through a temporary name.
		 */
		err = make_tmp_name(v, src, &tmp, &tmplen);
		if (!err)
			err = do_link(v, src, todir, tmp, tmplen, -1);
		if (err)
			goto out;
		err = do_unlink(v, fromdir, smatch, smlen, src);
		if (err) {
			do_unlink(v, todir, tmp, tmplen, src);
			goto out;
		}
		err = do_link(v, src, todir, uto, utolen, hidden);
		if (err) {
			/* put the old name back */
			do_link(v, src, fromdir, smatch, smlen, old_hidden);
			do_unlink(v, todir, tmp, tmplen, src);
			goto out;
		}
		if (do_unlink(v, todir, tmp, tmplen, src))
			n4m_log(0, "rename: could not remove temporary name");
		goto out;
	}

	if (!derr) {
		/* replacing an existing destination */
		ni = n4m_iopen(v, dst, &err);
		if (!ni)
			goto out;
		dtype = n4m_classify(ni, NULL);
		if (stype == N4M_TYPE_DIR && dtype != N4M_TYPE_DIR)
			err = ENOTDIR;
		else if (stype != N4M_TYPE_DIR && dtype == N4M_TYPE_DIR)
			err = EISDIR;
		else if (dtype == N4M_TYPE_DIR && ntfs_check_empty_dir(ni))
			err = errno == ENOTEMPTY ? ENOTEMPTY : n4m_errno();
		ntfs_inode_close(ni);
		if (err)
			goto out;

		err = make_tmp_name(v, dst, &tmp, &tmplen);
		if (err)
			goto out;
		/* 1. park the old destination under a temporary name */
		err = do_link(v, dst, todir, tmp, tmplen, -1);
		if (err)
			goto out;
		/* 2. free the destination name */
		err = do_unlink(v, todir, dmatch, dmlen, dst);
		if (err) {
			do_unlink(v, todir, tmp, tmplen, dst);
			goto out;
		}
		/* 3. give the source its new name */
		err = do_link(v, src, todir, uto, utolen, hidden);
		if (err)
			goto restore;
		/* 4. drop the old source name */
		err = do_unlink(v, fromdir, smatch, smlen, src);
		if (err) {
			do_unlink(v, todir, uto, utolen, src);
			goto restore;
		}
		/* 5. now the old destination can go */
		if (do_unlink(v, todir, tmp, tmplen, dst))
			n4m_log(0, "rename: replaced file left behind as a "
				"hidden temporary file");
		goto out;
restore:
		if (do_link(v, dst, todir, dmatch, dmlen, -1) == 0)
			do_unlink(v, todir, tmp, tmplen, dst);
		else
			n4m_log(0, "rename failed, the replaced file is still "
				"there under a hidden temporary name");
		goto out;
	}

	/* plain move or rename */
	err = do_link(v, src, todir, uto, utolen, hidden);
	if (err)
		goto out;
	err = do_unlink(v, fromdir, smatch, smlen, src);
	if (err)
		do_unlink(v, todir, uto, utolen, src);
out:
	UNLOCK(v);
	free(uto);
	free(smatch);
	free(dmatch);
	free(tmp);
	return err;
}
