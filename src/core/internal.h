/*
 * Internal definitions shared by the engine source files.
 * Only this layer includes libntfs-3g headers.
 */
#ifndef N4M_INTERNAL_H
#define N4M_INTERNAL_H

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "types.h"
#include "attrib.h"
#include "cache.h"
#include "bootsect.h"
#include "device.h"
#include "dir.h"
#include "ea.h"
#include "inode.h"
#include "layout.h"
#include "logging.h"
#include "misc.h"
#include "ntfstime.h"
#include "reparse.h"
#include "security.h"
#include "unistr.h"
#include "volume.h"

#include "n4m.h"

struct n4m_cache;

struct n4m_volume {
	pthread_mutex_t lock;
	ntfs_volume *vol;
	n4m_blockdev bdev;
	struct n4m_cache *cache;
	uid_t uid;
	gid_t gid;
	bool readonly;
	bool was_hibernated;
	bool was_dirty;
	uint64_t serial;
	uint64_t tmp_seq;	/* for temporary names during rename */
	/* inodes whose mtime update is deferred, see file.c */
	uint64_t pending[64];
};

#define LOCK(v)		pthread_mutex_lock(&(v)->lock)
#define UNLOCK(v)	pthread_mutex_unlock(&(v)->lock)

/* errno helper: libntfs-3g sometimes fails without setting errno */
static inline int n4m_errno(void)
{
	return errno ? errno : EIO;
}

/* device.c */
struct ntfs_device *n4m_device_new(struct n4m_volume *v);
void n4m_device_free_cache(struct n4m_volume *v);

/* log.c */
void n4m_log(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void n4m_log_init(void);

/* names.c */

/*
 * Converts a macOS file name (UTF-8) to an NTFS name (UTF-16LE).
 * Characters Windows can not store (: * ? " < > | \ and control chars, plus
 * trailing dots and spaces) are mapped to the Unicode private use area the
 * same way Apple's SMB client and Windows Services for Mac do.
 */
int n4m_name_to_ntfs(const char *name, size_t len, ntfschar **out,
		int *outlen);
/* The reverse of n4m_name_to_ntfs. Result is malloc'ed and NUL terminated. */
int n4m_name_from_ntfs(const ntfschar *name, int len, char **out,
		size_t *outlen);
/* Plain UTF-8 <-> UTF-16LE without any character mapping. */
int n4m_utf8_to_utf16(const char *s, size_t len, ntfschar **out,
		int *outlen);
int n4m_utf16_to_utf8(const ntfschar *s, int len, char **out,
		size_t *outlen);
/*
 * Unicode normalization. form is 'C' or 'D'. Returns a malloc'ed copy or
 * NULL if the string is already in that form (or is not valid UTF-8).
 */
char *n4m_normalize(const char *s, size_t len, char form);
/* Checks a single path component. */
int n4m_check_name(const char *name, size_t len);

/* inode.c */
int n4m_fill_attr(struct n4m_volume *v, ntfs_inode *ni, n4m_attr *attr);
int n4m_lookup_ni(struct n4m_volume *v, ntfs_inode *dir_ni,
		const char *name, u64 *mref, ntfschar **matched,
		int *matched_len);
int n4m_reparse_tag(ntfs_inode *ni, le32 *tag);
int n4m_classify(ntfs_inode *ni, le32 *tag);
/* number of names (hard links) of ni, ignoring DOS 8.3 names */
uint32_t n4m_name_count(ntfs_inode *ni);
uint64_t n4m_parent_of(ntfs_inode *ni);
void n4m_set_archive(ntfs_inode *ni);
void n4m_touch(struct n4m_volume *v, ntfs_inode *ni,
		ntfs_time_update_flags mask);
ntfs_inode *n4m_iopen(struct n4m_volume *v, u64 ino, int *err);

/* symlink.c */
int n4m_readlink_ni(struct n4m_volume *v, ntfs_inode *ni, char **target);
int n4m_symlink_size(struct n4m_volume *v, ntfs_inode *ni, uint64_t *size);

#endif /* N4M_INTERNAL_H */
