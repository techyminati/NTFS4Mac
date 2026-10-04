/*
 * ntfs4mac: mount NTFS drives read-write on macOS.
 *
 *   ntfs4mac list
 *   ntfs4mac mount <disk|image> [mountpoint] [options]
 *   ntfs4mac unmount <disk|mountpoint> [--eject]
 *   ntfs4mac info <disk|image>
 */
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "daemon.h"
#include "disk.h"
#include "n4m.h"
#include "serve.h"

extern char **environ;

static void usage(void)
{
	fprintf(stderr,
"NTFS4Mac %s - read and write NTFS drives on your Mac\n"
"\n"
"usage:\n"
"  ntfs4mac list                         show NTFS drives\n"
"  sudo ntfs4mac mount <disk> [folder]   mount read-write (eg. disk4s1)\n"
"  sudo ntfs4mac unmount <disk|folder>   unmount safely\n"
"  ntfs4mac info <disk|image>            details about a volume\n"
"  sudo ntfs4mac install                 auto-mount NTFS drives read-write\n"
"  sudo ntfs4mac uninstall               turn auto-mount off\n"
"\n"
"mount options:\n"
"  --read-only          mount without write access\n"
"  --remove-hiberfile   delete hiberfil.sys left by Windows Fast Startup\n"
"                       (anything unsaved in that Windows session is lost)\n"
"  --foreground         stay in the terminal, log to stderr\n"
"\n"
"unmount options:\n"
"  --eject              also eject the drive so you can unplug it\n"
"  --force              unmount even if files are open\n",
		N4M_VERSION);
}

static void human(uint64_t n, char *buf, size_t bufsz)
{
	const char *u[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	double d = (double)n;
	int i = 0;

	while (d >= 1000 && i < 5) {
		d /= 1000;
		i++;
	}
	snprintf(buf, bufsz, i ? "%.1f %s" : "%.0f %s", d, u[i]);
}

/* Who should own the files: the user that ran sudo, not root. */
static void real_user(uid_t *uid, gid_t *gid)
{
	const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");

	*uid = su ? (uid_t)strtoul(su, NULL, 10) : getuid();
	*gid = sg ? (gid_t)strtoul(sg, NULL, 10) : getgid();
}

/* Finds our mount for a bsd name or mount point. */
static bool find_our_mount(const char *what, char *id, size_t idsz,
		char *mp, size_t mpsz)
{
	struct statfs *mnt;
	char rp[PATH_MAX], bsd[64];
	int n, i;

	if (!realpath(what, rp))
		snprintf(rp, sizeof(rp), "%s", what);
	if (!disk_normalize_name(what, bsd, sizeof(bsd)))
		bsd[0] = 0;
	n = getmntinfo(&mnt, MNT_NOWAIT);
	for (i = 0; i < n; i++) {
		const char *from = mnt[i].f_mntfromname;
		const char *p = strstr(from, ":/ntfs4mac/");

		if (!p)
			continue;
		p += strlen(":/ntfs4mac/");
		if ((bsd[0] && !strcmp(p, bsd)) ||
				!strcmp(mnt[i].f_mntonname, rp)) {
			snprintf(id, idsz, "%s", p);
			snprintf(mp, mpsz, "%s", mnt[i].f_mntonname);
			return true;
		}
	}
	return false;
}

static void list_cb(const struct diskinfo *d, void *ctx)
{
	char size[32], id[80], mp[1024];
	const char *state;
	int *count = ctx;

	human(d->size, size, sizeof(size));
	if (find_our_mount(d->bsd, id, sizeof(id), mp, sizeof(mp)))
		state = "read-write (NTFS4Mac)";
	else if (d->mountpoint[0])
		state = "read-only (macOS)";
	else
		state = "not mounted";
	printf("%-10s %-24s %10s  %-22s %s\n", d->bsd,
		d->label[0] ? d->label : "(no name)", size, state,
		find_our_mount(d->bsd, id, sizeof(id), mp, sizeof(mp)) ? mp :
		d->mountpoint);
	(*count)++;
}

static int cmd_list(void)
{
	int count = 0;

	printf("%-10s %-24s %10s  %-22s %s\n", "DEVICE", "NAME", "SIZE",
		"STATE", "MOUNTED AT");
	disk_list_ntfs(list_cb, &count);
	if (!count)
		printf("(no NTFS drives found)\n");
	return 0;
}

static int cmd_info(const char *target)
{
	char bsd[64], path[1024], size[32], fr[32];
	struct stat st;
	n4m_blockdev dev;
	n4m_mount_opts mo = { .readonly = true };
	n4m_volinfo vi;
	n4m_volume *vol;
	int err;

	if (!stat(target, &st) && S_ISREG(st.st_mode)) {
		snprintf(path, sizeof(path), "%s", target);
	} else if (disk_normalize_name(target, bsd, sizeof(bsd))) {
		snprintf(path, sizeof(path), "/dev/r%s", bsd);
	} else {
		fprintf(stderr, "ntfs4mac: %s is not a disk or image\n",
			target);
		return 1;
	}
	err = n4m_blockdev_open_path(path, true, &dev);
	if (err) {
		fprintf(stderr, "ntfs4mac: cannot open %s: %s%s\n", path,
			strerror(err), err == EACCES ? " (try sudo)" : "");
		return 1;
	}
	err = n4m_mount(&dev, &mo, &vol);
	if (err) {
		dev.close(dev.ctx);
		fprintf(stderr, "ntfs4mac: %s is not NTFS (%s)\n", path,
			strerror(err));
		return 1;
	}
	n4m_volinfo_get(vol, &vi);
	human(vi.total_clusters * vi.cluster_size, size, sizeof(size));
	human(vi.free_clusters * vi.cluster_size, fr, sizeof(fr));
	printf("Name:          %s\n", vi.label[0] ? vi.label : "(none)");
	printf("NTFS version:  %d.%d\n", vi.major_ver, vi.minor_ver);
	printf("Serial:        %016llX\n", (unsigned long long)vi.serial);
	printf("Size:          %s\n", size);
	printf("Free:          %s\n", fr);
	printf("Cluster size:  %u bytes\n", vi.cluster_size);
	printf("Files in use:  %llu\n",
		(unsigned long long)(vi.total_inodes - vi.free_inodes));
	n4m_unmount(vol);
	return 0;
}

static int cmd_mount(int argc, char **argv)
{
	struct serve_opts o = { .status_fd = -1 };
	bool foreground = false;
	const char *target = NULL, *mp = NULL;
	char self[PATH_MAX], fdarg[16], uidarg[16], gidarg[16];
	struct stat st;
	int i, pfd[2];

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--read-only") || !strcmp(argv[i], "-r"))
			o.readonly = true;
		else if (!strcmp(argv[i], "--remove-hiberfile"))
			o.remove_hiberfile = true;
		else if (!strcmp(argv[i], "--eject-on-unmount"))
			o.eject_on_unmount = true;
		else if (!strcmp(argv[i], "--foreground") ||
				!strcmp(argv[i], "-f"))
			foreground = true;
		else if (argv[i][0] == '-') {
			fprintf(stderr, "ntfs4mac: unknown option %s\n",
				argv[i]);
			return 2;
		} else if (!target)
			target = argv[i];
		else if (!mp)
			mp = argv[i];
		else {
			usage();
			return 2;
		}
	}
	if (!target) {
		usage();
		return 2;
	}
	real_user(&o.uid, &o.gid);
	if (!stat(target, &st) && S_ISREG(st.st_mode)) {
		if (!realpath(target, o.source)) {
			perror(target);
			return 1;
		}
	} else if (disk_normalize_name(target, o.bsd, sizeof(o.bsd))) {
		char id[80], cur[1024];

		if (find_our_mount(o.bsd, id, sizeof(id), cur, sizeof(cur))) {
			printf("%s is already mounted read-write at %s\n",
				o.bsd, cur);
			return 0;
		}
		snprintf(o.source, sizeof(o.source), "/dev/r%s", o.bsd);
		if (access(o.source, o.readonly ? R_OK : R_OK | W_OK)) {
			fprintf(stderr, "ntfs4mac: no access to %s, run: "
				"sudo ntfs4mac mount %s\n", o.source, o.bsd);
			return 1;
		}
	} else {
		fprintf(stderr, "ntfs4mac: %s is not a disk (like disk4s1) or "
			"an image file\n", target);
		return 1;
	}
	if (mp) {
		if (!realpath(mp, o.mountpoint)) {
			perror(mp);
			return 1;
		}
	} else if (geteuid() != 0) {
		fprintf(stderr, "ntfs4mac: give a mount folder, only root can "
			"mount in /Volumes\n");
		return 1;
	}

	if (foreground)
		return serve_main(&o);

	/* run the server as a detached process and wait until it's ready */
	if (geteuid() == 0)
		snprintf(o.logfile, sizeof(o.logfile),
			"/var/log/ntfs4mac.log");
	else
		snprintf(o.logfile, sizeof(o.logfile), "%s/Library/Logs/"
			"ntfs4mac.log", getenv("HOME") ? getenv("HOME") : "/tmp");
	if (pipe(pfd)) {
		perror("pipe");
		return 1;
	}
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	{
		uint32_t sz = sizeof(self);
		posix_spawnattr_t attr;
		posix_spawn_file_actions_t fa;
		char *args[24];
		int n = 0, err;
		pid_t pid;

		if (_NSGetExecutablePath(self, &sz)) {
			fprintf(stderr, "ntfs4mac: cannot find myself\n");
			return 1;
		}
		snprintf(fdarg, sizeof(fdarg), "%d", pfd[1]);
		snprintf(uidarg, sizeof(uidarg), "%u", o.uid);
		snprintf(gidarg, sizeof(gidarg), "%u", o.gid);
		args[n++] = self;
		args[n++] = "__serve";
		args[n++] = "--status-fd";
		args[n++] = fdarg;
		args[n++] = "--source";
		args[n++] = o.source;
		args[n++] = "--uid";
		args[n++] = uidarg;
		args[n++] = "--gid";
		args[n++] = gidarg;
		args[n++] = "--log";
		args[n++] = o.logfile;
		if (o.bsd[0]) {
			args[n++] = "--bsd";
			args[n++] = o.bsd;
		}
		if (o.mountpoint[0]) {
			args[n++] = "--mountpoint";
			args[n++] = o.mountpoint;
		}
		if (o.readonly)
			args[n++] = "--read-only";
		if (o.remove_hiberfile)
			args[n++] = "--remove-hiberfile";
		if (o.eject_on_unmount)
			args[n++] = "--eject-on-unmount";
		args[n] = NULL;

		posix_spawnattr_init(&attr);
		posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, 0, "/dev/null",
			O_RDONLY, 0);
		posix_spawn_file_actions_addopen(&fa, 1, "/dev/null",
			O_WRONLY, 0);
		posix_spawn_file_actions_addopen(&fa, 2, "/dev/null",
			O_WRONLY, 0);
		err = posix_spawn(&pid, self, &fa, &attr, args, environ);
		posix_spawn_file_actions_destroy(&fa);
		posix_spawnattr_destroy(&attr);
		close(pfd[1]);
		if (err) {
			fprintf(stderr, "ntfs4mac: cannot start: %s\n",
				strerror(err));
			return 1;
		}
	}
	{
		char buf[1300];
		size_t len = 0;
		ssize_t r;

		while (len < sizeof(buf) - 1 &&
				(r = read(pfd[0], buf + len,
				sizeof(buf) - 1 - len)) > 0)
			len += (size_t)r;
		buf[len] = 0;
		close(pfd[0]);
		while (len && buf[len - 1] == '\n')
			buf[--len] = 0;
		if (!strncmp(buf, "OK ", 3)) {
			printf("Mounted read-write at %s\n", buf + 3);
			return 0;
		}
		fprintf(stderr, "ntfs4mac: %s\n", !strncmp(buf, "ERR ", 4) ?
			buf + 4 : "failed to mount, see the log");
		fprintf(stderr, "log: %s\n", o.logfile);
		return 1;
	}
}

static int cmd_serve(int argc, char **argv)
{
	struct serve_opts o = { .status_fd = -1 };
	int i;

	for (i = 0; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";

		if (!strcmp(a, "--status-fd"))
			o.status_fd = atoi(v), i++;
		else if (!strcmp(a, "--source"))
			snprintf(o.source, sizeof(o.source), "%s", v), i++;
		else if (!strcmp(a, "--bsd"))
			snprintf(o.bsd, sizeof(o.bsd), "%s", v), i++;
		else if (!strcmp(a, "--mountpoint"))
			snprintf(o.mountpoint, sizeof(o.mountpoint), "%s", v),
				i++;
		else if (!strcmp(a, "--log"))
			snprintf(o.logfile, sizeof(o.logfile), "%s", v), i++;
		else if (!strcmp(a, "--uid"))
			o.uid = (uid_t)strtoul(v, NULL, 10), i++;
		else if (!strcmp(a, "--gid"))
			o.gid = (gid_t)strtoul(v, NULL, 10), i++;
		else if (!strcmp(a, "--read-only"))
			o.readonly = true;
		else if (!strcmp(a, "--remove-hiberfile"))
			o.remove_hiberfile = true;
		else if (!strcmp(a, "--eject-on-unmount"))
			o.eject_on_unmount = true;
	}
	return serve_main(&o);
}

static int cmd_unmount(int argc, char **argv)
{
	const char *target = NULL;
	bool eject = false, force = false;
	char id[80], mp[1024], pidfile[1100];
	FILE *pf;
	int i, pid = 0, waited;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--eject") || !strcmp(argv[i], "-e"))
			eject = true;
		else if (!strcmp(argv[i], "--force"))
			force = true;
		else
			target = argv[i];
	}
	if (!target) {
		usage();
		return 2;
	}
	if (!find_our_mount(target, id, sizeof(id), mp, sizeof(mp))) {
		fprintf(stderr, "ntfs4mac: %s is not mounted by NTFS4Mac\n",
			target);
		return 1;
	}
	serve_pidfile(id, pidfile, sizeof(pidfile));
	pf = fopen(pidfile, "r");
	if (pf) {
		if (fscanf(pf, "%d", &pid) != 1)
			pid = 0;
		fclose(pf);
	}
	if (unmount(mp, force ? MNT_FORCE : 0)) {
		fprintf(stderr, "ntfs4mac: cannot unmount %s: %s%s\n", mp,
			strerror(errno), errno == EBUSY ?
			" (close the files using it, or use --force)" : "");
		return 1;
	}
	/* wait for the server to close the volume cleanly */
	for (waited = 0; pid > 0 && waited < 300 && !kill(pid, 0); waited++)
		usleep(100000);
	if (pid > 0 && !kill(pid, 0)) {
		fprintf(stderr, "ntfs4mac: the volume is still closing, do not "
			"unplug yet\n");
		return 1;
	}
	printf("Unmounted %s\n", mp);
	if (eject && strncmp(id, "image-", 6)) {
		int err = disk_eject(id);

		if (err) {
			fprintf(stderr, "ntfs4mac: eject failed: %s\n",
				strerror(err));
			return 1;
		}
		printf("Ejected, safe to unplug\n");
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 2;
	}
	if (!strcmp(argv[1], "list"))
		return cmd_list();
	if (!strcmp(argv[1], "mount"))
		return cmd_mount(argc - 2, argv + 2);
	if (!strcmp(argv[1], "unmount") || !strcmp(argv[1], "umount"))
		return cmd_unmount(argc - 2, argv + 2);
	if (!strcmp(argv[1], "info") && argc == 3)
		return cmd_info(argv[2]);
	if (!strcmp(argv[1], "__serve"))
		return cmd_serve(argc - 2, argv + 2);
	if (!strcmp(argv[1], "daemon"))
		return daemon_main(argc - 2, argv + 2);
	if (!strcmp(argv[1], "install"))
		return install_main();
	if (!strcmp(argv[1], "uninstall"))
		return uninstall_main();
	if (!strcmp(argv[1], "version") || !strcmp(argv[1], "--version")) {
		printf("NTFS4Mac %s\n", N4M_VERSION);
		return 0;
	}
	usage();
	return 2;
}
