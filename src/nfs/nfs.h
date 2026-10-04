/*
 * Local NFSv3 server that exposes one NTFS volume to the macOS NFS client.
 *
 * It only listens on 127.0.0.1 and only serves the volume it was started
 * for. The macOS kernel talks to it like to any NFS server, so no kernel
 * extension, macFUSE or system security change is needed.
 */
#ifndef N4M_NFS_H
#define N4M_NFS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "n4m.h"
#include "xdr.h"

/* RPC */
#define RPC_CALL		0
#define RPC_REPLY		1
#define RPC_MSG_ACCEPTED	0
#define RPC_MSG_DENIED		1
#define RPC_SUCCESS		0
#define RPC_PROG_UNAVAIL	1
#define RPC_PROG_MISMATCH	2
#define RPC_PROC_UNAVAIL	3
#define RPC_GARBAGE_ARGS	4
#define RPC_SYSTEM_ERR		5
#define RPC_MISMATCH		0

#define PROG_NFS		100003
#define PROG_MOUNT		100005

/* nfsstat3 */
#define NFS3_OK			0
#define NFS3ERR_PERM		1
#define NFS3ERR_NOENT		2
#define NFS3ERR_IO		5
#define NFS3ERR_NXIO		6
#define NFS3ERR_ACCES		13
#define NFS3ERR_EXIST		17
#define NFS3ERR_XDEV		18
#define NFS3ERR_NODEV		19
#define NFS3ERR_NOTDIR		20
#define NFS3ERR_ISDIR		21
#define NFS3ERR_INVAL		22
#define NFS3ERR_FBIG		27
#define NFS3ERR_NOSPC		28
#define NFS3ERR_ROFS		30
#define NFS3ERR_MLINK		31
#define NFS3ERR_NAMETOOLONG	63
#define NFS3ERR_NOTEMPTY	66
#define NFS3ERR_DQUOT		69
#define NFS3ERR_STALE		70
#define NFS3ERR_BADHANDLE	10001
#define NFS3ERR_NOT_SYNC	10002
#define NFS3ERR_BAD_COOKIE	10003
#define NFS3ERR_NOTSUPP		10004
#define NFS3ERR_TOOSMALL	10005
#define NFS3ERR_SERVERFAULT	10006

#define NFS_MAXDATA	(1024 * 1024)	/* rsize / wsize */
#define NFS_MAXNAME	1024		/* bytes, UTF-8 */
#define NFS_MAXPATH	4096
#define NFS_FHSIZE	16

struct nfs_server {
	n4m_volume *vol;
	uint64_t fsid;
	uint64_t root_ref;
	uint32_t uid, gid;
	bool readonly;
	uint8_t writeverf[8];
	/* set after a write, cleared by the periodic device flush */
	bool dirty;
	uint64_t last_flush_ms;
	/* bumped by every RPC, used to notice the client went quiet */
	uint64_t requests;
};

/* nfs3.c */
void nfs3_dispatch(struct nfs_server *s, uint32_t proc, struct xdr_in *in,
		struct xdr_out *out);
/* mount3.c */
void mount3_dispatch(struct nfs_server *s, uint32_t proc, struct xdr_in *in,
		struct xdr_out *out);
/* both write the accept_stat themselves; return false for PROC_UNAVAIL */

/* common helpers (nfs3.c) */
uint32_t nfs_errno_to_stat(int err);
void nfs_put_fh(struct nfs_server *s, struct xdr_out *out, uint64_t ref);

/* rpc.c */
struct rpc_server;
struct rpc_server *rpc_listen(struct nfs_server *s, uint16_t *port);
/*
 * Serves requests until stop() returns true. stop is polled about every
 * 250ms and after each batch of requests.
 */
int rpc_run(struct rpc_server *r, bool (*stop)(void *ctx), void *ctx);
void rpc_close(struct rpc_server *r);
/* number of client connections currently open */
int rpc_connections(struct rpc_server *r);

#endif /* N4M_NFS_H */
