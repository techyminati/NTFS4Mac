/*
 * Engine test: runs real operations against an NTFS image.
 *
 *   enginetest <image>
 *
 * The image is modified. tests/run.sh creates a fresh one with mkntfs and
 * afterwards checks it with the independent ntfsprogs tools.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "n4m.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define OK(expr) do { \
	int _e = (expr); \
	CHECK(_e == 0, "%s -> %s", #expr, strerror(_e)); \
} while (0)

#define ERR(expr, want) do { \
	int _e = (expr); \
	CHECK(_e == (want), "%s -> %s, wanted %s", #expr, strerror(_e), \
		strerror(want)); \
} while (0)

#define ROOT N4M_ROOT_INO

static n4m_volume *vol;
static char *image;

static void section(const char *name)
{
	printf("- %s\n", name);
}

static uint64_t mk(uint64_t dir, const char *name, int type)
{
	n4m_attr a;
	int e = n4m_create(vol, dir, name, type, 0644, &a);

	CHECK(e == 0, "create %s -> %s", name, strerror(e));
	return e ? 0 : a.ino;
}

static uint64_t find(uint64_t dir, const char *name)
{
	n4m_attr a;

	return n4m_lookup(vol, dir, name, &a) ? 0 : a.ino;
}

static void put(uint64_t ino, uint64_t off, const void *buf, size_t len)
{
	size_t w = 0;

	OK(n4m_write(vol, ino, off, len, buf, &w));
	CHECK(w == len, "short write %zu/%zu", w, len);
}

static bool same_data(uint64_t ino, uint64_t off, const void *want,
		size_t len)
{
	char *got = malloc(len ? len : 1);
	size_t n = 0;
	bool same;

	OK(n4m_read(vol, ino, off, len, got, &n));
	same = n == len && !memcmp(got, want, len);
	free(got);
	return same;
}

static void fill_pattern(unsigned char *p, size_t len, unsigned seed)
{
	size_t i;
	uint32_t x = seed * 2654435761u + 1;

	for (i = 0; i < len; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		p[i] = (unsigned char)x;
	}
}

static void do_mount2(bool ro, bool remove_hiberfile)
{
	n4m_blockdev dev;
	n4m_mount_opts o = { .readonly = ro, .uid = getuid(), .gid = getgid(),
		.remove_hiberfile = remove_hiberfile };
	int e = n4m_blockdev_open_path(image, ro, &dev);

	CHECK(e == 0, "open %s -> %s", image, strerror(e));
	if (e)
		exit(1);
	e = n4m_mount(&dev, &o, &vol);
	CHECK(e == 0, "mount -> %s", strerror(e));
	if (e)
		exit(1);
}

static void do_mount(bool ro)
{
	do_mount2(ro, false);
}

/* readdir helpers */
struct listing {
	char names[2048][256];
	int count;
	int stop_every;
	int since_stop;
	uint64_t last_cookie;
};

static int collect(void *ctx, const char *name, size_t len, uint64_t ino,
		int type, uint64_t next, const n4m_attr *attr)
{
	struct listing *l = ctx;

	if (l->stop_every && l->since_stop == l->stop_every) {
		l->since_stop = 0;
		return 1;
	}
	if (l->count < 2048) {
		snprintf(l->names[l->count], 256, "%.*s", (int)len, name);
		l->count++;
	}
	l->since_stop++;
	l->last_cookie = next;
	return 0;
}

static void list_dir(uint64_t dir, struct listing *l, int flags)
{
	uint64_t cookie = 0;
	bool eof = false;
	int rounds = 0;

	l->count = 0;
	l->last_cookie = 0;
	while (!eof && rounds++ < 10000) {
		l->since_stop = 0;
		if (n4m_readdir(vol, dir, cookie, flags, collect, l, &eof))
			break;
		cookie = l->last_cookie;
	}
}

static bool listed(struct listing *l, const char *name)
{
	int i;

	for (i = 0; i < l->count; i++)
		if (!strcmp(l->names[i], name))
			return true;
	return false;
}

int main(int argc, char **argv)
{
	n4m_attr a;
	n4m_volinfo info;
	struct listing *l = calloc(1, sizeof(*l));
	uint64_t f, d, d2, big;
	unsigned char *buf;
	size_t n;
	char tbuf[1024];
	int i;

	if (argc != 2) {
		fprintf(stderr, "usage: enginetest <image>\n");
		return 2;
	}
	image = argv[1];
	n4m_set_log(NULL, 0);

	section("mount and volume info");
	do_mount(false);
	OK(n4m_volinfo_get(vol, &info));
	CHECK(!info.readonly, "mounted read only");
	CHECK(!strcmp(info.label, "N4MTest"), "label '%s'", info.label);
	OK(n4m_getattr(vol, ROOT, &a));
	CHECK(a.type == N4M_TYPE_DIR, "root type %d", a.type);

	section("create, write, read");
	f = mk(ROOT, "hello.txt", N4M_TYPE_FILE);
	put(f, 0, "hello world", 11);
	CHECK(same_data(f, 0, "hello world", 11), "hello content");
	OK(n4m_getattr(vol, f, &a));
	CHECK(a.size == 11, "size %llu", (unsigned long long)a.size);
	CHECK(a.type == N4M_TYPE_FILE, "type");
	n = 0;
	OK(n4m_read(vol, f, 100, 10, tbuf, &n));
	CHECK(n == 0, "read past end returned %zu", n);

	section("case insensitive lookup");
	CHECK(find(ROOT, "HELLO.TXT") == f, "HELLO.TXT");
	CHECK(find(ROOT, "Hello.Txt") == f, "Hello.Txt");
	ERR(n4m_create(vol, ROOT, "HELLO.txt", N4M_TYPE_FILE, 0644, &a),
		EEXIST);
	CHECK(find(ROOT, "$MFT") == 0, "$MFT must be hidden");

	section("unicode names");
	{
		const char *nfd = "cafe\xcc\x81.txt";	/* e + combining */
		const char *nfc = "caf\xc3\xa9.txt";
		uint64_t u = mk(ROOT, nfd, N4M_TYPE_FILE);

		CHECK(find(ROOT, nfc) == u, "lookup NFC");
		CHECK(find(ROOT, nfd) == u, "lookup NFD");
		list_dir(ROOT, l, 0);
		CHECK(listed(l, nfc), "stored as NFC");
		mk(ROOT, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x98\x80",
			N4M_TYPE_FILE);
		CHECK(find(ROOT, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e "
			"\xf0\x9f\x98\x80") != 0, "CJK and emoji");
	}

	section("characters Windows does not allow");
	{
		const char *odd = "a:b?c*d<e>f|g\"h\\i.txt";
		uint64_t o = mk(ROOT, odd, N4M_TYPE_FILE);

		CHECK(find(ROOT, odd) == o, "lookup odd");
		list_dir(ROOT, l, 0);
		CHECK(listed(l, odd), "odd name round trips");
		o = mk(ROOT, "ends with dot.", N4M_TYPE_FILE);
		CHECK(find(ROOT, "ends with dot.") == o, "trailing dot");
		o = mk(ROOT, "ends with space ", N4M_TYPE_FILE);
		CHECK(find(ROOT, "ends with space ") == o, "trailing space");
		ERR(n4m_create(vol, ROOT, "a/b", N4M_TYPE_FILE, 0644, &a),
			EINVAL);
	}

	section("directories and big data");
	d = mk(ROOT, "Folder", N4M_TYPE_DIR);
	OK(n4m_getattr(vol, d, &a));
	CHECK(a.type == N4M_TYPE_DIR, "Folder is a dir");
	big = mk(d, "inner.bin", N4M_TYPE_FILE);
	buf = malloc(64 << 20);
	fill_pattern(buf, 5 << 20, 7);
	put(big, 0, buf, 5 << 20);
	CHECK(same_data(big, 0, buf, 5 << 20), "5 MiB round trip");
	/* unaligned overwrite in the middle */
	put(big, 12345, "PATCHED", 7);
	memcpy(buf + 12345, "PATCHED", 7);
	CHECK(same_data(big, 0, buf, 5 << 20), "unaligned overwrite");
	CHECK(same_data(big, 12340, buf + 12340, 20), "unaligned read");

	section("truncate and sparse growth");
	{
		n4m_setattr_req sa = { .mask = N4M_SET_SIZE, .size = 1000 };

		OK(n4m_setattr(vol, big, &sa, &a));
		CHECK(a.size == 1000, "shrunk to %llu",
			(unsigned long long)a.size);
		CHECK(same_data(big, 0, buf, 1000), "data kept after shrink");
		sa.size = 3 << 20;
		OK(n4m_setattr(vol, big, &sa, &a));
		CHECK(a.size == 3 << 20, "grew");
		memset(buf + 1000, 0, (3 << 20) - 1000);
		CHECK(same_data(big, 0, buf, 3 << 20), "zeros after growth");
	}

	section("timestamps and flags");
	{
		n4m_setattr_req sa = {
			.mask = N4M_SET_MTIME | N4M_SET_BTIME | N4M_SET_ATIME,
			.mtime = { 1600000000, 123456700 },
			.btime = { 1500000000, 0 },
			.atime = { 1650000000, 500 },
		};

		OK(n4m_setattr(vol, f, &sa, &a));
		CHECK(a.mtime.tv_sec == 1600000000 &&
			a.mtime.tv_nsec == 123456700, "mtime");
		CHECK(a.btime.tv_sec == 1500000000, "btime");
		sa.mask = N4M_SET_FLAGS;
		sa.flags = UF_HIDDEN;
		OK(n4m_setattr(vol, f, &sa, &a));
		CHECK(a.flags & UF_HIDDEN, "hidden set");
		sa.flags = 0;
		OK(n4m_setattr(vol, f, &sa, &a));
		CHECK(!(a.flags & UF_HIDDEN), "hidden cleared");
		mk(ROOT, ".DS_Store", N4M_TYPE_FILE);
		OK(n4m_lookup(vol, ROOT, ".DS_Store", &a));
		CHECK(a.ntfs_attrib & 0x2, "dot files hidden on Windows");
	}

	section("rename");
	OK(n4m_rename(vol, ROOT, "hello.txt", ROOT, "greeting.txt"));
	CHECK(find(ROOT, "hello.txt") == 0, "old name gone");
	CHECK(find(ROOT, "greeting.txt") == f, "new name");
	CHECK(same_data(f, 0, "hello world", 11), "content follows");
	OK(n4m_rename(vol, ROOT, "greeting.txt", ROOT, "Greeting.TXT"));
	list_dir(ROOT, l, 0);
	CHECK(listed(l, "Greeting.TXT") && !listed(l, "greeting.txt"),
		"case only rename");
	CHECK(find(ROOT, "greeting.txt") == f, "same inode after case rename");
	{
		uint64_t x = mk(ROOT, "x.txt", N4M_TYPE_FILE);
		uint64_t y = mk(ROOT, "y.txt", N4M_TYPE_FILE);

		put(x, 0, "XXXX", 4);
		put(y, 0, "YY", 2);
		OK(n4m_rename(vol, ROOT, "x.txt", ROOT, "y.txt"));
		CHECK(find(ROOT, "x.txt") == 0, "x gone");
		CHECK(find(ROOT, "y.txt") == x, "y is now x");
		CHECK(same_data(x, 0, "XXXX", 4), "replaced content");
		ERR(n4m_getattr(vol, y, &a), ESTALE);
		list_dir(ROOT, l, 0);
		for (i = 0; i < l->count; i++)
			CHECK(strncmp(l->names[i], ".ntfs4mac-rename", 16),
				"temp name left: %s", l->names[i]);
	}
	d2 = mk(ROOT, "B", N4M_TYPE_DIR);
	mk(ROOT, "A", N4M_TYPE_DIR);
	mk(find(ROOT, "A"), "deep.txt", N4M_TYPE_FILE);
	OK(n4m_rename(vol, ROOT, "A", d2, "A"));
	CHECK(find(ROOT, "A") == 0, "A moved");
	CHECK(find(find(d2, "A"), "deep.txt") != 0, "A content moved");
	OK(n4m_lookup(vol, find(d2, "A"), "..", &a));
	CHECK(a.ino == d2, "parent of moved dir is B");
	ERR(n4m_rename(vol, ROOT, "B", find(d2, "A"), "B"), EINVAL);
	ERR(n4m_rename(vol, ROOT, "B", ROOT, "y.txt"), ENOTDIR);
	ERR(n4m_rename(vol, ROOT, "nope", ROOT, "x"), ENOENT);

	section("hard links");
	OK(n4m_link(vol, f, d, "link.txt", &a));
	CHECK(a.nlink == 2, "nlink %u", a.nlink);
	CHECK(find(d, "link.txt") == f, "link resolves");
	OK(n4m_remove(vol, ROOT, "Greeting.TXT", false));
	CHECK(same_data(f, 0, "hello world", 11), "data alive via link");
	OK(n4m_getattr(vol, f, &a));
	CHECK(a.nlink == 1, "nlink back to 1");
	ERR(n4m_link(vol, d, ROOT, "dirlink", &a), EPERM);

	section("symlinks");
	OK(n4m_symlink(vol, ROOT, "rel", "Folder/inner.bin", &a));
	CHECK(a.type == N4M_TYPE_SYMLINK, "rel is a symlink");
	n = 0;
	OK(n4m_readlink(vol, a.ino, tbuf, sizeof(tbuf), &n));
	CHECK(!strcmp(tbuf, "Folder/inner.bin"), "rel target '%s'", tbuf);
	CHECK(a.size == strlen("Folder/inner.bin"), "symlink size");
	OK(n4m_symlink(vol, ROOT, "reldir", "Folder", &a));
	OK(n4m_readlink(vol, a.ino, tbuf, sizeof(tbuf), &n));
	CHECK(!strcmp(tbuf, "Folder"), "dir symlink target '%s'", tbuf);
	OK(n4m_symlink(vol, d, "abs", "/Users/someone/file", &a));
	OK(n4m_readlink(vol, a.ino, tbuf, sizeof(tbuf), &n));
	CHECK(!strcmp(tbuf, "/Users/someone/file"), "abs target '%s'", tbuf);
	OK(n4m_symlink(vol, d, "up", "../x y:z/..", &a));
	OK(n4m_readlink(vol, a.ino, tbuf, sizeof(tbuf), &n));
	CHECK(!strcmp(tbuf, "../x y:z/.."), "odd target '%s'", tbuf);
	OK(n4m_remove(vol, ROOT, "reldir", false));
	CHECK(find(ROOT, "Folder") == d, "removing link kept target");

	section("remove");
	ERR(n4m_remove(vol, ROOT, "Folder", true), ENOTEMPTY);
	ERR(n4m_remove(vol, ROOT, "Folder", false), EPERM);
	ERR(n4m_remove(vol, ROOT, "y.txt", true), ENOTDIR);
	OK(n4m_remove(vol, ROOT, "y.txt", false));
	CHECK(find(ROOT, "y.txt") == 0, "y removed");
	{
		uint64_t e = mk(ROOT, "empty", N4M_TYPE_DIR);

		(void)e;
		OK(n4m_remove(vol, ROOT, "empty", true));
		CHECK(find(ROOT, "empty") == 0, "empty dir removed");
	}

	section("large directory with paged listing");
	{
		uint64_t many = mk(ROOT, "many", N4M_TYPE_DIR);
		char name[64];
		int missing = 0;

		for (i = 0; i < 700; i++) {
			snprintf(name, sizeof(name), "file-%04d.dat", i);
			mk(many, name, N4M_TYPE_FILE);
		}
		l->stop_every = 37;
		list_dir(many, l, N4M_READDIR_NO_DOTS);
		l->stop_every = 0;
		CHECK(l->count == 700, "listed %d of 700", l->count);
		for (i = 0; i < 700; i++) {
			snprintf(name, sizeof(name), "file-%04d.dat", i);
			if (!listed(l, name))
				missing++;
		}
		CHECK(!missing, "%d names missing", missing);
		for (i = 0; i < 700; i += 2) {
			snprintf(name, sizeof(name), "file-%04d.dat", i);
			OK(n4m_remove(vol, many, name, false));
		}
		list_dir(many, l, N4M_READDIR_NO_DOTS);
		CHECK(l->count == 350, "350 left, got %d", l->count);
	}

	section("alternate data streams");
	{
		const char *ad = N4M_APPLEDOUBLE_STREAM;
		uint64_t sf = mk(ROOT, "streams.txt", N4M_TYPE_FILE);
		uint64_t sz = 0;
		char sbuf[64];

		put(sf, 0, "main data", 9);
		ERR(n4m_stream_size(vol, sf, ad, &sz), ENOENT);
		OK(n4m_stream_write(vol, sf, ad, 0, 11, "mac metdata", &n));
		CHECK(n == 11, "stream write %zu", n);
		OK(n4m_stream_write(vol, sf, ad, 6, 5, "adata", &n));
		OK(n4m_stream_size(vol, sf, ad, &sz));
		CHECK(sz == 11, "stream size %llu", (unsigned long long)sz);
		OK(n4m_stream_read(vol, sf, ad, 0, sizeof(sbuf), sbuf, &n));
		CHECK(n == 11 && !memcmp(sbuf, "mac meadata", 11),
			"stream content '%.*s'", (int)n, sbuf);
		CHECK(same_data(sf, 0, "main data", 9), "main data untouched");
		OK(n4m_getattr(vol, sf, &a));
		CHECK(a.size == 9, "file size ignores streams");
		OK(n4m_rename(vol, ROOT, "streams.txt", d, "moved.txt"));
		OK(n4m_stream_size(vol, sf, ad, &sz));
		CHECK(sz == 11, "stream moved with the file");
		OK(n4m_stream_truncate(vol, sf, ad, 3));
		OK(n4m_stream_size(vol, sf, ad, &sz));
		CHECK(sz == 3, "stream truncated");
		OK(n4m_stream_write(vol, d, ad, 0, 4, "dir!", &n));
		OK(n4m_stream_size(vol, d, ad, &sz));
		CHECK(sz == 4, "stream on a folder");
		OK(n4m_stream_remove(vol, d, ad));
		ERR(n4m_stream_size(vol, d, ad, &sz), ENOENT);
		ERR(n4m_stream_remove(vol, d, ad), ENOENT);
		list_dir(d, l, 0);
		CHECK(listed(l, "moved.txt"), "file listed once");
	}

	section("files over 4 GiB (sparse)");
	{
		uint64_t sp = mk(ROOT, "sparse.bin", N4M_TYPE_FILE);
		uint64_t far = (5ULL << 30) + 12345;	/* past 4 GiB */
		char zeros[4096] = { 0 };

		put(sp, far, "far away", 8);
		OK(n4m_getattr(vol, sp, &a));
		CHECK(a.size == far + 8, "size %llu", (unsigned long long)a.size);
		CHECK(a.alloc_size < (64ULL << 20), "should be sparse, %llu "
			"bytes allocated", (unsigned long long)a.alloc_size);
		CHECK(same_data(sp, far, "far away", 8), "data past 4 GiB");
		CHECK(same_data(sp, 1ULL << 32, zeros, sizeof(zeros)),
			"hole reads as zeros");
		CHECK(same_data(sp, far - 100, zeros, 100), "zeros just before");
		put(sp, (4ULL << 30) - 2, "edge", 4);	/* across 4 GiB */
		CHECK(same_data(sp, (4ULL << 30) - 2, "edge", 4),
			"write across the 4 GiB line");
		OK(n4m_remove(vol, ROOT, "sparse.bin", false));
	}

	section("64 MiB file");
	{
		uint64_t g = mk(ROOT, "big.bin", N4M_TYPE_FILE);
		struct timespec t0, t1;
		double secs;

		fill_pattern(buf, 64 << 20, 99);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < 64; i++)
			put(g, (uint64_t)i << 20, buf + ((size_t)i << 20), 1 << 20);
		OK(n4m_close_write(vol, g));
		OK(n4m_sync(vol));
		clock_gettime(CLOCK_MONOTONIC, &t1);
		secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("  write: %.0f MB/s\n", 64 / secs);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CHECK(same_data(g, 0, buf, 64 << 20), "64 MiB round trip");
		clock_gettime(CLOCK_MONOTONIC, &t1);
		secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("  read: %.0f MB/s\n", 64 / secs);
	}

	section("label");
	OK(n4m_set_label(vol, "Renamed"));

	section("remount and verify");
	OK(n4m_unmount(vol));
	do_mount(false);
	OK(n4m_volinfo_get(vol, &info));
	CHECK(!strcmp(info.label, "Renamed"), "label after remount '%s'",
		info.label);
	f = find(d, "link.txt");
	CHECK(f && same_data(f, 0, "hello world", 11), "link.txt survived");
	big = find(ROOT, "big.bin");
	CHECK(big && same_data(big, 0, buf, 64 << 20), "big.bin survived");
	CHECK(find(ROOT, "y.txt") == 0, "deleted stays deleted");
	OK(n4m_getattr(vol, find(d2, "A"), &a));
	OK(n4m_unmount(vol));

	section("read only mount");
	do_mount(true);
	ERR(n4m_create(vol, ROOT, "nope", N4M_TYPE_FILE, 0644, &a), EROFS);
	OK(n4m_unmount(vol));

	section("hibernated Windows (Fast Startup)");
	do_mount(false);
	{
		char *hb = calloc(1, 8192);
		uint64_t h = mk(ROOT, "hiberfil.sys", N4M_TYPE_FILE);

		memcpy(hb, "HIBR", 4);
		put(h, 0, hb, 8192);
		free(hb);
	}
	OK(n4m_unmount(vol));
	do_mount(false);
	OK(n4m_volinfo_get(vol, &info));
	CHECK(info.readonly, "hibernated volume must mount read only");
	CHECK(info.was_hibernated, "hibernation detected");
	ERR(n4m_create(vol, ROOT, "nope", N4M_TYPE_FILE, 0644, &a), EROFS);
	OK(n4m_unmount(vol));
	do_mount2(false, true);
	OK(n4m_volinfo_get(vol, &info));
	CHECK(!info.readonly, "writable after removing hiberfil.sys");
	CHECK(find(ROOT, "hiberfil.sys") == 0, "hiberfil.sys removed");
	CHECK(find(ROOT, "big.bin") != 0, "other files untouched");
	OK(n4m_unmount(vol));

	free(buf);
	free(l);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
