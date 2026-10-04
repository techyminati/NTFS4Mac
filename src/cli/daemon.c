/*
 * Auto-mount daemon: "ntfs4mac daemon", started by launchd at boot.
 *
 * It hooks into DiskArbitration's mount approval. When macOS is about to
 * mount an NTFS partition (read only), we decline and mount it read-write
 * with NTFS4Mac instead. If that fails for any reason we let macOS mount
 * it read only as usual, so a drive never just goes missing.
 *
 * Drives that are already mounted read only when the daemon starts are
 * taken over as well.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "daemon.h"
#include "disk.h"
#include "n4m.h"

extern char **environ;

#define IGNORE_FILE "/Library/Application Support/NTFS4Mac/ignore"
#define MAX_TRACKED 64

static char self_path[1024];

/*
 * Test mode (--test-only diskN --mount-dir DIR): runs without root, only
 * ever touches partitions of that one disk and mounts them inside DIR.
 * Used to test the daemon against a disk image.
 */
static char test_only[64];
static char test_dir[1024];
static dispatch_queue_t da_queue;	/* DiskArbitration callbacks */
static dispatch_queue_t work_queue;	/* mounting, one at a time */

/* partitions we are mounting right now, and ones we gave back to macOS */
static char busy[MAX_TRACKED][64];
static char allow_apple[MAX_TRACKED][64];

static void dlog(const char *fmt, ...)
{
	char ts[32];
	time_t t = time(NULL);
	va_list ap;

	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
	fprintf(stderr, "%s [daemon] ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static bool set_has(char set[][64], const char *bsd)
{
	int i;

	for (i = 0; i < MAX_TRACKED; i++)
		if (!strcmp(set[i], bsd))
			return true;
	return false;
}

static void set_add(char set[][64], const char *bsd)
{
	int i;

	if (set_has(set, bsd))
		return;
	for (i = 0; i < MAX_TRACKED; i++)
		if (!set[i][0]) {
			snprintf(set[i], 64, "%s", bsd);
			return;
		}
}

static void set_del(char set[][64], const char *bsd)
{
	int i;

	for (i = 0; i < MAX_TRACKED; i++)
		if (!strcmp(set[i], bsd))
			set[i][0] = 0;
}

static bool desc_str(CFDictionaryRef d, CFStringRef key, char *buf,
		size_t bufsz)
{
	CFTypeRef v = CFDictionaryGetValue(d, key);

	buf[0] = 0;
	if (v && CFGetTypeID(v) == CFStringGetTypeID())
		return CFStringGetCString(v, buf, (CFIndex)bufsz,
				kCFStringEncodingUTF8);
	if (v && CFGetTypeID(v) == CFUUIDGetTypeID()) {
		CFStringRef s = CFUUIDCreateString(NULL, v);
		bool ok = s && CFStringGetCString(s, buf, (CFIndex)bufsz,
				kCFStringEncodingUTF8);

		if (s)
			CFRelease(s);
		return ok;
	}
	return false;
}

/* Volumes the user asked us to leave alone, one UUID or name per line. */
static bool ignored(const char *uuid, const char *label)
{
	FILE *f = fopen(IGNORE_FILE, "r");
	char line[512];
	bool hit = false;

	if (!f)
		return false;
	while (!hit && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		if ((uuid[0] && !strcasecmp(line, uuid)) ||
				(label[0] && !strcmp(line, label)))
			hit = true;
	}
	fclose(f);
	return hit;
}

/* The user sitting at the Mac owns the files we mount. */
static void console_user(uid_t *uid, gid_t *gid)
{
	struct stat st;

	*uid = 0;
	*gid = 0;
	if (!stat("/dev/console", &st)) {
		*uid = st.st_uid;
		*gid = st.st_gid;
	}
	if (*uid == 0)	/* nobody logged in yet */
		*gid = 20;	/* staff */
}

/* Runs "ntfs4mac mount <bsd>" and waits for it. */
static int run_mount(const char *bsd)
{
	char uid_s[16], gid_s[16];
	char mp[1200];
	char *argv[] = { self_path, "mount", (char *)bsd, "--eject-on-unmount",
		NULL, NULL };
	char *envp[4];
	uid_t uid;
	gid_t gid;
	pid_t pid;
	int status, err;

	if (test_dir[0]) {
		snprintf(mp, sizeof(mp), "%s/%s", test_dir, bsd);
		mkdir(mp, 0755);
		argv[4] = mp;
	}
	console_user(&uid, &gid);
	if (test_dir[0]) {
		uid = getuid();
		gid = getgid();
	}
	snprintf(uid_s, sizeof(uid_s), "SUDO_UID=%u", uid);
	snprintf(gid_s, sizeof(gid_s), "SUDO_GID=%u", gid);
	envp[0] = uid_s;
	envp[1] = gid_s;
	envp[2] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
	envp[3] = NULL;
	err = posix_spawn(&pid, self_path, NULL, NULL, argv, envp);
	if (err) {
		dlog("cannot run %s: %s", self_path, strerror(err));
		return err;
	}
	if (waitpid(pid, &status, 0) != pid)
		return errno;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 3)
		return EBUSY;	/* another NTFS4Mac already has it */
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : EIO;
}

/* Our mount failed: let macOS mount it read only after all. */
static void give_back(const char *bsd)
{
	DASessionRef s = DASessionCreate(kCFAllocatorDefault);
	DADiskRef disk;

	if (!s)
		return;
	DASessionSetDispatchQueue(s, da_queue);
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, s, bsd);
	if (disk) {
		dispatch_sync(da_queue, ^{ set_add(allow_apple, bsd); });
		DADiskMount(disk, NULL, kDADiskMountOptionDefault, NULL, NULL);
		CFRelease(disk);
	}
	/* keep the session alive long enough for the request to go out */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC),
		da_queue, ^{
			DASessionSetDispatchQueue(s, NULL);
			CFRelease(s);
		});
}

static void schedule_mount(const char *bsd_in)
{
	char *bsd = strdup(bsd_in);

	if (!bsd)
		return;
	dispatch_async(work_queue, ^{
		int err;

		dlog("mounting %s read-write", bsd);
		err = run_mount(bsd);
		if (err == EBUSY) {
			/* mounted read-write by a manual ntfs4mac mount */
			dlog("%s is already mounted by NTFS4Mac", bsd);
		} else if (err) {
			dlog("could not mount %s read-write, letting macOS "
				"mount it read only", bsd);
			give_back(bsd);
		}
		dispatch_sync(da_queue, ^{ set_del(busy, bsd); });
		free(bsd);
	});
}

/* Should we take this partition? Fills bsd. Runs on da_queue. */
static bool wanted(DADiskRef disk, char *bsd, size_t bsdsz, bool *mounted)
{
	CFDictionaryRef d = DADiskCopyDescription(disk);
	char kind[32], uuid[64], label[256];
	const char *name = DADiskGetBSDName(disk);
	bool ok = false;

	*mounted = false;
	if (!d || !name)
		goto out;
	snprintf(bsd, bsdsz, "%s", name);
	if (test_only[0] && (strncmp(name, test_only, strlen(test_only)) ||
			name[strlen(test_only)] != 's'))
		goto out;	/* test mode: not our disk image */
	desc_str(d, kDADiskDescriptionVolumeKindKey, kind, sizeof(kind));
	if (strcmp(kind, "ntfs"))
		goto out;
	desc_str(d, kDADiskDescriptionVolumeUUIDKey, uuid, sizeof(uuid));
	desc_str(d, kDADiskDescriptionVolumeNameKey, label, sizeof(label));
	*mounted = CFDictionaryGetValue(d, kDADiskDescriptionVolumePathKey)
		!= NULL;
	if (ignored(uuid, label)) {
		dlog("leaving %s (%s) alone, it is in %s", name, label,
			IGNORE_FILE);
		goto out;
	}
	ok = true;
out:
	if (d)
		CFRelease(d);
	return ok;
}

static DADissenterRef approve_mount(DADiskRef disk, void *ctx)
{
	char bsd[64];
	bool mounted;

	(void)ctx;
	if (!wanted(disk, bsd, sizeof(bsd), &mounted))
		return NULL;
	if (set_has(allow_apple, bsd)) {
		set_del(allow_apple, bsd);
		return NULL;	/* our fallback asked for this mount */
	}
	if (!set_has(busy, bsd)) {
		set_add(busy, bsd);
		schedule_mount(bsd);
	}
	return DADissenterCreate(kCFAllocatorDefault, kDAReturnExclusiveAccess,
		CFSTR("NTFS4Mac mounts this volume read-write"));
}

static void disk_appeared(DADiskRef disk, void *ctx)
{
	char bsd[64];
	bool mounted;

	(void)ctx;
	/* only drives macOS already mounted read only before we started */
	if (!wanted(disk, bsd, sizeof(bsd), &mounted) || !mounted)
		return;
	if (!set_has(busy, bsd)) {
		set_add(busy, bsd);
		schedule_mount(bsd);
	}
}

static void disk_disappeared(DADiskRef disk, void *ctx)
{
	const char *bsd = DADiskGetBSDName(disk);

	(void)ctx;
	if (bsd) {
		set_del(allow_apple, bsd);
		set_del(busy, bsd);
	}
}

int daemon_main(int argc, char **argv)
{
	uint32_t sz = sizeof(self_path);
	DASessionRef session;
	CFMutableDictionaryRef match;
	int i;

	for (i = 0; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--test-only"))
			snprintf(test_only, sizeof(test_only), "%s", argv[i + 1]);
		else if (!strcmp(argv[i], "--mount-dir"))
			snprintf(test_dir, sizeof(test_dir), "%s", argv[i + 1]);
	}
	if (test_only[0] && (strncmp(test_only, "disk", 4) ||
			strchr(test_only + 4, 's') || !test_dir[0])) {
		fprintf(stderr, "test mode needs --test-only diskN (a whole "
			"disk) and --mount-dir DIR\n");
		return 1;
	}
	if (geteuid() != 0 && !test_only[0]) {
		fprintf(stderr, "ntfs4mac daemon must run as root\n");
		return 1;
	}
	if (_NSGetExecutablePath(self_path, &sz)) {
		fprintf(stderr, "ntfs4mac: cannot find myself\n");
		return 1;
	}
	da_queue = dispatch_queue_create("ntfs4mac.da", DISPATCH_QUEUE_SERIAL);
	work_queue = dispatch_queue_create("ntfs4mac.work",
			DISPATCH_QUEUE_SERIAL);
	session = DASessionCreate(kCFAllocatorDefault);
	if (!session) {
		fprintf(stderr, "ntfs4mac: no DiskArbitration session\n");
		return 1;
	}
	match = CFDictionaryCreateMutable(NULL, 0,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(match, kDADiskDescriptionVolumeKindKey,
		CFSTR("ntfs"));
	DARegisterDiskMountApprovalCallback(session, match, approve_mount,
		NULL);
	DARegisterDiskAppearedCallback(session, match, disk_appeared, NULL);
	DARegisterDiskDisappearedCallback(session, match, disk_disappeared,
		NULL);
	DASessionSetDispatchQueue(session, da_queue);
	CFRelease(match);
	if (test_only[0])
		dlog("test mode: only handling %s, mounting in %s", test_only,
			test_dir);
	dlog("%s auto-mount is running", n4m_version_long());
	dispatch_main();
}

/* ---- install / uninstall ---------------------------------------------- */

#define INSTALL_BIN	"/usr/local/bin/ntfs4mac"
#define PLIST_LABEL	"com.ntfs4mac.automount"
#define PLIST_PATH	"/Library/LaunchDaemons/com.ntfs4mac.automount.plist"
#define SUPPORT_DIR	"/Library/Application Support/NTFS4Mac"

static const char plist[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
"\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
"<plist version=\"1.0\">\n"
"<dict>\n"
"\t<key>Label</key>\n"
"\t<string>" PLIST_LABEL "</string>\n"
"\t<key>ProgramArguments</key>\n"
"\t<array>\n"
"\t\t<string>" INSTALL_BIN "</string>\n"
"\t\t<string>daemon</string>\n"
"\t</array>\n"
"\t<key>RunAtLoad</key>\n"
"\t<true/>\n"
"\t<key>KeepAlive</key>\n"
"\t<true/>\n"
"\t<!-- mounted volumes keep running if the daemon restarts -->\n"
"\t<key>AbandonProcessGroup</key>\n"
"\t<true/>\n"
"\t<key>StandardOutPath</key>\n"
"\t<string>/var/log/ntfs4mac.log</string>\n"
"\t<key>StandardErrorPath</key>\n"
"\t<string>/var/log/ntfs4mac.log</string>\n"
"</dict>\n"
"</plist>\n";

static const char ignore_template[] =
"# NTFS4Mac leaves the volumes listed here alone, so macOS mounts them\n"
"# read only as usual. One volume name or volume UUID per line.\n"
"# Find them with: ntfs4mac list   or   diskutil info disk4s1\n";

static int run(char *const argv[])
{
	pid_t pid;
	int status, err = posix_spawn(&pid, argv[0], NULL, NULL, argv,
		environ);

	if (err)
		return err;
	if (waitpid(pid, &status, 0) != pid)
		return errno;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Same as run(), with the command's own output thrown away. */
static int run_quiet(char *const argv[])
{
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int status, err;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (err)
		return err;
	if (waitpid(pid, &status, 0) != pid)
		return errno;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static bool daemon_loaded(void)
{
	char *argv[] = { "/bin/launchctl", "print", "system/" PLIST_LABEL,
		NULL };

	return run_quiet(argv) == 0;
}

/*
 * launchctl bootout returns before launchd has really let go of the
 * service, and bootstrapping the same label right away then fails with
 * "Bootstrap failed: 5: Input/output error". Wait until it is gone.
 */
static void stop_daemon(void)
{
	char *argv[] = { "/bin/launchctl", "bootout", "system/" PLIST_LABEL,
		NULL };
	int i;

	if (!daemon_loaded())
		return;
	run_quiet(argv);
	for (i = 0; i < 100 && daemon_loaded(); i++)
		usleep(100000);		/* up to 10 seconds */
}

static int start_daemon(void)
{
	char *enable[] = { "/bin/launchctl", "enable", "system/" PLIST_LABEL,
		NULL };
	char *boot[] = { "/bin/launchctl", "bootstrap", "system", PLIST_PATH,
		NULL };
	int i;

	run_quiet(enable);	/* in case it was disabled at some point */
	for (i = 0; i < 5; i++) {
		if (run_quiet(boot) == 0 || daemon_loaded())
			return 0;
		usleep(500000);
	}
	/* one last time, letting launchctl explain what is wrong */
	return run(boot) == 0 || daemon_loaded() ? 0 : 1;
}

static int write_file(const char *path, const char *data, mode_t mode)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return errno;
	fputs(data, f);
	if (fclose(f))
		return errno;
	chown(path, 0, 0);
	chmod(path, mode);
	return 0;
}

int install_main(void)
{
	uint32_t sz = sizeof(self_path);
	struct stat st;
	int err;

	if (geteuid() != 0) {
		fprintf(stderr, "run: sudo ntfs4mac install\n");
		return 1;
	}
	if (_NSGetExecutablePath(self_path, &sz)) {
		fprintf(stderr, "ntfs4mac: cannot find myself\n");
		return 1;
	}
	mkdir("/usr/local", 0755);
	mkdir("/usr/local/bin", 0755);
	{
		char *argv[] = { "/usr/bin/install", "-m", "755", "-o", "root",
			"-g", "wheel", self_path, INSTALL_BIN, NULL };

		if (strcmp(self_path, INSTALL_BIN) && run(argv)) {
			fprintf(stderr, "ntfs4mac: could not copy to %s\n",
				INSTALL_BIN);
			return 1;
		}
	}
	printf("installed %s\n", INSTALL_BIN);
	mkdir(SUPPORT_DIR, 0755);
	if (stat(SUPPORT_DIR "/ignore", &st))
		write_file(SUPPORT_DIR "/ignore", ignore_template, 0644);

	/* replace an older daemon if one is loaded */
	stop_daemon();
	err = write_file(PLIST_PATH, plist, 0644);
	if (err) {
		fprintf(stderr, "ntfs4mac: cannot write %s: %s\n", PLIST_PATH,
			strerror(err));
		return 1;
	}
	if (start_daemon()) {
		fprintf(stderr, "ntfs4mac: could not start the auto-mount "
			"daemon (launchctl bootstrap failed)\n");
		return 1;
	}
	printf("auto-mount is on: NTFS drives now mount read-write when "
		"plugged in\n");
	printf("drives to leave alone go in: %s/ignore\n", SUPPORT_DIR);
	return 0;
}

int uninstall_main(void)
{
	struct stat st;
	int rc = 0;

	if (geteuid() != 0) {
		fprintf(stderr, "run: sudo ntfs4mac uninstall\n");
		return 1;
	}
	stop_daemon();
	/* exactly what install created, nothing else */
	if (unlink(PLIST_PATH) && errno != ENOENT) {
		fprintf(stderr, "ntfs4mac: cannot remove %s: %s\n", PLIST_PATH,
			strerror(errno));
		rc = 1;
	}
	if (!lstat(INSTALL_BIN, &st) && unlink(INSTALL_BIN)) {
		fprintf(stderr, "ntfs4mac: cannot remove %s: %s\n",
			INSTALL_BIN, strerror(errno));
		rc = 1;
	}
	unlink(SUPPORT_DIR "/ignore");
	rmdir(SUPPORT_DIR);	/* only if nothing else is in there */
	if (rc)
		return rc;
	printf("NTFS4Mac is removed. Drives mounted right now stay mounted "
		"until you eject them.\n");
	return 0;
}
