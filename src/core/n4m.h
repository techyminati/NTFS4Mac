/*
 * NTFS4Mac engine public API.
 *
 * This is a small, frontend agnostic layer on top of libntfs-3g. Frontends
 * (the local NFS server, the FSKit module, the CLI) only talk to this API and
 * never touch libntfs-3g directly. Every call is serialized with a per volume
 * lock because libntfs-3g is not thread safe.
 *
 * All functions return 0 on success or a positive errno value on failure.
 */
#ifndef N4M_H
#define N4M_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N4M_VERSION "0.1.0"

/* MFT record number of the NTFS root directory. */
#define N4M_ROOT_INO 5ULL

typedef struct n4m_volume n4m_volume;

/*
 * The block device the engine reads and writes.
 *
 * read and write are always called with offsets and lengths that are
 * multiples of sector_size, so raw disks work. They return 0 or an errno.
 */
typedef struct n4m_blockdev {
	void *ctx;
	uint64_t size;		/* bytes */
	uint32_t sector_size;	/* required I/O alignment, power of two */
	bool readonly;
	int (*read)(void *ctx, void *buf, uint64_t off, size_t len);
	int (*write)(void *ctx, const void *buf, uint64_t off, size_t len);
	int (*flush)(void *ctx);
	void (*close)(void *ctx);
} n4m_blockdev;

/* Opens a disk image, /dev/diskN or /dev/rdiskN as a block device. */
int n4m_blockdev_open_path(const char *path, bool readonly, n4m_blockdev *out);

typedef struct n4m_mount_opts {
	bool readonly;
	/*
	 * Windows "Fast Startup" or hibernation leaves the volume in a state
	 * that is unsafe to write. By default we then mount read only.
	 * Setting this deletes hiberfil.sys (losing the hibernated Windows
	 * session) and mounts read-write.
	 */
	bool remove_hiberfile;
	/*
	 * When Windows crashed or the drive was unplugged, NTFS's journal
	 * still holds unfinished changes and we mount read only. Setting this
	 * throws the journal away and mounts read-write anyway (what ntfs-3g
	 * does by default). Letting Windows repair the drive is safer.
	 */
	bool reset_journal;
	/* Owner reported for every file. */
	uid_t uid;
	gid_t gid;
} n4m_mount_opts;

/* Information about a mounted volume. */
typedef struct n4m_volinfo {
	char label[256];	/* UTF-8 */
	uint64_t serial;	/* NTFS volume serial number */
	uint32_t cluster_size;
	uint64_t total_clusters;
	uint64_t free_clusters;
	uint64_t total_inodes;
	uint64_t free_inodes;
	uint8_t major_ver, minor_ver;
	bool readonly;		/* actually mounted read only */
	bool was_hibernated;	/* forced read only because of hibernation */
	bool was_dirty;		/* journal was not clean, it got reset */
} n4m_volinfo;

enum n4m_type {
	N4M_TYPE_FILE = 1,
	N4M_TYPE_DIR,
	N4M_TYPE_SYMLINK,
	N4M_TYPE_FIFO,
	N4M_TYPE_CHR,
	N4M_TYPE_BLK,
	N4M_TYPE_SOCK,
};

typedef struct n4m_attr {
	uint64_t ino;		/* MFT record number */
	uint64_t ref;		/* MFT reference incl. sequence number */
	uint64_t parent;	/* MFT record of the first parent dir */
	int type;		/* enum n4m_type */
	uint32_t mode;		/* permission bits */
	uint32_t nlink;
	uint32_t uid, gid;
	uint32_t flags;		/* BSD st_flags: UF_HIDDEN, UF_IMMUTABLE */
	uint64_t size;
	uint64_t alloc_size;
	uint64_t rdev;
	uint32_t ntfs_attrib;	/* raw FILE_ATTR_* bits */
	struct timespec atime, mtime, ctime, btime;
} n4m_attr;

enum {
	N4M_SET_SIZE	= 1 << 0,
	N4M_SET_MODE	= 1 << 1,
	N4M_SET_UID	= 1 << 2,
	N4M_SET_GID	= 1 << 3,
	N4M_SET_ATIME	= 1 << 4,
	N4M_SET_MTIME	= 1 << 5,
	N4M_SET_BTIME	= 1 << 6,
	N4M_SET_FLAGS	= 1 << 7,
	N4M_SET_ATIME_NOW = 1 << 8,
	N4M_SET_MTIME_NOW = 1 << 9,
};

typedef struct n4m_setattr_req {
	uint32_t mask;
	uint64_t size;
	uint32_t mode, uid, gid, flags;
	struct timespec atime, mtime, btime;
} n4m_setattr_req;

/* Volume life cycle */
int n4m_mount(n4m_blockdev *dev, const n4m_mount_opts *opts,
		n4m_volume **out);
int n4m_unmount(n4m_volume *vol);
int n4m_volinfo_get(n4m_volume *vol, n4m_volinfo *info);
int n4m_sync(n4m_volume *vol);
int n4m_set_label(n4m_volume *vol, const char *label);
bool n4m_is_readonly(n4m_volume *vol);

/* Probe a device without mounting. Fills label and serial if it is NTFS. */
int n4m_probe(n4m_blockdev *dev, char *label, size_t labelsz,
		uint64_t *serial);

/* Items. ino is always an MFT record number. */
int n4m_getattr(n4m_volume *vol, uint64_t ino, n4m_attr *attr);
int n4m_setattr(n4m_volume *vol, uint64_t ino, const n4m_setattr_req *sa,
		n4m_attr *attr);
int n4m_lookup(n4m_volume *vol, uint64_t dir, const char *name,
		n4m_attr *attr);

/*
 * Directory listing.
 *
 * The callback gets each entry together with the cookie to resume after it.
 * Return nonzero from the callback to stop; the stopped entry is not
 * consumed and will be delivered again when resuming from the previous
 * cookie. Start with cookie 0. "." and ".." are included unless
 * N4M_READDIR_NO_DOTS is set. attr is only filled with N4M_READDIR_ATTRS.
 *
 * The callback runs with the volume locked, it must not call back into
 * the engine.
 */
typedef int (*n4m_dirent_cb)(void *ctx, const char *name, size_t namelen,
		uint64_t ino, int type, uint64_t next_cookie,
		const n4m_attr *attr);
enum {
	/* Skip Windows system folders in the root ($RECYCLE.BIN etc) */
	N4M_READDIR_HIDE_PROTECTED = 1 << 0,
	/* Skip "." and ".." */
	N4M_READDIR_NO_DOTS = 1 << 1,
	/* Fill attributes for every entry */
	N4M_READDIR_ATTRS = 1 << 2,
};
int n4m_readdir(n4m_volume *vol, uint64_t dir, uint64_t cookie, int flags,
		n4m_dirent_cb cb, void *ctx, bool *eof);
/* A number that changes whenever the directory changes. */
int n4m_dir_version(n4m_volume *vol, uint64_t dir, uint64_t *version);

/* Data */
int n4m_read(n4m_volume *vol, uint64_t ino, uint64_t off, size_t len,
		void *buf, size_t *got);
int n4m_write(n4m_volume *vol, uint64_t ino, uint64_t off, size_t len,
		const void *buf, size_t *written);
/* Called when the last writer closes a file (finishes compression). */
int n4m_close_write(n4m_volume *vol, uint64_t ino);

/* Namespace */
int n4m_create(n4m_volume *vol, uint64_t dir, const char *name, int type,
		uint32_t mode, n4m_attr *attr);
int n4m_symlink(n4m_volume *vol, uint64_t dir, const char *name,
		const char *target, n4m_attr *attr);
int n4m_readlink(n4m_volume *vol, uint64_t ino, char *buf, size_t bufsz,
		size_t *len);
int n4m_link(n4m_volume *vol, uint64_t ino, uint64_t dir, const char *name,
		n4m_attr *attr);
/* Removes a file, symlink or empty directory. */
int n4m_remove(n4m_volume *vol, uint64_t dir, const char *name, bool isdir);
int n4m_rename(n4m_volume *vol, uint64_t fromdir, const char *fromname,
		uint64_t todir, const char *toname);

/*
 * Named data streams (NTFS alternate data streams) on a file or folder.
 * Writing or truncating creates the stream when it does not exist yet.
 */
#define N4M_APPLEDOUBLE_STREAM "com.apple.AppleDouble"
int n4m_stream_size(n4m_volume *vol, uint64_t ino, const char *stream,
		uint64_t *size);
int n4m_stream_read(n4m_volume *vol, uint64_t ino, const char *stream,
		uint64_t off, size_t len, void *buf, size_t *got);
int n4m_stream_write(n4m_volume *vol, uint64_t ino, const char *stream,
		uint64_t off, size_t len, const void *buf, size_t *written);
int n4m_stream_truncate(n4m_volume *vol, uint64_t ino, const char *stream,
		uint64_t size);
int n4m_stream_remove(n4m_volume *vol, uint64_t ino, const char *stream);

/* Extended attributes, stored as NTFS alternate data streams. */
int n4m_listxattr(n4m_volume *vol, uint64_t ino, char *buf, size_t bufsz,
		size_t *len);
int n4m_getxattr(n4m_volume *vol, uint64_t ino, const char *name,
		void *buf, size_t bufsz, size_t *len);
enum { N4M_XATTR_CREATE = 1, N4M_XATTR_REPLACE = 2 };
int n4m_setxattr(n4m_volume *vol, uint64_t ino, const char *name,
		const void *buf, size_t len, int flags);
int n4m_removexattr(n4m_volume *vol, uint64_t ino, const char *name);

/* Logging hook, defaults to stderr. level: 0 error, 1 info, 2 debug */
typedef void (*n4m_log_fn)(int level, const char *msg);
void n4m_set_log(n4m_log_fn fn, int max_level);

#ifdef __cplusplus
}
#endif

#endif /* N4M_H */
