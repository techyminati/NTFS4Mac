/*
 * ONC RPC over TCP (RFC 5531): record marking, call parsing, dispatch.
 *
 * Single threaded on purpose. The engine serializes everything anyway and
 * the client is the local kernel, so one loop with poll() is plenty.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "nfs.h"

#define MAX_CONNS	16
#define MAX_RECORD	(NFS_MAXDATA + 64 * 1024)
#define READ_CHUNK	(256 * 1024)
#define FLUSH_MS	2000

struct conn {
	int fd;
	uint8_t *in;		/* raw bytes from the socket */
	size_t inlen, incap;
	uint8_t *rec;		/* reassembled record */
	size_t reclen, reccap;
};

struct rpc_server {
	struct nfs_server *s;
	int lfd;
	struct conn conns[MAX_CONNS];
	int nconns;
	struct xdr_out out;
};

static uint64_t now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

struct rpc_server *rpc_listen(struct nfs_server *s, uint16_t *port)
{
	struct rpc_server *r = calloc(1, sizeof(*r));
	struct sockaddr_in sin = { 0 };
	socklen_t slen = sizeof(sin);
	int one = 1;

	if (!r)
		return NULL;
	r->s = s;
	r->lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (r->lfd < 0)
		goto fail;
	setsockopt(r->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sin.sin_family = AF_INET;
	sin.sin_len = sizeof(sin);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(*port);
	if (bind(r->lfd, (struct sockaddr *)&sin, sizeof(sin)) ||
			listen(r->lfd, 8) ||
			getsockname(r->lfd, (struct sockaddr *)&sin, &slen))
		goto fail;
	*port = ntohs(sin.sin_port);
	s->last_flush_ms = now_ms();
	return r;
fail:
	if (r->lfd >= 0)
		close(r->lfd);
	free(r);
	return NULL;
}

static void conn_close(struct rpc_server *r, int i)
{
	struct conn *c = &r->conns[i];

	close(c->fd);
	free(c->in);
	free(c->rec);
	r->conns[i] = r->conns[--r->nconns];
	memset(&r->conns[r->nconns], 0, sizeof(struct conn));
}

int rpc_connections(struct rpc_server *r)
{
	return r->nconns;
}

static bool send_all(int fd, const uint8_t *p, size_t len)
{
	while (len) {
		ssize_t n = send(fd, p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

/* Handles one complete RPC call record. Returns false to drop the conn. */
static bool handle_call(struct rpc_server *r, struct conn *c)
{
	struct xdr_in in = { .p = c->rec, .len = c->reclen };
	struct xdr_out *out = &r->out;
	uint32_t xid, mtype, rpcvers, prog, vers, proc;
	size_t n;

	xid = xdr_get_u32(&in);
	mtype = xdr_get_u32(&in);
	if (in.err || mtype != RPC_CALL)
		return !in.err;		/* ignore stray replies */
	rpcvers = xdr_get_u32(&in);
	prog = xdr_get_u32(&in);
	vers = xdr_get_u32(&in);
	proc = xdr_get_u32(&in);
	xdr_get_u32(&in);			/* cred flavor */
	xdr_get_opaque(&in, 400, &n);		/* cred body */
	xdr_get_u32(&in);			/* verifier flavor */
	xdr_get_opaque(&in, 400, &n);		/* verifier body */

	out->len = 0;
	out->err = false;
	xdr_put_u32(out, 0);			/* record mark, set below */
	xdr_put_u32(out, xid);
	xdr_put_u32(out, RPC_REPLY);
	if (rpcvers != 2) {
		xdr_put_u32(out, RPC_MSG_DENIED);
		xdr_put_u32(out, RPC_MISMATCH);
		xdr_put_u32(out, 2);
		xdr_put_u32(out, 2);
	} else {
		xdr_put_u32(out, RPC_MSG_ACCEPTED);
		xdr_put_u32(out, 0);		/* AUTH_NONE verifier */
		xdr_put_u32(out, 0);
		if (in.err) {
			xdr_put_u32(out, RPC_GARBAGE_ARGS);
		} else if (prog == PROG_NFS || prog == PROG_MOUNT) {
			if (vers != 3) {
				xdr_put_u32(out, RPC_PROG_MISMATCH);
				xdr_put_u32(out, 3);
				xdr_put_u32(out, 3);
			} else if (prog == PROG_NFS) {
				nfs3_dispatch(r->s, proc, &in, out);
			} else {
				mount3_dispatch(r->s, proc, &in, out);
			}
		} else {
			xdr_put_u32(out, RPC_PROG_UNAVAIL);
		}
	}
	if (out->err)
		return false;
	xdr_set_u32(out, 0, 0x80000000u | (uint32_t)(out->len - 4));
	return send_all(c->fd, out->p, out->len);
}

/* Pulls complete records out of the input buffer. */
static bool process_input(struct rpc_server *r, struct conn *c)
{
	size_t pos = 0;

	while (c->inlen - pos >= 4) {
		const uint8_t *h = c->in + pos;
		uint32_t mark = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 |
			(uint32_t)h[2] << 8 | h[3];
		size_t frag = mark & 0x7fffffffu;
		bool last = (mark & 0x80000000u) != 0;

		if (frag > MAX_RECORD || c->reclen + frag > MAX_RECORD)
			return false;
		if (c->inlen - pos - 4 < frag)
			break;		/* wait for the rest */
		if (c->reclen + frag > c->reccap) {
			size_t cap = c->reclen + frag + 4096;
			uint8_t *p = realloc(c->rec, cap);

			if (!p)
				return false;
			c->rec = p;
			c->reccap = cap;
		}
		memcpy(c->rec + c->reclen, h + 4, frag);
		c->reclen += frag;
		pos += 4 + frag;
		if (last) {
			bool ok = handle_call(r, c);

			c->reclen = 0;
			if (!ok)
				return false;
		}
	}
	if (pos) {
		memmove(c->in, c->in + pos, c->inlen - pos);
		c->inlen -= pos;
	}
	return true;
}

static bool conn_read(struct rpc_server *r, struct conn *c)
{
	ssize_t n;

	if (c->incap - c->inlen < READ_CHUNK) {
		size_t cap = c->inlen + READ_CHUNK * 2;
		uint8_t *p = realloc(c->in, cap);

		if (!p)
			return false;
		c->in = p;
		c->incap = cap;
	}
	n = recv(c->fd, c->in + c->inlen, c->incap - c->inlen, 0);
	if (n < 0 && errno == EINTR)
		return true;
	if (n <= 0)
		return false;
	c->inlen += (size_t)n;
	return process_input(r, c);
}

static void accept_conn(struct rpc_server *r)
{
	int fd = accept(r->lfd, NULL, NULL);
	int one = 1, bufsz = 4 * 1024 * 1024;

	if (fd < 0)
		return;
	if (r->nconns == MAX_CONNS) {
		close(fd);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
	r->conns[r->nconns++] = (struct conn){ .fd = fd };
}

static void maybe_flush(struct rpc_server *r, bool force)
{
	struct nfs_server *s = r->s;
	uint64_t now = now_ms();

	if (s->dirty && (force || now - s->last_flush_ms >= FLUSH_MS)) {
		n4m_sync(s->vol);
		s->dirty = false;
		s->last_flush_ms = now;
	}
}

int rpc_run(struct rpc_server *r, bool (*stop)(void *ctx), void *ctx)
{
	struct pollfd pfd[MAX_CONNS + 1];
	int i, n;

	while (!stop(ctx)) {
		pfd[0] = (struct pollfd){ .fd = r->lfd, .events = POLLIN };
		for (i = 0; i < r->nconns; i++)
			pfd[i + 1] = (struct pollfd){ .fd = r->conns[i].fd,
				.events = POLLIN };
		n = poll(pfd, (nfds_t)r->nconns + 1, 250);
		if (n < 0 && errno != EINTR)
			return errno;
		if (n > 0) {
			int count = r->nconns;

			/* walk backwards, conn_close moves the last entry */
			for (i = count - 1; i >= 0; i--) {
				if (!(pfd[i + 1].revents &
						(POLLIN | POLLHUP | POLLERR)))
					continue;
				if (!conn_read(r, &r->conns[i]))
					conn_close(r, i);
			}
			if (pfd[0].revents & POLLIN)
				accept_conn(r);
		}
		maybe_flush(r, false);
	}
	maybe_flush(r, true);
	return 0;
}

void rpc_close(struct rpc_server *r)
{
	if (!r)
		return;
	while (r->nconns)
		conn_close(r, r->nconns - 1);
	close(r->lfd);
	free(r->out.p);
	free(r);
}
