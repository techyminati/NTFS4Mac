/*
 * Logging. libntfs-3g messages are routed through the same hook so
 * frontends decide where everything ends up (stderr, os_log, a file).
 */
#include <stdio.h>

#include "internal.h"

static n4m_log_fn log_fn;
static int log_max = 1;

static void default_log(int level, const char *msg)
{
	static const char *names[] = { "error", "info", "debug" };

	fprintf(stderr, "ntfs4mac[%s]: %s\n", names[level], msg);
}

void n4m_set_log(n4m_log_fn fn, int max_level)
{
	log_fn = fn;
	log_max = max_level;
}

static void vlog(int level, const char *fmt, va_list ap)
{
	char buf[1024];
	size_t n;

	if (level > log_max)
		return;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	n = strlen(buf);
	while (n && buf[n - 1] == '\n')
		buf[--n] = 0;
	if (!n)
		return;
	(log_fn ? log_fn : default_log)(level, buf);
}

void n4m_log(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(level, fmt, ap);
	va_end(ap);
}

static int ntfs3g_handler(const char *function, const char *file, int line,
		u32 level, void *data, const char *format, va_list args)
{
	int ours;

	(void)function; (void)file; (void)line; (void)data;
	if (level & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR |
			NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_WARNING))
		ours = 0;
	else if (level & (NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_QUIET |
			NTFS_LOG_LEVEL_VERBOSE | NTFS_LOG_LEVEL_PROGRESS))
		ours = 1;
	else
		ours = 2;
	vlog(ours, format, args);
	return 0;
}

static void log_once_init(void)
{
	ntfs_log_set_handler(ntfs3g_handler);
	ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR |
			NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_WARNING |
			NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_QUIET);
	/* We do our own Unicode handling in names.c */
	ntfs_macosx_normalize_filenames(0);
}

void n4m_log_init(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, log_once_init);
}
