/*
 * DiskArbitration helpers for the command line tool.
 */
#ifndef N4M_DISK_H
#define N4M_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct diskinfo {
	char bsd[64];		/* disk4s1 */
	char label[256];
	char kind[32];		/* volume kind as macOS sees it, "ntfs" */
	char content[64];	/* partition type */
	char mountpoint[1024];	/* empty when not mounted */
	char fstype[32];	/* what it is mounted with right now */
	uint64_t size;
	bool removable;
	bool internal;
};

/* Fills info for one BSD name. Returns 0 or an errno. */
int disk_describe(const char *bsd, struct diskinfo *info);

/* Calls cb for every partition that looks like NTFS. */
int disk_list_ntfs(void (*cb)(const struct diskinfo *info, void *ctx),
		void *ctx);

/* Unmounts whatever is mounted from this partition. */
int disk_unmount(const char *bsd, bool force);

/* Ejects the whole disk the partition belongs to. */
int disk_eject(const char *bsd);

/* "disk4s1", "/dev/disk4s1", "/dev/rdisk4s1" -> "disk4s1" */
bool disk_normalize_name(const char *in, char *out, size_t outsz);

#endif /* N4M_DISK_H */
