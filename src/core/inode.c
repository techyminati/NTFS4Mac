/*
 * Attributes, lookup and directory listing.
 *
 * Rules for using libntfs-3g safely here:
 *  - never have the same inode open twice, close before reopening
 *  - ntfs_delete() closes both inodes it is given
 *  - everything runs under the volume lock
 */
#include <sys/stat.h>

#include "internal.h"

#define DEFAULT_DIR_MODE	0755
#define DEFAULT_FILE_MODE	0644

ntfs_inode *n4m_iopen(struct n4m_volume *v, u64 ino, int *err)
{
	ntfs_inode *ni;

	if (MREF(ino) < FILE_first_user && MREF(ino) != FILE_root) {
		*err = ENOENT;
		return NULL;
	}
	errno = 0;
	ni = ntfs_inode_open(v->vol, MREF(ino));
	if (!ni) {
		*err = n4m_errno();
		if (*err == ENOENT || *err == EIO)
			*err = ESTALE;
		return NULL;
	}
	if (!(ni->mrec->flags & MFT_RECORD_IN_USE) ||
			ni->mrec->base_mft_record) {
		ntfs_inode_close(ni);
		*err = ESTALE;
		return NULL;
	}
	return ni;
}

int n4m_reparse_tag(ntfs_inode *ni, le32 *tag)
{
	REPARSE_POINT *rp;
	s64 size = 0;

	*tag = 0;
	rp = ntfs_attr_readall(ni, AT_REPARSE_POINT, NULL, 0, &size);
	if (!rp)
		return n4m_errno();
	if (size >= (s64)sizeof(le32))
		*tag = rp->reparse_tag;
	free(rp);
	return 0;
}

void n4m_set_archive(ntfs_inode *ni)
{
	if (!(ni->flags & FILE_ATTR_ARCHIVE)) {
		ni->flags |= FILE_ATTR_ARCHIVE;
		NInoFileNameSetDirty(ni);
		NInoSetDirty(ni);
	}
}

void n4m_touch(struct n4m_volume *v, ntfs_inode *ni,
		ntfs_time_update_flags mask)
{
	if (!v->readonly)
		ntfs_inode_update_times(ni, mask);
}

/* Works out what kind of object an inode is. */
static int classify(ntfs_inode *ni, le32 *tagp)
{
	bool isdir = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
	le32 tag = 0;

	if (tagp)
		*tagp = 0;
	if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		n4m_reparse_tag(ni, &tag);
		if (tagp)
			*tagp = tag;
		if (tag == IO_REPARSE_TAG_SYMLINK ||
				tag == IO_REPARSE_TAG_MOUNT_POINT ||
				tag == IO_REPARSE_TAG_LX_SYMLINK)
			return N4M_TYPE_SYMLINK;
		if (tag == IO_REPARSE_TAG_AF_UNIX)
			return N4M_TYPE_SOCK;
		if (tag == IO_REPARSE_TAG_LX_FIFO)
			return N4M_TYPE_FIFO;
		if (tag == IO_REPARSE_TAG_LX_CHR)
			return N4M_TYPE_CHR;
		if (tag == IO_REPARSE_TAG_LX_BLK)
			return N4M_TYPE_BLK;
		/* cloud files, dedup, WOF compression... */
		return isdir ? N4M_TYPE_DIR : N4M_TYPE_FILE;
	}
	if (isdir)
		return N4M_TYPE_DIR;
	/*
	 * Interix (SFU) special files are system files with a magic
	 * header. Only trust the unambiguous magic based ones: ntfs-3g
	 * would also turn every empty system file into a FIFO.
	 */
	if (ni->flags & FILE_ATTR_SYSTEM) {
		switch (ntfs_interix_types(ni)) {
		case NTFS_DT_LNK:
			return N4M_TYPE_SYMLINK;
		case NTFS_DT_BLK:
			return N4M_TYPE_BLK;
		case NTFS_DT_CHR:
			return N4M_TYPE_CHR;
		default:
			break;
		}
	}
	return N4M_TYPE_FILE;
}

/* Counts the hard links (ignoring DOS 8.3 names) and finds a parent. */
static void count_links(ntfs_inode *ni, uint32_t *nlink, uint64_t *parent)
{
	ntfs_attr_search_ctx *ctx;
	uint32_t n = 0;

	*parent = FILE_root;
	ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx) {
		*nlink = le16_to_cpu(ni->mrec->link_count);
		return;
	}
	while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE,
			0, NULL, 0, ctx)) {
		FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)ctx->attr +
				le16_to_cpu(ctx->attr->value_offset));

		if (fn->file_name_type == FILE_NAME_DOS)
			continue;
		if (!n)
			*parent = MREF_LE(fn->parent_directory);
		n++;
	}
	ntfs_attr_put_search_ctx(ctx);
	*nlink = n ? n : 1;
}

int n4m_fill_attr(struct n4m_volume *v, ntfs_inode *ni, n4m_attr *a)
{
	le32 tag;

	memset(a, 0, sizeof(*a));
	a->ino = ni->mft_no;
	a->ref = MK_MREF(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
	a->ntfs_attrib = le32_to_cpu(ni->flags);
	a->uid = v->uid;
	a->gid = v->gid;
	a->type = classify(ni, &tag);
	count_links(ni, &a->nlink, &a->parent);
	if (ni->mft_no == FILE_root)
		a->parent = FILE_root;

	switch (a->type) {
	case N4M_TYPE_DIR:
		a->mode = DEFAULT_DIR_MODE;
		a->nlink = 1;	/* tells find(1) not to count subdirs */
		if (!test_nino_flag(ni, KnownSize)) {
			ntfs_attr *na = ntfs_attr_open(ni,
					AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);

			if (na) {
				ni->data_size = na->data_size;
				ni->allocated_size = na->allocated_size;
				set_nino_flag(ni, KnownSize);
				ntfs_attr_close(na);
			}
		}
		a->size = (uint64_t)ni->data_size;
		a->alloc_size = (uint64_t)ni->allocated_size;
		break;
	case N4M_TYPE_SYMLINK:
		a->mode = 0777;
		n4m_symlink_size(v, ni, &a->size);
		a->alloc_size = (uint64_t)ni->allocated_size;
		break;
	case N4M_TYPE_CHR:
	case N4M_TYPE_BLK:
		if (tag) {
			dev_t rdev = 0;

			if (!ntfs_ea_check_wsldev(ni, &rdev))
				a->rdev = (uint64_t)rdev;
		}
		a->mode = DEFAULT_FILE_MODE;
		break;
	default:
		a->mode = DEFAULT_FILE_MODE;
		a->size = (uint64_t)ni->data_size;
		a->alloc_size = (uint64_t)ni->allocated_size;
		break;
	}

	if (ni->flags & FILE_ATTR_HIDDEN)
		a->flags |= UF_HIDDEN;
	/* Windows "read-only" is the Finder "Locked" checkbox for files */
	if ((ni->flags & FILE_ATTR_READONLY) && a->type != N4M_TYPE_DIR)
		a->flags |= UF_IMMUTABLE;

	a->atime = ntfs2timespec(ni->last_access_time);
	a->mtime = ntfs2timespec(ni->last_data_change_time);
	a->ctime = ntfs2timespec(ni->last_mft_change_time);
	a->btime = ntfs2timespec(ni->creation_time);
	return 0;
}

int n4m_getattr(n4m_volume *v, uint64_t ino, n4m_attr *attr)
{
	ntfs_inode *ni;
	int err = 0;

	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (ni) {
		err = n4m_fill_attr(v, ni, attr);
		if (ntfs_inode_close(ni) && !err)
			err = n4m_errno();
	}
	UNLOCK(v);
	return err;
}

/* ---- lookup ----------------------------------------------------------- */

static u64 lookup_one(ntfs_inode *dir_ni, const ntfschar *u, int ulen,
		bool ignore_case)
{
	ntfs_volume *vol = dir_ni->vol;
	u64 mref;

	if (ignore_case)
		NVolClearCaseSensitive(vol);
	errno = 0;
	mref = ntfs_inode_lookup_by_name(dir_ni, u, ulen);
	if (ignore_case)
		NVolSetCaseSensitive(vol);
	return mref;
}

/*
 * Finds name in dir_ni the way Windows would: case insensitive, and
 * tolerant of NFC vs NFD and of the private use character mapping.
 * An exact match always wins over a case insensitive one.
 */
int n4m_lookup_ni(struct n4m_volume *v, ntfs_inode *dir_ni, const char *name,
		u64 *mref, ntfschar **matched, int *matched_len)
{
	size_t len = strlen(name);
	ntfschar *cand[4] = { NULL };
	int clen[4] = { 0 };
	char *nfc, *nfd;
	int n = 0, i, pass, err;
	u64 m = (u64)-1;

	(void)v;
	err = n4m_check_name(name, len);
	if (err)
		return err == EINVAL ? ENOENT : err;

	err = n4m_name_to_ntfs(name, len, &cand[n], &clen[n]);
	if (err)
		return err == EILSEQ ? ENOENT : err;
	n++;
	nfc = n4m_normalize(name, len, 'C');
	nfd = n4m_normalize(name, len, 'D');
	if (nfc && !n4m_name_to_ntfs(nfc, strlen(nfc), &cand[n], &clen[n]))
		n++;
	if (nfd && !n4m_name_to_ntfs(nfd, strlen(nfd), &cand[n], &clen[n]))
		n++;
	free(nfc);
	free(nfd);
	/* names written by Linux tools may contain ':' and friends as is */
	if (!n4m_utf8_to_utf16(name, len, &cand[n], &clen[n])) {
		if (clen[n] == clen[0] && !memcmp(cand[n], cand[0],
				(size_t)clen[0] * sizeof(ntfschar))) {
			free(cand[n]);
			cand[n] = NULL;
		} else {
			n++;
		}
	}

	err = ENOENT;
	for (pass = 0; pass < 2 && m == (u64)-1; pass++) {
		for (i = 0; i < n; i++) {
			m = lookup_one(dir_ni, cand[i], clen[i], pass == 1);
			if (m != (u64)-1)
				break;
			if (errno && errno != ENOENT) {
				err = errno;
				pass = 2;
				break;
			}
		}
	}
	if (m != (u64)-1 && (MREF(m) < FILE_first_user)) {
		/* never expose $MFT, $Bitmap, ... */
		m = (u64)-1;
		err = ENOENT;
	}
	if (m != (u64)-1) {
		*mref = m;
		err = 0;
		if (matched) {
			*matched = cand[i];
			*matched_len = clen[i];
			cand[i] = NULL;
		}
	}
	for (i = 0; i < 4; i++)
		free(cand[i]);
	return err;
}

static uint64_t parent_of(ntfs_inode *ni)
{
	uint32_t nlink;
	uint64_t parent;

	if (ni->mft_no == FILE_root)
		return FILE_root;
	count_links(ni, &nlink, &parent);
	return parent;
}

int n4m_lookup(n4m_volume *v, uint64_t dir, const char *name, n4m_attr *attr)
{
	ntfs_inode *dir_ni, *ni;
	u64 mref = 0;
	int err = 0;

	LOCK(v);
	dir_ni = n4m_iopen(v, dir, &err);
	if (!dir_ni)
		goto out;
	if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		ntfs_inode_close(dir_ni);
		err = ENOTDIR;
		goto out;
	}
	if (!strcmp(name, "."))
		mref = dir_ni->mft_no;
	else if (!strcmp(name, ".."))
		mref = parent_of(dir_ni);
	else
		err = n4m_lookup_ni(v, dir_ni, name, &mref, NULL, NULL);
	if (ntfs_inode_close(dir_ni) && !err)
		err = n4m_errno();
	if (err)
		goto out;
	ni = n4m_iopen(v, MREF(mref), &err);
	if (!ni)
		goto out;
	err = n4m_fill_attr(v, ni, attr);
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
out:
	UNLOCK(v);
	return err;
}

/* ---- readdir ---------------------------------------------------------- */

#define RD_BATCH 128

struct rd_entry {
	char *name;
	size_t len;
	u64 mref;
	unsigned dt;
	s64 pos;
};

struct rd_ctx {
	struct rd_entry ent[RD_BATCH];
	int count;
	int flags;
	bool full;
	bool is_root;
	int err;
};

static const char *const protected_names[] = {
	"$RECYCLE.BIN", "$Recycle.Bin", "System Volume Information",
	"RECYCLER", "$WinREAgent", "$SysReset", "$Windows.~BT",
	"$Windows.~WS", "$GetCurrent", "Config.Msi", "Recovery",
	"hiberfil.sys", "pagefile.sys", "swapfile.sys", "DumpStack.log",
	"DumpStack.log.tmp", NULL,
};

static bool is_protected(const char *name)
{
	int i;

	for (i = 0; protected_names[i]; i++)
		if (!strcmp(name, protected_names[i]))
			return true;
	return false;
}

static int rd_filler(void *dirent, const ntfschar *name, const int name_len,
		const int name_type, const s64 pos, const MFT_REF mref,
		const unsigned dt_type)
{
	struct rd_ctx *rd = dirent;
	struct rd_entry *e;
	char *s = NULL;
	size_t len = 0;
	bool dot;

	if (name_type == FILE_NAME_DOS)
		return 0;
	if (MREF(mref) <= 1)
		return 0;
	dot = pos < 2;
	if (dot && (rd->flags & N4M_READDIR_NO_DOTS))
		return 0;
	if (rd->count == RD_BATCH) {
		rd->full = true;
		return -1;
	}
	if (dot) {
		s = strdup(pos == 0 ? "." : "..");
		len = (size_t)pos + 1;
		if (!s)
			rd->err = ENOMEM;
	} else {
		rd->err = n4m_name_from_ntfs(name, name_len, &s, &len);
	}
	if (rd->err)
		return -1;
	if (!dot && rd->is_root && (rd->flags & N4M_READDIR_HIDE_PROTECTED) &&
			is_protected(s)) {
		free(s);
		return 0;
	}
	e = &rd->ent[rd->count++];
	e->name = s;
	e->len = len;
	e->mref = mref;
	e->dt = dt_type;
	e->pos = pos;
	return 0;
}

static int dt_to_type(unsigned dt)
{
	switch (dt) {
	case NTFS_DT_DIR:
		return N4M_TYPE_DIR;
	case NTFS_DT_LNK:
		return N4M_TYPE_SYMLINK;
	case NTFS_DT_REG:
		return N4M_TYPE_FILE;
	default:
		return 0;	/* needs a closer look */
	}
}

int n4m_readdir(n4m_volume *v, uint64_t dir, uint64_t cookie, int flags,
		n4m_dirent_cb cb, void *cbctx, bool *eof)
{
	struct rd_ctx *rd;
	ntfs_inode *dir_ni, *ni;
	bool done = false, stopped = false;
	int err = 0, i;

	*eof = false;
	rd = calloc(1, sizeof(*rd));
	if (!rd)
		return ENOMEM;
	LOCK(v);
	while (!done && !stopped && !err) {
		s64 pos = (s64)cookie;
		bool at_end;

		dir_ni = n4m_iopen(v, dir, &err);
		if (!dir_ni)
			break;
		if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
			ntfs_inode_close(dir_ni);
			err = ENOTDIR;
			break;
		}
		rd->count = 0;
		rd->full = false;
		rd->flags = flags;
		rd->err = 0;
		rd->is_root = dir_ni->mft_no == FILE_root;
		errno = 0;
		if (ntfs_readdir(dir_ni, &pos, rd, rd_filler) && !rd->full)
			err = rd->err ? rd->err : n4m_errno();
		at_end = !rd->full;
		if (ntfs_inode_close(dir_ni) && !err)
			err = n4m_errno();

		/* the directory is closed now, safe to open children */
		for (i = 0; i < rd->count && !err && !stopped; i++) {
			struct rd_entry *e = &rd->ent[i];
			n4m_attr attr, *ap = NULL;
			int type = dt_to_type(e->dt);
			int r;

			if (!type || (flags & N4M_READDIR_ATTRS)) {
				int ierr = 0;

				ni = n4m_iopen(v, MREF(e->mref), &ierr);
				if (!ni) {
					if (!type)
						type = N4M_TYPE_FILE;
				} else {
					n4m_fill_attr(v, ni, &attr);
					ntfs_inode_close(ni);
					type = attr.type;
					if (flags & N4M_READDIR_ATTRS)
						ap = &attr;
				}
			}
			r = cb(cbctx, e->name, e->len, MREF(e->mref), type,
					(uint64_t)e->pos + 1, ap);
			if (r)
				stopped = true;
			else
				cookie = (uint64_t)e->pos + 1;
		}
		for (i = 0; i < rd->count; i++)
			free(rd->ent[i].name);
		if (at_end && !stopped && !err)
			done = true;
	}
	UNLOCK(v);
	free(rd);
	if (!err && done)
		*eof = true;
	return err;
}

int n4m_dir_version(n4m_volume *v, uint64_t dir, uint64_t *version)
{
	ntfs_inode *ni;
	int err = 0;

	LOCK(v);
	ni = n4m_iopen(v, dir, &err);
	if (ni) {
		*version = (uint64_t)sle64_to_cpu(ni->last_data_change_time);
		ntfs_inode_close(ni);
	}
	UNLOCK(v);
	return err;
}

/* ---- setattr ---------------------------------------------------------- */

static int truncate_ni(ntfs_inode *ni, uint64_t size)
{
	ntfs_attr *na;
	int err = 0;

	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		return n4m_errno();
	/*
	 * For compressed files growing is done by writing a final zero,
	 * which ntfs-3g turns into a hole when it can (same as ntfs-3g).
	 */
	if ((na->data_flags & ATTR_COMPRESSION_MASK) &&
			(s64)size > na->initialized_size) {
		char zero = 0;

		if (ntfs_attr_pwrite(na, (s64)size - 1, 1, &zero) <= 0)
			err = n4m_errno();
	} else if (ntfs_attr_truncate(na, (s64)size)) {
		err = n4m_errno();
	}
	ntfs_attr_close(na);
	return err;
}

int n4m_setattr(n4m_volume *v, uint64_t ino, const n4m_setattr_req *sa,
		n4m_attr *attr)
{
	ntfs_inode *ni;
	ntfs_time_update_flags mask = 0;
	bool isdir, changed = false;
	int err = 0;

	if (v->readonly && (sa->mask & ~(N4M_SET_UID | N4M_SET_GID)))
		return EROFS;
	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (!ni)
		goto out;
	isdir = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;

	if ((sa->mask & N4M_SET_SIZE) && !isdir &&
			!(ni->flags & FILE_ATTR_REPARSE_POINT)) {
		if ((uint64_t)ni->data_size != sa->size) {
			err = truncate_ni(ni, sa->size);
			if (err)
				goto close;
			n4m_set_archive(ni);
		}
		mask |= NTFS_UPDATE_MCTIME;
	}
	if (sa->mask & N4M_SET_FLAGS) {
		le32 old = ni->flags;

		if (sa->flags & UF_HIDDEN)
			ni->flags |= FILE_ATTR_HIDDEN;
		else
			ni->flags &= ~FILE_ATTR_HIDDEN;
		if (!isdir) {
			if (sa->flags & (UF_IMMUTABLE | SF_IMMUTABLE))
				ni->flags |= FILE_ATTR_READONLY;
			else
				ni->flags &= ~FILE_ATTR_READONLY;
		}
		if (ni->flags != old) {
			NInoFileNameSetDirty(ni);
			NInoSetDirty(ni);
			mask |= NTFS_UPDATE_CTIME;
		}
	}
	if (sa->mask & N4M_SET_ATIME_NOW) {
		mask |= NTFS_UPDATE_ATIME;
	} else if (sa->mask & N4M_SET_ATIME) {
		ni->last_access_time = timespec2ntfs(sa->atime);
		changed = true;
	}
	if (sa->mask & N4M_SET_MTIME_NOW) {
		mask |= NTFS_UPDATE_MTIME;
	} else if (sa->mask & N4M_SET_MTIME) {
		ni->last_data_change_time = timespec2ntfs(sa->mtime);
		changed = true;
	}
	if (sa->mask & N4M_SET_BTIME) {
		ni->creation_time = timespec2ntfs(sa->btime);
		changed = true;
	}
	if (changed)
		mask |= NTFS_UPDATE_CTIME;
	if (mask)
		n4m_touch(v, ni, mask);
	if (changed) {
		/* update_times may skip system files, make sure it sticks */
		NInoFileNameSetDirty(ni);
		NInoSetDirty(ni);
	}
	/* mode, uid and gid are accepted and ignored for now */
	if (attr)
		err = n4m_fill_attr(v, ni, attr);
close:
	if (ntfs_inode_close(ni) && !err)
		err = n4m_errno();
out:
	UNLOCK(v);
	return err;
}
