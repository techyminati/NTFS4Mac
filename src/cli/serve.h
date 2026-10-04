/*
 * The long running part of "ntfs4mac mount": owns the NTFS volume, runs
 * the local NFS server and keeps going until the volume is unmounted.
 */
#ifndef N4M_SERVE_H
#define N4M_SERVE_H

#include <stdbool.h>
#include <sys/types.h>

struct serve_opts {
	char source[1024];	/* image file or /dev/rdiskNsM */
	char bsd[64];		/* disk4s1, empty for images */
	char mountpoint[1024];	/* empty: /Volumes/<label> */
	bool readonly;
	bool remove_hiberfile;
	uid_t uid;
	gid_t gid;
	int status_fd;		/* readiness pipe, -1 when in foreground */
	char logfile[1024];	/* empty: stderr */
};

int serve_main(const struct serve_opts *o);

/* "127.0.0.1:/ntfs4mac/<id>", how our mounts show up in mount(8) */
void serve_mount_from(const char *id, char *buf, size_t bufsz);
/* pid file of the server for id */
void serve_pidfile(const char *id, char *buf, size_t bufsz);

#endif /* N4M_SERVE_H */
