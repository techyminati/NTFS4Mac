/*
 * Minimal XDR (RFC 4506) reader and writer for the NFS server.
 * Everything is big endian and padded to 4 bytes.
 */
#ifndef N4M_XDR_H
#define N4M_XDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct xdr_in {
	const uint8_t *p;
	size_t len;
	size_t pos;
	bool err;
};

struct xdr_out {
	uint8_t *p;
	size_t len;
	size_t cap;
	bool err;
};

static inline uint32_t xdr_get_u32(struct xdr_in *x)
{
	uint32_t v;

	if (x->err || x->len - x->pos < 4) {
		x->err = true;
		return 0;
	}
	v = (uint32_t)x->p[x->pos] << 24 | (uint32_t)x->p[x->pos + 1] << 16 |
		(uint32_t)x->p[x->pos + 2] << 8 | x->p[x->pos + 3];
	x->pos += 4;
	return v;
}

static inline uint64_t xdr_get_u64(struct xdr_in *x)
{
	uint64_t hi = xdr_get_u32(x);

	return hi << 32 | xdr_get_u32(x);
}

static inline bool xdr_get_bool(struct xdr_in *x)
{
	return xdr_get_u32(x) != 0;
}

/* Variable length opaque. Returns a pointer into the input buffer. */
static inline const uint8_t *xdr_get_opaque(struct xdr_in *x, size_t max,
		size_t *len)
{
	size_t n = xdr_get_u32(x), padded = (n + 3) & ~(size_t)3;
	const uint8_t *p;

	*len = 0;
	if (x->err || n > max || x->len - x->pos < padded) {
		x->err = true;
		return NULL;
	}
	p = x->p + x->pos;
	x->pos += padded;
	*len = n;
	return p;
}

static inline void xdr_get_fixed(struct xdr_in *x, void *out, size_t n)
{
	size_t padded = (n + 3) & ~(size_t)3;

	if (x->err || x->len - x->pos < padded) {
		x->err = true;
		memset(out, 0, n);
		return;
	}
	memcpy(out, x->p + x->pos, n);
	x->pos += padded;
}

/* String into a NUL terminated buffer of size bufsz. */
static inline bool xdr_get_string(struct xdr_in *x, char *buf, size_t bufsz)
{
	size_t n;
	const uint8_t *p = xdr_get_opaque(x, bufsz - 1, &n);

	if (!p) {
		buf[0] = 0;
		return false;
	}
	memcpy(buf, p, n);
	buf[n] = 0;
	/* embedded NULs are not valid in names */
	return strlen(buf) == n;
}

static inline bool xdr_reserve(struct xdr_out *x, size_t n)
{
	if (x->err)
		return false;
	if (x->len + n > x->cap) {
		size_t cap = x->cap ? x->cap : 4096;
		uint8_t *p;

		while (cap < x->len + n)
			cap *= 2;
		p = realloc(x->p, cap);
		if (!p) {
			x->err = true;
			return false;
		}
		x->p = p;
		x->cap = cap;
	}
	return true;
}

static inline void xdr_put_u32(struct xdr_out *x, uint32_t v)
{
	if (!xdr_reserve(x, 4))
		return;
	x->p[x->len++] = (uint8_t)(v >> 24);
	x->p[x->len++] = (uint8_t)(v >> 16);
	x->p[x->len++] = (uint8_t)(v >> 8);
	x->p[x->len++] = (uint8_t)v;
}

static inline void xdr_put_u64(struct xdr_out *x, uint64_t v)
{
	xdr_put_u32(x, (uint32_t)(v >> 32));
	xdr_put_u32(x, (uint32_t)v);
}

static inline void xdr_put_bool(struct xdr_out *x, bool v)
{
	xdr_put_u32(x, v ? 1 : 0);
}

static inline void xdr_put_fixed(struct xdr_out *x, const void *p, size_t n)
{
	size_t padded = (n + 3) & ~(size_t)3;

	if (!xdr_reserve(x, padded))
		return;
	memcpy(x->p + x->len, p, n);
	memset(x->p + x->len + n, 0, padded - n);
	x->len += padded;
}

static inline void xdr_put_opaque(struct xdr_out *x, const void *p, size_t n)
{
	xdr_put_u32(x, (uint32_t)n);
	xdr_put_fixed(x, p, n);
}

static inline void xdr_put_string(struct xdr_out *x, const char *s)
{
	xdr_put_opaque(x, s, strlen(s));
}

/* Patches a u32 written earlier at offset off. */
static inline void xdr_set_u32(struct xdr_out *x, size_t off, uint32_t v)
{
	if (x->err || off + 4 > x->len)
		return;
	x->p[off] = (uint8_t)(v >> 24);
	x->p[off + 1] = (uint8_t)(v >> 16);
	x->p[off + 2] = (uint8_t)(v >> 8);
	x->p[off + 3] = (uint8_t)v;
}

#endif /* N4M_XDR_H */
