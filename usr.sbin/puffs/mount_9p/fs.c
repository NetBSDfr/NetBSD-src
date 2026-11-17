/*	$NetBSD: fs.c,v 1.11 2020/05/28 14:00:05 uwe Exp $	*/

/*
 * Copyright (c) 2007  Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#ifndef lint
__RCSID("$NetBSD: fs.c,v 1.11 2020/05/28 14:00:05 uwe Exp $");
#endif /* !lint */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <puffs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ninepuffs.h"
#include "nineproto.h"

#define DO_IO(fname, a1, a2, a3, a4, rv)				\
	puffs_framebuf_seekset(a2, 0);					\
	*(a4) = 0;							\
	rv = fname(a1, a2, a3, a4);					\
	if (rv) errx(1, "p9p_handshake io failed %d, %d", rv, *a4) 

static const char *
p9p_ver2str(int version)
{

	switch (version) {
	case P9PROTO_VERSION:	return P9PROTO_VERSTR;
	case P9PROTO_VERSION_U:	return P9PROTO_VERSTR_U;
	}
	return NULL;
}

struct puffs_node *
p9p_handshake(struct puffs_usermount *pu,
	const char *username, const char *path)
{
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct puffs_framebuf *pb;
	struct puffs_node *pn;
	struct vattr rootva;
	uint32_t maxreq;
	uint16_t dummy;
	p9ptag_t tagid, rtagid;
	p9pfid_t curfid;
	const char *p;
	uint8_t type;
	int rv, done, x = 1, ncomp;
	uint16_t strsize;
	char *str;

	/* send initial handshake */
	pb = p9pbuf_makeout();
	p9pbuf_put_1(pb, P9PROTO_T_VERSION);
	p9pbuf_put_2(pb, P9PROTO_NOTAG);
	p9pbuf_put_4(pb, p9p->maxreq);
	p9pbuf_put_str(pb, p9p_ver2str(p9p->protover));
	DO_IO(p9pbuf_write, pu, pb, p9p->servsock, &done, rv);

	puffs_framebuf_recycle(pb);
	DO_IO(p9pbuf_read, pu, pb, p9p->servsock, &done, rv);

	if ((type = p9pbuf_get_type(pb)) != P9PROTO_R_VERSION)
		errx(1, "server invalid response to Tversion: %d", type);
	if ((rtagid = p9pbuf_get_tag(pb)) != P9PROTO_NOTAG) {
		errx(1, "server invalid tag: %d vs. %d",
		    P9PROTO_NOTAG, rtagid);
		return NULL;
	}
	if (p9pbuf_get_4(pb, &maxreq))
		errx(1, "server invalid response: no request length");
	if (maxreq < P9P_MINREQLEN)
		errx(1, "server request length below minimum accepted: "
		    "%d vs. %d", P9P_MINREQLEN, maxreq);
	p9p->maxreq = maxreq;

	if (p9pbuf_get_str(pb, &str, &strsize))
		errx(1, "server invalid response: no version");
	if (strncmp(str, p9p_ver2str(p9p->protover), P9PROTO_VERSTR_MAXLEN) != 0) {
		errx(1, "server doesn't support %s", p9p_ver2str(p9p->protover));
		/* Should downgrade from 9P2000.u to 9P2000 if the server request? */
	}

	/* build attach message */
	p9pbuf_recycleout(pb);
	tagid = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_ATTACH);
	p9pbuf_put_2(pb, tagid);
	p9pbuf_put_4(pb, P9P_ROOTFID);
	p9pbuf_put_4(pb, P9PROTO_NOFID);
	p9pbuf_put_str(pb, username);
	p9pbuf_put_str(pb, "");
	if (p9p->protover == P9PROTO_VERSION_U)
		p9pbuf_put_4(pb, P9PROTO_NUNAME_UNSPECIFIED); /* n_uname[4] */
	DO_IO(p9pbuf_write, pu, pb, p9p->servsock, &done, rv);

	puffs_framebuf_recycle(pb);
	DO_IO(p9pbuf_read, pu, pb, p9p->servsock, &done, rv);

	if ((type = p9pbuf_get_type(pb)) != P9PROTO_R_ATTACH)
		errx(1, "Rattach not received, got %d", type);
	if ((rtagid = p9pbuf_get_tag(pb)) != tagid)
		errx(1, "server invalid tag: %d vs. %d", tagid, rtagid);

	/* just walk away rootfid, you won't see me follow you back home */

#define EATSLASH(p) while (*(p) == '/') p++
	assert(*path == '/');
	p = path;
	EATSLASH(p);
	for (ncomp = 0; p && *p; ncomp++) {
		EATSLASH(p);
		if (!*p)
			break;
		p = strchr(p, '/');
	}

	if (ncomp == 0) {
		curfid = P9P_ROOTFID;
	} else {
		uint16_t walked;

		p9pbuf_recycleout(pb);
		tagid = NEXTTAG(p9p);
		curfid = NEXTFID(p9p);
		p9pbuf_put_1(pb, P9PROTO_T_WALK);
		p9pbuf_put_2(pb, tagid);
		p9pbuf_put_4(pb, P9P_ROOTFID);
		p9pbuf_put_4(pb, curfid);
		p9pbuf_put_2(pb, ncomp);

		p = path;
		while (p && *p) {
			char *p2;

			EATSLASH(p);
			if (!*p)
				break;
			if ((p2 = strchr(p, '/')) == NULL)
				p2 = strchr(p, '\0');
			p9pbuf_put_data(pb, p, p2-p);
			p = p2;
		}

		DO_IO(p9pbuf_write, pu, pb, p9p->servsock, &done, rv);

		puffs_framebuf_recycle(pb);
		DO_IO(p9pbuf_read, pu, pb, p9p->servsock, &done, rv);

		if ((type = p9pbuf_get_type(pb)) != P9PROTO_R_WALK)
			errx(1, "Rwalk not received for rnode, got %d", type);
		if ((rtagid = p9pbuf_get_tag(pb)) != tagid)
			errx(1, "server invalid tag: %d vs. %d",
			    tagid, rtagid);
		if (p9pbuf_get_2(pb, &walked) == -1)
			errx(1, "can't get number of walked qids");
		if (walked != ncomp)
			errx(1, "can't locate rootpath %s, only %d/%d "
			    "components found", path, walked, ncomp);
		
		/* curfid is alive, clunk P9P_ROOTFID */
		p9pbuf_recycleout(pb);
		tagid = NEXTTAG(p9p);
		p9pbuf_put_1(pb, P9PROTO_T_CLUNK);
		p9pbuf_put_2(pb, tagid);
		p9pbuf_put_4(pb, P9P_ROOTFID);

		DO_IO(p9pbuf_write, pu, pb, p9p->servsock, &done, rv);
		puffs_framebuf_recycle(pb);
		DO_IO(p9pbuf_read, pu, pb, p9p->servsock, &done, rv);
		/* wedontcare */
	}

	/* finally, stat the node */
	p9pbuf_recycleout(pb);
	tagid = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_STAT);
	p9pbuf_put_2(pb, tagid);
	p9pbuf_put_4(pb, curfid);
	DO_IO(p9pbuf_write, pu, pb, p9p->servsock, &done, rv);

	puffs_framebuf_recycle(pb);
	DO_IO(p9pbuf_read, pu, pb, p9p->servsock, &done, rv);

	if ((type = p9pbuf_get_type(pb)) != P9PROTO_R_STAT)
		errx(1, "Rstat not received, got %d", type);
	if ((rtagid = p9pbuf_get_tag(pb)) != tagid)
		errx(1, "server invalid tag: %d vs. %d", tagid, rtagid);
	if (p9pbuf_get_2(pb, &dummy))
		errx(1, "couldn't get stat len parameter");
	if (proto_getstat(pu, pb, &rootva, NULL, NULL))
		errx(1, "could not parse root attributes");
	puffs_framebuf_destroy(pb);

	rootva.va_nlink = 0156; /* guess, will be fixed with first readdir */
	pn = newp9pnode_va(pu, &rootva, curfid);
	p9p->root_fid = curfid;

	if (ioctl(p9p->servsock, FIONBIO, &x) == -1)
		err(1, "cannot set socket in nonblocking mode");

	return pn;
}

int
puffs9p_fs_unmount(struct puffs_usermount *pu, int flags)
{
	struct puffs9p *p9p = puffs_getspecific(pu);

	close(p9p->servsock);
	return 0;
}

/*
 * df(1) and friends need filesystem stats.  9P2000(.u) has no such
 * request, so we use the 9P2000.L Tstatfs on the mount root's fid,
 * exactly like the TREADLINK trick in node.c: servers that dispatch
 * by opcode (qemu) serve it even under a 9P2000.u negotiation.  A
 * server without .L answers Rerror; we then report plausible zeros
 * rather than failing statvfs(2) outright, since the statvfs-noop
 * behavior before this meant the same zeros anyway.
 */
int
puffs9p_fs_statvfs(struct puffs_usermount *pu, struct puffs_statvfs *svfsb)
{
	AUTOVAR(pu);
	uint32_t fstype, bsize, namelen;
	uint64_t blocks, bfree, bavail, files, ffree, fsid;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_L_STATFS);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9p->root_fid);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) == P9PROTO_R_ERROR) {
		(void) proto_handle_rerror(pu, pb);	/* prints string */
		goto unsupported;
	}

	if (p9pbuf_get_type(pb) != P9PROTO_R_L_STATFS ||
	    p9pbuf_get_tag(pb) != tag ||
	    p9pbuf_get_4(pb, &fstype) == -1 ||
	    p9pbuf_get_4(pb, &bsize) == -1 ||
	    p9pbuf_get_8(pb, &blocks) == -1 ||
	    p9pbuf_get_8(pb, &bfree) == -1 ||
	    p9pbuf_get_8(pb, &bavail) == -1 ||
	    p9pbuf_get_8(pb, &files) == -1 ||
	    p9pbuf_get_8(pb, &ffree) == -1 ||
	    p9pbuf_get_8(pb, &fsid) == -1 ||
	    p9pbuf_get_4(pb, &namelen) == -1) {
		goto unsupported;
	}

	/* some servers send 0 for bsize; df divides by f_frsize */
	if (bsize == 0)
		bsize = DEV_BSIZE;

	/* leave f_flag, f_owner and the names as the kernel sent them */
	svfsb->f_bsize = bsize;
	svfsb->f_frsize = bsize;
	svfsb->f_iosize = bsize;
	svfsb->f_blocks = blocks;
	svfsb->f_bfree = bfree;
	svfsb->f_bavail = bavail;
	svfsb->f_bresvd = bfree > bavail ? bfree - bavail : 0;
	svfsb->f_files = files;
	svfsb->f_ffree = ffree;
	svfsb->f_favail = ffree;
	svfsb->f_fresvd = files > ffree ? files - ffree : 0;
	if (namelen > 0)
		svfsb->f_namemax = namelen;
	svfsb->f_fsidx.__fsid_val[0] = (int32_t)fsid;
	svfsb->f_fsidx.__fsid_val[1] = (int32_t)(fsid >> 32);
	svfsb->f_fsid = (unsigned long)fsid;
	goto out;

 unsupported:
	/* pre-.L behavior: zeros, but with a non-zero frsize for df */
	svfsb->f_bsize = svfsb->f_frsize = svfsb->f_iosize = DEV_BSIZE;
	svfsb->f_blocks = svfsb->f_bfree = svfsb->f_bavail = 0;
	svfsb->f_bresvd = 0;
	svfsb->f_files = svfsb->f_ffree = svfsb->f_favail = 0;
	svfsb->f_fresvd = 0;
	rv = 0;

 out:
	RETURN(rv);
}
