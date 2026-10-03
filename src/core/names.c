/*
 * File name handling.
 *
 * macOS hands us UTF-8, NTFS stores UTF-16LE. On top of the plain
 * conversion we do two things so files survive the trip between Mac and
 * Windows:
 *
 *  1. Characters that are legal on macOS but illegal in Windows names
 *     (" * : < > ? \ | and control characters, plus a trailing space or
 *     dot) are stored as Unicode private use characters U+F001..U+F029.
 *     This is the "Services for Mac" mapping that Windows itself and
 *     Apple's SMB client use, so Windows shows such files fine and they
 *     come back with the right name on the Mac.
 *
 *  2. Lone UTF-16 surrogates (Windows allows them) are passed through as
 *     WTF-8 so even broken names can still be opened, renamed and deleted.
 *
 * Unicode normalization (NFC/NFD) is done with CoreFoundation.
 */
#include <CoreFoundation/CoreFoundation.h>

#include "internal.h"

#define NTFS_NAME_MAX 255	/* UTF-16 code units */

static const unsigned char sfm_ascii[] = {
	'"', '*', ':', '<', '>', '?', '\\', '|',	/* U+F020..U+F027 */
};

static inline uint16_t sfm_encode(uint32_t c, bool last)
{
	unsigned i;

	if (c >= 0x01 && c <= 0x1f)
		return (uint16_t)(0xf000 + c);
	for (i = 0; i < sizeof(sfm_ascii); i++)
		if (c == sfm_ascii[i])
			return (uint16_t)(0xf020 + i);
	if (last && c == ' ')
		return 0xf028;
	if (last && c == '.')
		return 0xf029;
	return 0;
}

static inline uint32_t sfm_decode(uint16_t c, bool last)
{
	if (c >= 0xf001 && c <= 0xf01f)
		return c - 0xf000;
	if (c >= 0xf020 && c <= 0xf027)
		return sfm_ascii[c - 0xf020];
	if (last && c == 0xf028)
		return ' ';
	if (last && c == 0xf029)
		return '.';
	return c;
}

/*
 * Decodes one UTF-8 sequence. Accepts encoded surrogates (WTF-8).
 * Returns the number of bytes used, or 0 on invalid input.
 */
static size_t utf8_decode(const unsigned char *s, size_t len, uint32_t *cp)
{
	uint32_t c;
	size_t n, i;

	if (!len)
		return 0;
	c = s[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	} else if ((c & 0xe0) == 0xc0) {
		n = 2;
		c &= 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		n = 3;
		c &= 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		n = 4;
		c &= 0x07;
	} else {
		return 0;
	}
	if (len < n)
		return 0;
	for (i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return 0;
		c = (c << 6) | (s[i] & 0x3f);
	}
	/* reject overlong forms and out of range values */
	if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) ||
			(n == 4 && (c < 0x10000 || c > 0x10ffff)))
		return 0;
	*cp = c;
	return n;
}

static int to_utf16(const char *name, size_t len, bool map, ntfschar **out,
		int *outlen)
{
	const unsigned char *s = (const unsigned char *)name;
	ntfschar *u;
	size_t i = 0;
	int n = 0;

	/* every UTF-8 byte produces at most one UTF-16 unit */
	u = malloc((len + 1) * sizeof(ntfschar));
	if (!u)
		return ENOMEM;
	while (i < len) {
		uint32_t c;
		size_t k = utf8_decode(s + i, len - i, &c);
		uint16_t m;

		if (!k) {
			free(u);
			return EILSEQ;
		}
		i += k;
		m = map ? sfm_encode(c, i == len) : 0;
		if (m) {
			u[n++] = cpu_to_le16(m);
		} else if (c >= 0x10000) {
			c -= 0x10000;
			u[n++] = cpu_to_le16((uint16_t)(0xd800 + (c >> 10)));
			u[n++] = cpu_to_le16((uint16_t)(0xdc00 + (c & 0x3ff)));
		} else {
			u[n++] = cpu_to_le16((uint16_t)c);
		}
	}
	u[n] = 0;
	*out = u;
	*outlen = n;
	return 0;
}

static int from_utf16(const ntfschar *name, int len, bool map, char **out,
		size_t *outlen)
{
	char *s;
	size_t n = 0;
	int i;

	s = malloc((size_t)len * 3 + 1);
	if (!s)
		return ENOMEM;
	for (i = 0; i < len; i++) {
		uint32_t c = le16_to_cpu(name[i]);

		if (c >= 0xd800 && c <= 0xdbff && i + 1 < len) {
			uint32_t d = le16_to_cpu(name[i + 1]);

			if (d >= 0xdc00 && d <= 0xdfff) {
				c = 0x10000 + ((c - 0xd800) << 10) +
					(d - 0xdc00);
				i++;
			}
		} else if (map) {
			c = sfm_decode((uint16_t)c, i == len - 1);
		}
		if (c < 0x80) {
			s[n++] = (char)c;
		} else if (c < 0x800) {
			s[n++] = (char)(0xc0 | (c >> 6));
			s[n++] = (char)(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			s[n++] = (char)(0xe0 | (c >> 12));
			s[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
			s[n++] = (char)(0x80 | (c & 0x3f));
		} else {
			s[n++] = (char)(0xf0 | (c >> 18));
			s[n++] = (char)(0x80 | ((c >> 12) & 0x3f));
			s[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
			s[n++] = (char)(0x80 | (c & 0x3f));
		}
	}
	s[n] = 0;
	*out = s;
	if (outlen)
		*outlen = n;
	return 0;
}

int n4m_name_to_ntfs(const char *name, size_t len, ntfschar **out,
		int *outlen)
{
	int err = to_utf16(name, len, true, out, outlen);

	if (!err && *outlen > NTFS_NAME_MAX) {
		free(*out);
		*out = NULL;
		return ENAMETOOLONG;
	}
	return err;
}

int n4m_name_from_ntfs(const ntfschar *name, int len, char **out,
		size_t *outlen)
{
	return from_utf16(name, len, true, out, outlen);
}

int n4m_utf8_to_utf16(const char *s, size_t len, ntfschar **out, int *outlen)
{
	return to_utf16(s, len, false, out, outlen);
}

int n4m_utf16_to_utf8(const ntfschar *s, int len, char **out,
		size_t *outlen)
{
	return from_utf16(s, len, false, out, outlen);
}

char *n4m_normalize(const char *s, size_t len, char form)
{
	CFStringRef str;
	CFMutableStringRef m;
	CFIndex max;
	char *out = NULL;
	size_t i;

	for (i = 0; i < len; i++)
		if ((unsigned char)s[i] >= 0x80)
			break;
	if (i == len)
		return NULL;	/* plain ASCII is always normalized */
	str = CFStringCreateWithBytes(NULL, (const UInt8 *)s, (CFIndex)len,
			kCFStringEncodingUTF8, false);
	if (!str)
		return NULL;
	m = CFStringCreateMutableCopy(NULL, 0, str);
	CFRelease(str);
	if (!m)
		return NULL;
	CFStringNormalize(m, form == 'D' ? kCFStringNormalizationFormD :
			kCFStringNormalizationFormC);
	max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(m),
			kCFStringEncodingUTF8) + 1;
	out = malloc((size_t)max);
	if (out && !CFStringGetCString(m, out, max, kCFStringEncodingUTF8)) {
		free(out);
		out = NULL;
	}
	CFRelease(m);
	if (out && strlen(out) == len && !memcmp(out, s, len)) {
		free(out);
		out = NULL;
	}
	return out;
}

int n4m_check_name(const char *name, size_t len)
{
	if (!len)
		return EINVAL;
	if (memchr(name, '/', len) || memchr(name, 0, len))
		return EINVAL;
	if ((len == 1 && name[0] == '.') ||
			(len == 2 && name[0] == '.' && name[1] == '.'))
		return EINVAL;
	if (len > NTFS_NAME_MAX * 4)
		return ENAMETOOLONG;
	return 0;
}
