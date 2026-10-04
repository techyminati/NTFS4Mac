/*
 * Extended attributes. Not wired up yet, frontends fall back to their
 * own handling when they get ENOTSUP.
 */
#include "internal.h"

int n4m_listxattr(n4m_volume *v, uint64_t ino, char *buf, size_t bufsz,
		size_t *len)
{
	(void)v; (void)ino; (void)buf; (void)bufsz;
	*len = 0;
	return ENOTSUP;
}

int n4m_getxattr(n4m_volume *v, uint64_t ino, const char *name,
		void *buf, size_t bufsz, size_t *len)
{
	(void)v; (void)ino; (void)name; (void)buf; (void)bufsz;
	*len = 0;
	return ENOTSUP;
}

int n4m_setxattr(n4m_volume *v, uint64_t ino, const char *name,
		const void *buf, size_t len, int flags)
{
	(void)v; (void)ino; (void)name; (void)buf; (void)len; (void)flags;
	return ENOTSUP;
}

int n4m_removexattr(n4m_volume *v, uint64_t ino, const char *name)
{
	(void)v; (void)ino; (void)name;
	return ENOTSUP;
}
