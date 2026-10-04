/*
 * Runs one mounted volume: engine + NFS server + mount_nfs.
 *
 * Important rule: this process must never make a system call that goes
 * through its own NFS mount (statfs, unmount, open...). The kernel would
 * send the request to us and wait for an answer we can not give while we
 * are blocked. Mount checks therefore use getfsstat(MNT_NOWAIT) and
 * unmounting is done by a child umount process.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "disk.h"
#include "n4m.h"
#include "nfs.h"
#include "serve.h"

extern char **environ;

static volatile sig_atomic_t stop_requested;
static FILE *logf;

static void logmsg(const char *fmt, ...)
{
	char ts[32];
	time_t t = time(NULL);
	va_list ap;
	FILE *f = logf ? logf : stderr;

	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
	fprintf(f, "%s [%d] ", ts, getpid());
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

static void engine_log(int level, const char *msg)
{
	logmsg("%s%s", level == 0 ? "error: " : "", msg);
}

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

void serve_mount_from(const char *id, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "127.0.0.1:/ntfs4mac/%s", id);
}

void serve_pidfile(const char *id, char *buf, size_t bufsz)
{
	const char *dir = geteuid() == 0 ? "/var/run" : getenv("TMPDIR");

	snprintf(buf, bufsz, "%s/ntfs4mac-%s.pid", dir ? dir : "/tmp", id);
}

static void report(int fd, const char *fmt, ...)
{
	char buf[1200];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		logmsg("%s", buf);
	if (fd >= 0) {
		if (n > 0)
			write(fd, buf, strnlen(buf, sizeof(buf)));
		write(fd, "\n", 1);
	}
}

/* Is the NFS mount with this source still there? Never blocks. */
static bool still_mounted(const char *from, const char *mp)
{
	struct statfs *mnt;
	int n, i;

	n = getmntinfo(&mnt, MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (!strcmp(mnt[i].f_mntfromname, from) &&
				!strcmp(mnt[i].f_mntonname, mp))
			return true;
	return false;
}

static bool is_mountpoint(const char *path)
{
	struct statfs *mnt;
	int n, i;

	n = getmntinfo(&mnt, MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (!strcmp(mnt[i].f_mntonname, path))
			return true;
	return false;
}

/* /Volumes/<label>, with " 1", " 2"... when the name is taken */
static void pick_mountpoint(const char *label, char *buf, size_t bufsz)
{
	char name[256];
	size_t i;
	int n;

	snprintf(name, sizeof(name), "%s", label[0] ? label : "NTFS");
	for (i = 0; name[i]; i++)
		if (name[i] == '/')
			name[i] = ':';
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		snprintf(name, sizeof(name), "NTFS");
	snprintf(buf, bufsz, "/Volumes/%s", name);
	for (n = 1; n < 100; n++) {
		struct stat st;

		if (lstat(buf, &st) && errno == ENOENT)
			return;
		if (S_ISDIR(st.st_mode) && !is_mountpoint(buf) &&
				rmdir(buf) == 0)
			return;	/* stale empty folder */
		snprintf(buf, bufsz, "/Volumes/%s %d", name, n);
	}
}

static pid_t spawn(char *const argv[])
{
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int err;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	if (logf) {
		posix_spawn_file_actions_adddup2(&fa, fileno(logf), 1);
		posix_spawn_file_actions_adddup2(&fa, fileno(logf), 2);
	}
	err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (err) {
		errno = err;
		return -1;
	}
	return pid;
}

/*
 * After the user ejected our volume in Finder, eject the whole drive too so
 * it is gone from Disk Utility and can be unplugged. Only when no other
 * partition of that drive is still mounted.
 */
static void eject_if_idle(const char *bsd)
{
	char whole[64], dev[80], ours[96];
	struct statfs *mnt;
	size_t wl;
	int n, i, err;

	snprintf(whole, sizeof(whole), "%s", bsd);
	whole[strcspn(whole + 4, "s") + 4] = 0;	/* disk4s1 -> disk4 */
	snprintf(dev, sizeof(dev), "/dev/%ss", whole);
	snprintf(ours, sizeof(ours), "127.0.0.1:/ntfs4mac/%ss", whole);
	wl = strlen(dev);
	n = getmntinfo(&mnt, MNT_NOWAIT);
	for (i = 0; i < n; i++) {
		if (!strncmp(mnt[i].f_mntfromname, dev, wl) ||
				!strncmp(mnt[i].f_mntfromname, ours,
				strlen(ours))) {
			logmsg("not ejecting %s, %s is still mounted", whole,
				mnt[i].f_mntonname);
			return;
		}
	}
	err = disk_eject(bsd);
	logmsg(err ? "could not eject %s: %s" : "ejected %s%s", whole,
		err ? strerror(err) : "");
}

enum phase { MOUNTING, MOUNTED, UNMOUNTING, DONE };

struct state {
	const struct serve_opts *o;
	struct nfs_server *srv;
	char from[1200];
	char mp[1024];
	int status_fd;
	enum phase phase;
	pid_t mount_pid, umount_pid;
	uint64_t started_ms, umount_ms, last_check_ms;
	bool failed;
	bool forced;
	bool hibernated;
};

static uint64_t now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static void start_umount(struct state *st, bool force)
{
	char *argv[4];
	int i = 0;

	argv[i++] = "/sbin/umount";
	if (force)
		argv[i++] = "-f";
	argv[i++] = st->mp;
	argv[i] = NULL;
	st->umount_pid = spawn(argv);
	st->umount_ms = now_ms();
	logmsg("unmounting %s%s", st->mp, force ? " (forced)" : "");
}

static bool should_stop(void *ctx)
{
	struct state *st = ctx;
	uint64_t now = now_ms();
	int status;

	switch (st->phase) {
	case MOUNTING:
		if (waitpid(st->mount_pid, &status, WNOHANG) == st->mount_pid) {
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
					still_mounted(st->from, st->mp)) {
				st->phase = MOUNTED;
				/* first word tells the CLI how it went */
				report(st->status_fd, "%s %s",
					!st->srv->readonly ? "OK" :
					st->o->readonly ? "RO-ASKED" :
					st->hibernated ? "RO-HIBERNATED" :
					"RO-UNCLEAN", st->mp);
				if (st->status_fd >= 0)
					close(st->status_fd);
				st->status_fd = -1;
			} else {
				report(st->status_fd, "ERR mount_nfs failed "
					"(status %d), see the log",
					WIFEXITED(status) ? WEXITSTATUS(status)
					: -1);
				st->failed = true;
				return true;
			}
		} else if (now - st->started_ms > 30000) {
			kill(st->mount_pid, SIGKILL);
			waitpid(st->mount_pid, &status, 0);
			report(st->status_fd, "ERR mount_nfs timed out");
			st->failed = true;
			return true;
		}
		return false;
	case MOUNTED:
		if (stop_requested) {
			start_umount(st, false);
			st->phase = UNMOUNTING;
			return false;
		}
		/* the user ejected it, or someone ran umount */
		if (now - st->last_check_ms >= 250) {
			st->last_check_ms = now;
			if (!still_mounted(st->from, st->mp)) {
				logmsg("%s was unmounted", st->mp);
				return true;
			}
		}
		return false;
	case UNMOUNTING:
		if (st->umount_pid > 0 &&
				waitpid(st->umount_pid, &status, WNOHANG) ==
				st->umount_pid) {
			st->umount_pid = 0;
			if (!still_mounted(st->from, st->mp))
				return true;
			logmsg("umount failed, volume busy");
		}
		if (!still_mounted(st->from, st->mp))
			return true;
		/* busy for 15s after a stop request: force it */
		if (!st->umount_pid && !st->forced &&
				now - st->umount_ms > 15000) {
			st->forced = true;
			start_umount(st, true);
		}
		return false;
	default:
		return true;
	}
}

int serve_main(const struct serve_opts *o)
{
	struct state st = { .o = o, .status_fd = o->status_fd };
	struct nfs_server srv = { 0 };
	struct rpc_server *rpc = NULL;
	n4m_blockdev dev;
	n4m_mount_opts mo = { 0 };
	n4m_volinfo vi;
	n4m_attr root;
	n4m_volume *vol = NULL;
	char id[80], pidfile[1100], opts[512];
	bool made_dir = false;
	uint16_t port = 0;
	FILE *pf;
	int err, rc = 1;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);
	if (o->logfile[0]) {
		logf = fopen(o->logfile, "a");
		if (logf)
			fchmod(fileno(logf), 0644);
	}
	n4m_set_log(engine_log, 1);

	if (o->bsd[0])
		snprintf(id, sizeof(id), "%s", o->bsd);
	else
		snprintf(id, sizeof(id), "image-%d", getpid());
	serve_mount_from(id, st.from, sizeof(st.from));

	/* take the partition away from Apple's read only driver */
	if (o->bsd[0]) {
		struct diskinfo di;

		if (!disk_describe(o->bsd, &di) && di.mountpoint[0]) {
			logmsg("unmounting %s from %s (%s)", o->bsd,
				di.mountpoint, di.fstype);
			err = disk_unmount(o->bsd, false);
			if (err) {
				report(st.status_fd, "ERR could not unmount "
					"%s from %s: %s. Close any apps using "
					"it and try again.", o->bsd,
					di.mountpoint, strerror(err));
				goto out;
			}
		}
	}

	err = n4m_blockdev_open_path(o->source, o->readonly, &dev);
	if (err) {
		report(st.status_fd, "ERR cannot open %s: %s%s", o->source,
			strerror(err), err == EACCES || err == EPERM ?
			" (run with sudo)" : "");
		goto out;
	}
	mo.readonly = o->readonly;
	mo.remove_hiberfile = o->remove_hiberfile;
	mo.reset_journal = o->reset_journal;
	mo.uid = o->uid;
	mo.gid = o->gid;
	err = n4m_mount(&dev, &mo, &vol);
	if (err) {
		if (dev.close)
			dev.close(dev.ctx);
		report(st.status_fd, "ERR %s is not a usable NTFS volume: %s",
			o->source, strerror(err));
		goto out;
	}
	n4m_volinfo_get(vol, &vi);
	if (n4m_getattr(vol, N4M_ROOT_INO, &root)) {
		report(st.status_fd, "ERR cannot read the root folder");
		goto out;
	}
	logmsg("opened %s: \"%s\" NTFS %d.%d, %s%s", o->source, vi.label,
		vi.major_ver, vi.minor_ver, vi.readonly ? "read only" :
		"read-write", vi.was_hibernated ? " (Windows is hibernated "
		"or used Fast Startup)" : "");

	if (o->mountpoint[0]) {
		snprintf(st.mp, sizeof(st.mp), "%s", o->mountpoint);
	} else {
		pick_mountpoint(vi.label, st.mp, sizeof(st.mp));
	}
	if (mkdir(st.mp, 0755) == 0)
		made_dir = true;
	else if (errno != EEXIST) {
		report(st.status_fd, "ERR cannot create %s: %s", st.mp,
			strerror(errno));
		goto out;
	}

	srv.vol = vol;
	srv.fsid = vi.serial ? vi.serial : (uint64_t)getpid() << 16 | 0x4e34;
	srv.root_ref = root.ref;
	srv.uid = o->uid;
	srv.gid = o->gid;
	srv.readonly = vi.readonly;
	st.hibernated = vi.was_hibernated;
	arc4random_buf(srv.writeverf, sizeof(srv.writeverf));
	st.srv = &srv;
	rpc = rpc_listen(&srv, &port);
	if (!rpc) {
		report(st.status_fd, "ERR cannot start the local server: %s",
			strerror(errno));
		goto out;
	}

	serve_pidfile(id, pidfile, sizeof(pidfile));
	pf = fopen(pidfile, "w");
	if (pf) {
		fprintf(pf, "%d\n%s\n", getpid(), st.mp);
		fclose(pf);
	}

	snprintf(opts, sizeof(opts),
		"vers=3,tcp,port=%u,mountport=%u,locallocks,noquota,nfc,"
		"rwsize=%u,readahead=32,dsize=65536,noresvport,hard,intr%s",
		port, port, NFS_MAXDATA, vi.readonly ? ",rdonly" : "");
	{
		char *argv[] = { "/sbin/mount_nfs", "-o", opts, st.from + 0,
			st.mp, NULL };

		logmsg("mount_nfs -o %s %s %s", opts, st.from, st.mp);
		st.mount_pid = spawn(argv);
	}
	if (st.mount_pid < 0) {
		report(st.status_fd, "ERR cannot run mount_nfs: %s",
			strerror(errno));
		goto out;
	}
	st.started_ms = now_ms();
	st.phase = MOUNTING;

	err = rpc_run(rpc, should_stop, &st);
	if (err)
		logmsg("server loop error: %s", strerror(err));
	rc = st.failed ? 1 : 0;
	unlink(pidfile);
out:
	if (rpc)
		rpc_close(rpc);
	if (vol) {
		err = n4m_unmount(vol);
		logmsg("closed volume%s%s", err ? ": " : ", safe to unplug",
			err ? strerror(err) : "");
	}
	if (made_dir && !is_mountpoint(st.mp))
		rmdir(st.mp);
	if (rc == 0 && vol && o->eject_on_unmount && o->bsd[0])
		eject_if_idle(o->bsd);
	if (st.status_fd >= 0)
		close(st.status_fd);
	if (logf)
		fclose(logf);
	return rc;
}
