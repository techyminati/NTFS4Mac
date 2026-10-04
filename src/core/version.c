/*
 * Version information. The Makefile fills these in from the VERSION file
 * and git, so a binary can always tell exactly what it was built from.
 */
#include "internal.h"

#ifndef N4M_VERSION_STR
#define N4M_VERSION_STR "0.0.0"
#endif
#ifndef N4M_GIT
#define N4M_GIT "unknown"
#endif
#ifndef N4M_DATE
#define N4M_DATE "unknown"
#endif

const char *n4m_version(void)
{
	return N4M_VERSION_STR;
}

const char *n4m_version_long(void)
{
	return "NTFS4Mac " N4M_VERSION_STR " (" N4M_GIT ", " N4M_DATE ")";
}

const char *n4m_ntfs3g_version(void)
{
	return VERSION;	/* from libntfs-3g's config.h */
}
