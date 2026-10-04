/*
 * MOUNT v3 protocol (RFC 1813 appendix I). mount_nfs uses it once to get
 * the root file handle, we export exactly one path: "/".
 */
#include "nfs.h"

#define MNT3_OK		0
#define AUTH_UNIX	1

void mount3_dispatch(struct nfs_server *s, uint32_t proc, struct xdr_in *in,
		struct xdr_out *out)
{
	char path[1025];

	switch (proc) {
	case 0:		/* NULL */
		xdr_put_u32(out, RPC_SUCCESS);
		break;
	case 1:		/* MNT */
		xdr_get_string(in, path, sizeof(path));
		if (in->err) {
			xdr_put_u32(out, RPC_GARBAGE_ARGS);
			break;
		}
		xdr_put_u32(out, RPC_SUCCESS);
		xdr_put_u32(out, MNT3_OK);
		nfs_put_fh(s, out, s->root_ref);
		xdr_put_u32(out, 1);
		xdr_put_u32(out, AUTH_UNIX);
		break;
	case 2:		/* DUMP */
		xdr_put_u32(out, RPC_SUCCESS);
		xdr_put_bool(out, false);
		break;
	case 3:		/* UMNT */
	case 4:		/* UMNTALL */
		xdr_put_u32(out, RPC_SUCCESS);
		break;
	case 5:		/* EXPORT */
		xdr_put_u32(out, RPC_SUCCESS);
		xdr_put_bool(out, true);
		xdr_put_string(out, "/");
		xdr_put_bool(out, false);	/* no group restrictions */
		xdr_put_bool(out, false);	/* no more exports */
		break;
	default:
		xdr_put_u32(out, RPC_PROC_UNAVAIL);
		break;
	}
}
