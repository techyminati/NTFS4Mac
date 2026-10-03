/*
 * Symbolic links.
 *
 * Reading understands every kind of link found on NTFS:
 *  - native Windows symlinks (relative and absolute)
 *  - directory junctions
 *  - WSL symlinks
 *  - old Interix/SFU symlinks
 * Absolute Windows targets like C:\Users\me are rewritten relative to the
 * volume root (../../Users/me), so they work wherever the drive is mounted.
 *
 * New links get stored as a native Windows symlink when the target is
 * relative (Windows can follow those), and as a WSL symlink when the
 * target is an absolute Mac path (no Windows equivalent exists).
 */
#include <sys/stat.h>

#include "internal.h"

#define SYMLINK_FLAG_RELATIVE 1

struct symlink_data {		/* IO_REPARSE_TAG_SYMLINK */
	le16 subst_name_offset;
	le16 subst_name_length;
	le16 print_name_offset;
	le16 print_name_length;
	le32 flags;
	u8 path_buffer[];
} __attribute__((__packed__));

struct junction_data {		/* IO_REPARSE_TAG_MOUNT_POINT */
	le16 subst_name_offset;
	le16 subst_name_length;
	le16 print_name_offset;
	le16 print_name_length;
	u8 path_buffer[];
} __attribute__((__packed__));

struct wsl_link_data {		/* IO_REPARSE_TAG_LX_SYMLINK */
	le32 type;		/* 2 */
	char link[];
} __attribute__((__packed__));

/* String builder */
struct sbuf {
	char *s;
	size_t len, cap;
	int err;
};

static void sb_add(struct sbuf *b, const char *s, size_t n)
{
	if (b->err)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = (b->len + n + 1) * 2;
		char *p = realloc(b->s, cap);

		if (!p) {
			b->err = ENOMEM;
			return;
		}
		b->s = p;
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = 0;
}

/* Number of directories between the root and ni's parent. */
static int depth_below_root(struct n4m_volume *v, ntfs_inode *ni)
{
	ntfs_attr_search_ctx *ctx;
	u64 p = FILE_root;
	int depth = 0;

	ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx)
		return 0;
	if (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0,
			NULL, 0, ctx)) {
		FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)ctx->attr +
				le16_to_cpu(ctx->attr->value_offset));
		p = MREF_LE(fn->parent_directory);
	}
	ntfs_attr_put_search_ctx(ctx);
	while (p != FILE_root && depth < 512) {
		ntfs_inode *pi = ntfs_inode_open(v->vol, p);

		if (!pi)
			break;
		p = FILE_root;
		ctx = ntfs_attr_get_search_ctx(pi, NULL);
		if (ctx) {
			if (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0,
					CASE_SENSITIVE, 0, NULL, 0, ctx)) {
				FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)
					((u8 *)ctx->attr + le16_to_cpu(
					ctx->attr->value_offset));
				p = MREF_LE(fn->parent_directory);
			}
			ntfs_attr_put_search_ctx(ctx);
		}
		ntfs_inode_close(pi);
		depth++;
	}
	return depth;
}

/* Appends a backslash separated UTF-16 path as a slash separated one. */
static void add_win_path(struct sbuf *b, const ntfschar *p, int len)
{
	int start = 0, i;

	for (i = 0; i <= len; i++) {
		if (i < len && le16_to_cpu(p[i]) != '\\')
			continue;
		if (i > start) {
			char *s = NULL;
			size_t n = 0;

			if (n4m_name_from_ntfs(p + start, i - start, &s, &n)) {
				b->err = ENOMEM;
				return;
			}
			sb_add(b, s, n);
			free(s);
		}
		if (i < len)
			sb_add(b, "/", 1);
		start = i + 1;
	}
}

static bool wstr_prefix(const ntfschar *p, int len, const char *ascii)
{
	int i;

	for (i = 0; ascii[i]; i++)
		if (i >= len || le16_to_cpu(p[i]) != (u16)ascii[i])
			return false;
	return true;
}

/* Turns an absolute Windows target into a path relative to the link. */
static void add_abs_target(struct n4m_volume *v, ntfs_inode *ni,
		struct sbuf *b, const ntfschar *p, int len)
{
	int depth, i;

	if (wstr_prefix(p, len, "\\??\\") || wstr_prefix(p, len, "\\\\?\\")) {
		p += 4;
		len -= 4;
	}
	if (wstr_prefix(p, len, "UNC\\")) {
		sb_add(b, "//", 2);
		add_win_path(b, p + 4, len - 4);
		return;
	}
	if (len >= 2 && le16_to_cpu(p[1]) == ':') {
		p += 2;
		len -= 2;
	} else if (wstr_prefix(p, len, "Volume{")) {
		for (i = 0; i < len && le16_to_cpu(p[i]) != '}'; i++)
			;
		p += i < len ? i + 1 : len;
		len -= i < len ? i + 1 : len;
	} else {
		add_win_path(b, p, len);
		return;
	}
	while (len && le16_to_cpu(p[0]) == '\\') {
		p++;
		len--;
	}
	depth = depth_below_root(v, ni);
	for (i = 0; i < depth; i++)
		sb_add(b, i ? "/.." : "..", i ? 3 : 2);
	if (len) {
		if (depth)
			sb_add(b, "/", 1);
		add_win_path(b, p, len);
	} else if (!depth) {
		sb_add(b, ".", 1);
	}
}

static int read_reparse_link(struct n4m_volume *v, ntfs_inode *ni,
		struct sbuf *b)
{
	REPARSE_POINT *rp;
	s64 size = 0;
	size_t dlen;
	int err = 0;

	rp = ntfs_attr_readall(ni, AT_REPARSE_POINT, NULL, 0, &size);
	if (!rp)
		return n4m_errno();
	dlen = le16_to_cpu(rp->reparse_data_length);
	if ((size_t)size < sizeof(*rp) + dlen) {
		free(rp);
		return EIO;
	}
	if (rp->reparse_tag == IO_REPARSE_TAG_SYMLINK &&
			dlen >= sizeof(struct symlink_data)) {
		struct symlink_data *d = (void *)rp->reparse_data;
		size_t off = le16_to_cpu(d->subst_name_offset);
		size_t n = le16_to_cpu(d->subst_name_length);

		if (sizeof(*d) + off + n > dlen) {
			err = EIO;
		} else {
			const ntfschar *p = (void *)(d->path_buffer + off);

			if (le32_to_cpu(d->flags) & SYMLINK_FLAG_RELATIVE)
				add_win_path(b, p, (int)(n / 2));
			else
				add_abs_target(v, ni, b, p, (int)(n / 2));
		}
	} else if (rp->reparse_tag == IO_REPARSE_TAG_MOUNT_POINT &&
			dlen >= sizeof(struct junction_data)) {
		struct junction_data *d = (void *)rp->reparse_data;
		size_t off = le16_to_cpu(d->subst_name_offset);
		size_t n = le16_to_cpu(d->subst_name_length);

		if (sizeof(*d) + off + n > dlen)
			err = EIO;
		else
			add_abs_target(v, ni, b,
				(void *)(d->path_buffer + off), (int)(n / 2));
	} else if (rp->reparse_tag == IO_REPARSE_TAG_LX_SYMLINK &&
			dlen > sizeof(struct wsl_link_data)) {
		struct wsl_link_data *d = (void *)rp->reparse_data;

		if (le32_to_cpu(d->type) == 2)
			sb_add(b, d->link, dlen - sizeof(*d));
		else
			err = EINVAL;
	} else {
		err = EINVAL;
	}
	free(rp);
	return err;
}

static int read_interix_link(ntfs_inode *ni, struct sbuf *b)
{
	ntfs_attr *na;
	INTX_FILE *intx;
	char *s = NULL;
	size_t n = 0;
	s64 len;
	int err = 0;

	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		return n4m_errno();
	len = na->data_size;
	if (len <= (s64)sizeof(INTX_FILE_TYPES) || len > 65536) {
		ntfs_attr_close(na);
		return EINVAL;
	}
	intx = malloc((size_t)len);
	if (!intx) {
		ntfs_attr_close(na);
		return ENOMEM;
	}
	if (ntfs_attr_pread(na, 0, len, intx) != len)
		err = n4m_errno();
	else if (intx->magic != INTX_SYMBOLIC_LINK)
		err = EINVAL;
	else if (!(err = n4m_utf16_to_utf8(intx->target,
			(int)((len - (s64)offsetof(INTX_FILE, target)) / 2),
			&s, &n)))
		sb_add(b, s, n);
	free(s);
	free(intx);
	ntfs_attr_close(na);
	return err;
}

int n4m_readlink_ni(struct n4m_volume *v, ntfs_inode *ni, char **target)
{
	struct sbuf b = { 0 };
	int err;

	if (ni->flags & FILE_ATTR_REPARSE_POINT)
		err = read_reparse_link(v, ni, &b);
	else if (ni->flags & FILE_ATTR_SYSTEM)
		err = read_interix_link(ni, &b);
	else
		err = EINVAL;
	if (!err)
		err = b.err;
	if (!err && !b.s)
		sb_add(&b, ".", 1);
	if (err || b.err) {
		free(b.s);
		return err ? err : b.err;
	}
	*target = b.s;
	return 0;
}

int n4m_symlink_size(struct n4m_volume *v, ntfs_inode *ni, uint64_t *size)
{
	char *t = NULL;
	int err = n4m_readlink_ni(v, ni, &t);

	*size = err ? 0 : strlen(t);
	free(t);
	return err;
}

int n4m_readlink(n4m_volume *v, uint64_t ino, char *buf, size_t bufsz,
		size_t *len)
{
	ntfs_inode *ni;
	char *t = NULL;
	int err = 0;

	LOCK(v);
	ni = n4m_iopen(v, ino, &err);
	if (ni) {
		err = n4m_readlink_ni(v, ni, &t);
		ntfs_inode_close(ni);
	}
	UNLOCK(v);
	if (err)
		return err;
	*len = strlen(t);
	if (bufsz) {
		size_t n = *len < bufsz - 1 ? *len : bufsz - 1;

		memcpy(buf, t, n);
		buf[n] = 0;
	}
	free(t);
	return 0;
}

/* ---- creating links --------------------------------------------------- */

/*
 * Resolves a relative target from dir to see if it names a directory.
 * Windows needs to know: directory symlinks are directory records.
 */
static bool target_is_dir(struct n4m_volume *v, u64 dir, const char *target)
{
	char *copy = strdup(target), *save = NULL, *comp;
	u64 cur = dir;
	bool isdir = false, ok = true;

	if (!copy)
		return false;
	for (comp = strtok_r(copy, "/", &save); comp && ok;
			comp = strtok_r(NULL, "/", &save)) {
		ntfs_inode *ni;
		u64 mref = 0;
		int err = 0;

		if (!strcmp(comp, "."))
			continue;
		ni = n4m_iopen(v, cur, &err);
		if (!ni) {
			ok = false;
			break;
		}
		if (!strcmp(comp, "..")) {
			if (ni->mft_no == FILE_root) {
				ok = false;	/* leaves the volume */
			} else {
				ntfs_attr_search_ctx *ctx =
					ntfs_attr_get_search_ctx(ni, NULL);

				ok = false;
				if (ctx && !ntfs_attr_lookup(AT_FILE_NAME,
						AT_UNNAMED, 0, CASE_SENSITIVE,
						0, NULL, 0, ctx)) {
					FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)
						((u8 *)ctx->attr + le16_to_cpu(
						ctx->attr->value_offset));
					cur = MREF_LE(fn->parent_directory);
					ok = true;
				}
				if (ctx)
					ntfs_attr_put_search_ctx(ctx);
			}
		} else if (!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) ||
				(ni->flags & FILE_ATTR_REPARSE_POINT)) {
			ok = false;
		} else if (n4m_lookup_ni(v, ni, comp, &mref, NULL, NULL)) {
			ok = false;
		} else {
			cur = MREF(mref);
		}
		ntfs_inode_close(ni);
	}
	if (ok) {
		ntfs_inode *ni;
		int err = 0;

		ni = n4m_iopen(v, cur, &err);
		if (ni) {
			isdir = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) &&
				!(ni->flags & FILE_ATTR_REPARSE_POINT);
			ntfs_inode_close(ni);
		}
	}
	free(copy);
	return isdir;
}

/* Builds an IO_REPARSE_TAG_SYMLINK buffer for a relative target. */
static int build_native_link(const char *target, char **out, size_t *outlen)
{
	ntfschar *path = NULL;
	ntfschar *buf;
	size_t plen = 0, cap = strlen(target) + 1;
	const char *p = target;
	REPARSE_POINT *rp;
	struct symlink_data *d;
	size_t bytes, total;

	path = calloc(cap, sizeof(ntfschar));
	if (!path)
		return ENOMEM;
	/* encode each component, join with backslashes */
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);

		if (n) {
			ntfschar *u = NULL;
			int ulen = 0;
			int err = n4m_name_to_ntfs(p, n, &u, &ulen);

			if (err) {
				free(path);
				return err;
			}
			if (plen + (size_t)ulen + 1 > cap) {
				ntfschar *np;

				cap = (plen + (size_t)ulen + 1) * 2;
				np = realloc(path, cap * sizeof(ntfschar));
				if (!np) {
					free(u);
					free(path);
					return ENOMEM;
				}
				path = np;
			}
			memcpy(path + plen, u, (size_t)ulen * sizeof(ntfschar));
			plen += (size_t)ulen;
			free(u);
		}
		if (!slash)
			break;
		if (plen && le16_to_cpu(path[plen - 1]) != '\\')
			path[plen++] = cpu_to_le16('\\');
		p = slash + 1;
	}
	if (plen > 8000) {
		free(path);
		return ENAMETOOLONG;
	}
	bytes = plen * sizeof(ntfschar);
	total = sizeof(REPARSE_POINT) + sizeof(*d) + 2 * bytes;
	rp = calloc(1, total);
	if (!rp) {
		free(path);
		return ENOMEM;
	}
	rp->reparse_tag = IO_REPARSE_TAG_SYMLINK;
	rp->reparse_data_length = cpu_to_le16((u16)(sizeof(*d) + 2 * bytes));
	d = (struct symlink_data *)rp->reparse_data;
	d->subst_name_offset = cpu_to_le16(0);
	d->subst_name_length = cpu_to_le16((u16)bytes);
	d->print_name_offset = cpu_to_le16((u16)bytes);
	d->print_name_length = cpu_to_le16((u16)bytes);
	d->flags = cpu_to_le32(SYMLINK_FLAG_RELATIVE);
	buf = (ntfschar *)d->path_buffer;
	memcpy(buf, path, bytes);
	memcpy((u8 *)buf + bytes, path, bytes);
	free(path);
	*out = (char *)rp;
	*outlen = total;
	return 0;
}

static int build_wsl_link(const char *target, char **out, size_t *outlen)
{
	size_t n = strlen(target);
	size_t total = sizeof(REPARSE_POINT) + sizeof(struct wsl_link_data) + n;
	REPARSE_POINT *rp;
	struct wsl_link_data *d;

	if (n > 16000)
		return ENAMETOOLONG;
	rp = calloc(1, total);
	if (!rp)
		return ENOMEM;
	rp->reparse_tag = IO_REPARSE_TAG_LX_SYMLINK;
	rp->reparse_data_length = cpu_to_le16((u16)(sizeof(*d) + n));
	d = (struct wsl_link_data *)rp->reparse_data;
	d->type = cpu_to_le32(2);
	memcpy(d->link, target, n);
	*out = (char *)rp;
	*outlen = total;
	return 0;
}

int n4m_symlink(n4m_volume *v, uint64_t dir, const char *name,
		const char *target, n4m_attr *attr)
{
	ntfs_inode *dir_ni = NULL, *ni = NULL;
	ntfschar *uname = NULL;
	char *rp = NULL;
	size_t rplen = 0;
	int ulen, err;
	u64 mref, ino = 0;
	bool isdir = false;

	if (v->readonly)
		return EROFS;
	if (!*target)
		return ENOENT;
	err = n4m_check_name(name, strlen(name));
	if (err)
		return err;
	{
		char *nfc = n4m_normalize(name, strlen(name), 'C');

		err = n4m_name_to_ntfs(nfc ? nfc : name,
				strlen(nfc ? nfc : name), &uname, &ulen);
		free(nfc);
		if (err)
			return err;
	}
	LOCK(v);
	if (target[0] == '/') {
		err = build_wsl_link(target, &rp, &rplen);
	} else {
		isdir = target_is_dir(v, dir, target);
		err = build_native_link(target, &rp, &rplen);
	}
	if (err)
		goto out;
	dir_ni = n4m_iopen(v, dir, &err);
	if (!dir_ni)
		goto out;
	if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		err = ENOTDIR;
		goto out;
	}
	if (dir_ni->mft_no == FILE_Extend) {
		err = EPERM;
		goto out;
	}
	err = n4m_lookup_ni(v, dir_ni, name, &mref, NULL, NULL);
	if (!err) {
		err = EEXIST;
		goto out;
	}
	if (err != ENOENT)
		goto out;
	err = 0;
	ni = ntfs_create(dir_ni, const_cpu_to_le32(0), uname, (u8)ulen,
			isdir ? S_IFDIR : S_IFREG);
	if (!ni) {
		err = n4m_errno();
		goto out;
	}
	if (ntfs_set_ntfs_reparse_data(ni, rp, rplen, 0)) {
		err = n4m_errno();
		/* do not leave a half made link behind, closes both */
		ntfs_delete(v->vol, NULL, ni, dir_ni, uname, (u8)ulen);
		ni = dir_ni = NULL;
		goto out;
	}
	n4m_set_archive(ni);
	ino = ni->mft_no;
	if (ntfs_inode_close_in_dir(ni, dir_ni) && !err)
		err = n4m_errno();
	ni = NULL;
	n4m_touch(v, dir_ni, NTFS_UPDATE_MCTIME);
out:
	if (dir_ni && ntfs_inode_close(dir_ni) && !err)
		err = n4m_errno();
	if (!err && attr) {
		ni = n4m_iopen(v, ino, &err);
		if (ni) {
			err = n4m_fill_attr(v, ni, attr);
			ntfs_inode_close(ni);
		}
	}
	UNLOCK(v);
	free(rp);
	free(uname);
	return err;
}
