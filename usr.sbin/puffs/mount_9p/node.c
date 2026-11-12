/*	$NetBSD: node.c,v 1.32 2025/03/10 21:21:20 riastradh Exp $	*/

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
__RCSID("$NetBSD: node.c,v 1.32 2025-03-10 21:21:20 riastradh Exp $");
#endif /* !lint */

#include <sys/param.h>

#include <assert.h>
#include <errno.h>
#include <puffs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ninepuffs.h"
#include "nineproto.h"

static void *
nodecmp(struct puffs_usermount *pu, struct puffs_node *pn, void *arg)
{
	struct vattr *vap = &pn->pn_va;
	struct qid9p *qid = arg;

	if (vap->va_fileid == qid->qidpath && vap->va_gen == qid->qidvers)
		return pn;

	return NULL;
}

static int
do_getattr(struct puffs_usermount *pu, struct puffs_node *pn, struct vattr *vap)
{
	AUTOVAR(pu);
	struct p9pnode *p9n = pn->pn_data;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_STAT);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n->fid_base);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_STAT) {
		rv = proto_handle_rerror(pu, pb);
		goto out;
	}
	rv = proto_expect_stat(pu, pb, vap);

 out:
	RETURN(rv);
}

int
puffs9p_node_getattr(struct puffs_usermount *pu, void *opc, struct vattr *vap,
	const struct puffs_cred *pcr)
{
	struct puffs_node *pn = opc;
	int rv;

	rv = do_getattr(pu, pn, &pn->pn_va);
	if (rv == 0)
		memcpy(vap, &pn->pn_va, sizeof(struct vattr));
	return rv;
}

int
puffs9p_node_lookup(struct puffs_usermount *pu, void *opc, struct puffs_newinfo *pni,
	const struct puffs_cn *pcn)
{
	AUTOVAR(pu);
	struct vattr va;
	struct puffs_node *pn, *pn_dir = opc;
	struct p9pnode *p9n_dir = pn_dir->pn_data;
	p9pfid_t tfid = NEXTFID(p9p);
	struct qid9p newqid;
	uint16_t nqid;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_WALK);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n_dir->fid_base);
	p9pbuf_put_4(pb, tfid);
	p9pbuf_put_2(pb, 1);
	p9pbuf_put_str(pb, pcn->pcn_name);
	GETRESPONSE(pb);

	rv = proto_expect_walk_nqids(pu, pb, &nqid);
	if (rv) {
		rv = ENOENT;
		goto out;
	}
	if (nqid != 1) {
		rv = EPROTO;
		goto out;
	}
	if ((rv = proto_getqid(pb, &newqid)))
		goto out;

	/* we get the parent vers in walk(?)  compensate */
	p9pbuf_recycleout(pb);
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_STAT);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, tfid);
	GETRESPONSE(pb);
	if ((rv = proto_expect_stat(pu, pb, &va)) != 0) {
		proto_cc_clunkfid(pu, tfid, 0);
		rv = ENOENT;
		goto out;
	}
	if (newqid.qidpath != va.va_fileid) {
		proto_cc_clunkfid(pu, tfid, 0);
		rv = EPROTO;
		goto out;
	}
	newqid.qidvers = va.va_gen;

	pn = puffs_pn_nodewalk(pu, nodecmp, &newqid);
	if (pn == NULL)
		pn = newp9pnode_qid(pu, &newqid, tfid);
	else
		proto_cc_clunkfid(pu, tfid, 0);
	/* assert pn */
	memcpy(&pn->pn_va, &va, sizeof(va));

	puffs_newinfo_setcookie(pni, pn);
	puffs_newinfo_setvtype(pni, pn->pn_va.va_type);
	puffs_newinfo_setsize(pni, pn->pn_va.va_size);
	puffs_newinfo_setrdev(pni, pn->pn_va.va_rdev);

 out:
	RETURN(rv);
}

/*
 * Problem is that 9P doesn't allow seeking into a directory.  So we
 * maintain a list of active fids for any given directory.  They
 * start living at the first read and exist either until the directory
 * is closed or until they reach the end.
 */
int
puffs9p_node_readdir(struct puffs_usermount *pu, void *opc, struct dirent *dent,
	off_t *readoff, size_t *reslen, const struct puffs_cred *pcr,
	int *eofflag, off_t *cookies, size_t *ncookies)
{
	AUTOVAR(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;
	struct vattr va;
	struct dirfid *dfp;
	char *name;
	uint32_t count;
	uint16_t statsize;

	rv = getdfwithoffset(pu, p9n, *readoff, &dfp);
	if (rv)
		goto out;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_READ);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, dfp->fid);
	p9pbuf_put_8(pb, *readoff);
	p9pbuf_put_4(pb, *reslen); /* XXX */
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_READ) {
		rv = proto_handle_rerror(pu, pb);
		goto out;
	}

	p9pbuf_get_4(pb, &count);

	/*
	 * if count is 0, assume we at end-of-dir.  dfp is no longer
	 * useful, so nuke it
	 */
	if (count == 0) {
		*eofflag = 1;
		releasedf(pu, dfp);
		goto out;
	}

	while (count > 0) {
		if ((rv = proto_getstat(pu, pb, &va, &name, &statsize))) {
			/*
			 * If there was an error, it's unlikely we'll be
			 * coming back, so just nuke the dfp.  If we do
			 * come back for some strange reason, we'll just
			 * regen it.
			 */
			releasedf(pu, dfp);
			goto out;
		}

		puffs_nextdent(&dent, name, va.va_fileid,
		    puffs_vtype2dt(va.va_type), reslen);

		count -= statsize;
		*readoff += statsize;
		dfp->seekoff += statsize;
		free(name);
	}

	storedf(p9n, dfp);

 out:
	RETURN(rv);
}

int
puffs9p_node_setattr(struct puffs_usermount *pu, void *opc,
	const struct vattr *va, const struct puffs_cred *pcr)
{
	AUTOVAR(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_WSTAT);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n->fid_base);
	proto_make_stat(pu, pb, va, NULL, pn->pn_va.va_type);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_WSTAT) {
		rv = proto_handle_rerror(pu, pb);
	} else {
		/*
		 * Refresh the attribute mirror from the server so that
		 * e.g. a truncate is visible to subsequent reads (which
		 * use pn_va as the upper bound).  Not exactly cheap, but
		 * a stale mirror loses file status.
		 */
		do_getattr(pu, pn, &pn->pn_va);
	}

 out:
	RETURN(rv);
}

/*
 * Ok, time to get clever.  There are two possible cases: we are
 * opening a file or we are opening a directory.
 *
 * If it's a directory, don't bother opening it here, but rather
 * wait until readdir, since it's probable we need to be able to
 * open a directory there in any case.
 * 
 * If it's a regular file, open it here with whatever credentials
 * we happen to have.   Let the upper layers of the kernel worry
 * about permission control.
 */
int
puffs9p_node_open(struct puffs_usermount *pu, void *opc, int mode,
	const struct puffs_cred *pcr)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;
	p9pfid_t nfid;
	int error = 0;

	puffs_setback(pcc, PUFFS_SETBACK_INACT_N1);
	if (pn->pn_va.va_type != VDIR) {
		/* Always require read access for page cache */
		mode |= FREAD;
		if (mode & FREAD && p9n->fid_read == P9P_INVALFID) {
			nfid = NEXTFID(p9p);
			error = proto_cc_open(pu, p9n->fid_base, nfid,
			    P9PROTO_OMODE_READ);
			if (error)
				return error;
			p9n->fid_read = nfid;
		}
		if (mode & FWRITE && p9n->fid_write == P9P_INVALFID) {
			nfid = NEXTFID(p9p);
			error = proto_cc_open(pu, p9n->fid_base, nfid,
			    P9PROTO_OMODE_WRITE);
			if (error)
				return error;
			p9n->fid_write = nfid;
		}
	}

	return 0;
}

int
puffs9p_node_inactive(struct puffs_usermount *pu, void *opc)
{
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;

	if (pn->pn_va.va_type == VDIR) {
		nukealldf(pu, p9n);
	} else  {
		if (p9n->fid_read != P9P_INVALFID) {
			proto_cc_clunkfid(pu, p9n->fid_read, 0);
			p9n->fid_read = P9P_INVALFID;
		}
		if (p9n->fid_write != P9P_INVALFID) {
			proto_cc_clunkfid(pu, p9n->fid_write, 0);
			p9n->fid_write = P9P_INVALFID;
		}
	}

	return 0;
}

int
puffs9p_node_read(struct puffs_usermount *pu, void *opc, uint8_t *buf,
	off_t offset, size_t *resid, const struct puffs_cred *pcr,
	int ioflag)
{
	AUTOVAR(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;
	uint32_t count;
	size_t nread;

	nread = 0;
	while (*resid > 0 && (uint64_t)(offset+nread) < pn->pn_va.va_size) {
		tag = NEXTTAG(p9p);
		p9pbuf_put_1(pb, P9PROTO_T_READ);
		p9pbuf_put_2(pb, tag);
		p9pbuf_put_4(pb, p9n->fid_read);
		p9pbuf_put_8(pb, offset+nread);
		p9pbuf_put_4(pb, MIN((uint32_t)*resid,p9p->maxreq-24));
		GETRESPONSE(pb);

		if (p9pbuf_get_type(pb) != P9PROTO_R_READ) {
			rv = proto_handle_rerror(pu, pb);
			break;
		}

		p9pbuf_get_4(pb, &count);
		if ((rv = p9pbuf_read_data(pb, buf + nread, count)))
			break;

		if (count == 0)
			break;

		*resid -= count;
		nread += count;

		p9pbuf_recycleout(pb);
	}
			
 out:
	RETURN(rv);
}

int
puffs9p_node_write(struct puffs_usermount *pu, void *opc, uint8_t *buf,
	off_t offset, size_t *resid, const struct puffs_cred *cred,
	int ioflag)
{
	AUTOVAR(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;
	uint32_t chunk, count;
	size_t nwrite;

	if (ioflag & PUFFS_IO_APPEND)
		offset = pn->pn_va.va_size;

	nwrite = 0;
	while (*resid > 0) {
		chunk = MIN(*resid, p9p->maxreq-32);

		tag = NEXTTAG(p9p);
		p9pbuf_put_1(pb, P9PROTO_T_WRITE);
		p9pbuf_put_2(pb, tag);
		p9pbuf_put_4(pb, p9n->fid_write);
		p9pbuf_put_8(pb, offset+nwrite);
		p9pbuf_put_4(pb, chunk);
		p9pbuf_write_data(pb, buf+nwrite, chunk);
		GETRESPONSE(pb);

		if (p9pbuf_get_type(pb) != P9PROTO_R_WRITE) {
			rv = proto_handle_rerror(pu, pb);
			break;
		}

		p9pbuf_get_4(pb, &count);
		*resid -= count;
		nwrite += count;

		if (count != chunk) {
			rv = EPROTO;
			break;
		}

		p9pbuf_recycleout(pb);
	}

	/* keep the attribute mirror in sync (read bounds on it) */
	if (nwrite > 0 && (uint64_t)(offset + nwrite) > pn->pn_va.va_size) {
		pn->pn_va.va_size = offset + nwrite;
		pn->pn_va.va_bytes = offset + nwrite;
	}
			
 out:
	RETURN(rv);
}

static int
nodecreate(struct puffs_usermount *pu, struct puffs_node *pn,
	struct puffs_newinfo *pni, const char *name,
	const struct vattr *vap, uint32_t dirbit)
{
	AUTOVAR(pu);
	struct puffs_node *pn_new;
	struct p9pnode *p9n = pn->pn_data;
	p9pfid_t nfid = NEXTFID(p9p);
	struct qid9p nqid;
	int tries = 0;

 again:
	if (++tries > 5) {
		rv = EPROTO;
		goto out;
	}

	rv = proto_cc_dupfid(pu, p9n->fid_base, nfid);
	if (rv)
		goto out;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_CREATE);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, nfid);
	p9pbuf_put_str(pb, name);
	p9pbuf_put_4(pb, dirbit | (vap->va_mode & 0777));
	p9pbuf_put_1(pb, 0);
	if (p9p->protover == P9PROTO_VERSION_U)
		p9pbuf_put_str(pb, ""); /* extension[s] */
	GETRESPONSE(pb);

	rv = proto_expect_qid(pu, pb, P9PROTO_R_CREATE, &nqid);
	if (rv)
		goto out;

	/*
	 * Now, little problem here: create returns an *open* fid.
	 * So, clunk it and walk the parent directory to get a fid
	 * which is not open for I/O yet.
	 */
	proto_cc_clunkfid(pu, nfid, 0);
	nfid = NEXTFID(p9p);

	p9pbuf_recycleout(pb);
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_WALK);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n->fid_base);
	p9pbuf_put_4(pb, nfid);
	p9pbuf_put_2(pb, 1);
	p9pbuf_put_str(pb, name);
	GETRESPONSE(pb);

	/*
	 * someone removed it already? try again
	 * note: this is kind of lose/lose
	 */
	if (p9pbuf_get_type(pb) != P9PROTO_R_WALK)
		goto again;

	pn_new = newp9pnode_va(pu, vap, nfid);
	qid2vattr(&pn_new->pn_va, &nqid);
	puffs_newinfo_setcookie(pni, pn_new);

 out:
	RETURN(rv);
}

int
puffs9p_node_create(struct puffs_usermount *pu, void *opc, struct puffs_newinfo *pni,
	const struct puffs_cn *pcn, const struct vattr *va)
{

	return nodecreate(pu, opc, pni, pcn->pcn_name, va, 0);
}

int
puffs9p_node_mkdir(struct puffs_usermount *pu, void *opc, struct puffs_newinfo *pni,
	const struct puffs_cn *pcn, const struct vattr *va)
{

	return nodecreate(pu, opc, pni, pcn->pcn_name,
	    va, P9PROTO_CPERM_DIR);
}

/*
 * readlink: 9P2000 (all flavors) models symlinks as files whose content
 * is the link target, so just read the link file.  Dup a fid so that a
 * possible concurrent I/O on the node's other fids is not disturbed.
 */
int
puffs9p_node_readlink(struct puffs_usermount *pu, void *opc,
	const struct puffs_cred *pcr, char *buf, size_t *sizep)
{
	AUTOVAR(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;
	p9pfid_t nfid = NEXTFID(p9p);
	uint32_t count;
	size_t len = 0;
	int orv;

	/*
	 * 9P2000.L TREADLINK is the primary method: Linux-flavoured
	 * dotu backends (qemu) store the link target out of band, so
	 * a plain read yields 0 bytes there; servers without .L
	 * support answer Rerror and we fall back to reading the link
	 * as a file.  The fallback must TOPEN first: 9P2000 only
	 * allows TREAD on opened fids (an unopened TREAD wedges some
	 * servers).  Dup a fid so concurrent I/O on the node is not
	 * disturbed.
	 */
	/*
	 * try the 9P2000.L readlink on the node's own fid first: a
	 * walked-clone fid confuses some servers' readlink (qemu
	 * answers ENOENT for it), and the node fid needs no dup
	 * since readlink takes no offset state.
	 */
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_L_READLINK);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n->fid_base);
	GETRESPONSE(pb);
	if (p9pbuf_get_type(pb) == P9PROTO_R_ERROR)
		(void) proto_handle_rerror(pu, pb);	/* prints string */

	if (p9pbuf_get_type(pb) == P9PROTO_R_L_READLINK) {
		char *str = NULL;
		uint16_t slen = 0;

		rv = p9pbuf_get_str(pb, &str, &slen);
		if (rv == 0) {
			len = MIN((size_t)slen, *sizep - 1);
			memcpy(buf, str, len);
			buf[len] = '\0';
			*sizep = len;
		}
		free(str);
		goto out;
	}
	rv = 0;

	/*
	 * legacy fallback: T_OPEN + T_READ on a fresh fid.  Some
	 * servers (e.g. qemu in 9P2000.u mode) refuse to open() a
	 * symlink fid but happily serve TREAD on the walked fid, so
	 * tolerate an open failure and try the read anyway.
	 */
	nfid = NEXTFID(p9p);
	orv = proto_cc_open(pu, p9n->fid_base, nfid, P9PROTO_OMODE_READ);

	p9pbuf_recycleout(pb);
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_READ);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, nfid);
	p9pbuf_put_8(pb, 0);
	p9pbuf_put_4(pb, MAXPATHLEN);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_READ) {
		rv = proto_handle_rerror(pu, pb);
		if (orv != 0)
			rv = orv;	/* more descriptive than read error */
	} else {
		p9pbuf_get_4(pb, &count);
		len = MIN((size_t)count, *sizep - 1);
		rv = p9pbuf_read_data(pb, (uint8_t *)buf, len);
		if (rv == 0) {
			buf[len] = '\0';
			*sizep = len;
		}
	}
	proto_cc_clunkfid(pu, nfid, 1);

	/*
	 * last resort: 9P2000.u carries the link target in the
	 * stat extension field
	 */
	if (rv != 0 && p9p->protover == P9PROTO_VERSION_U) {
		char *ext = NULL;
		int erv;

		erv = proto_cc_symlink_target(pu, p9n->fid_base, &ext);
		if (erv == 0) {
			len = MIN(strlen(ext), *sizep - 1);
			memcpy(buf, ext, len);
			buf[len] = '\0';
			*sizep = len;
			rv = 0;
		}
		free(ext);
	}

 out:
	RETURN(rv);
}

/*
 * symlink creation in 9P2000.u: Tcreate with the symlink mode bit and
 * the link target in the extension field, exactly as done by the
 * Linux 9p client's dotu flavor.
 */
int
puffs9p_node_symlink(struct puffs_usermount *pu, void *opc,
	struct puffs_newinfo *pni, const struct puffs_cn *pcn,
	const struct vattr *vap, const char *target)
{
	AUTOVAR(pu);
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;
	struct puffs_node *pn_new;
	p9pfid_t nfid = NEXTFID(p9p);
	struct qid9p nqid;
	uint32_t tlen;

	if (p9p->protover != P9PROTO_VERSION_U)
		return EOPNOTSUPP;

	tlen = strlen(target);

	rv = proto_cc_dupfid(pu, p9n->fid_base, nfid);
	if (rv)
		goto out;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_CREATE);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, nfid);
	p9pbuf_put_str(pb, pcn->pcn_name);
	p9pbuf_put_4(pb, 0777 | P9PROTO_CPERM_SYMLINK);
	p9pbuf_put_1(pb, 0);
	/* 9P2000.u carries the link target in the extension field */
	p9pbuf_put_str(pb, target);
	GETRESPONSE(pb);

	rv = proto_expect_qid(pu, pb, P9PROTO_R_CREATE, &nqid);
	if (rv)
		goto out;

	/* create returned an open fid we do not need: clunk it */
	rv = proto_cc_clunkfid(pu, nfid, 0);
	if (rv)
		goto out;

	/* walk ourselves a clean fid for the new node */
	nfid = NEXTFID(p9p);
	p9pbuf_recycleout(pb);
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_WALK);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n->fid_base);
	p9pbuf_put_4(pb, nfid);
	p9pbuf_put_2(pb, 1);
	p9pbuf_put_str(pb, pcn->pcn_name);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_WALK) {
		rv = EIO;
		goto out;
	}

	pn_new = newp9pnode_qid(pu, &nqid, nfid);
	pn_new->pn_va.va_mode = 0777;
	pn_new->pn_va.va_size = pn_new->pn_va.va_bytes = tlen;
	/*
	 * NOTE: puffs_newinfo_setvtype()/setsize() must NOT be called
	 * here: the dispatcher only wires pni_vtype/pni_size for the
	 * lookup op, so for symlink they would be NULL dereferences
	 * (segfault, killing the daemon).  The kernel hardcodes VLNK
	 * and size 0 when creating the vnode anyway (puffs_vnop_symlink).
	 */
	puffs_newinfo_setcookie(pni, pn_new);

 out:
	RETURN(rv);
}

/*
 * Need to be a bit clever again: the fid is clunked no matter if
 * the remove succeeds or not.  Re-getting a fid would be way too
 * difficult in case the remove failed for a valid reason (directory
 * not empty etcetc.).  So walk ourselves another fid to prod the
 * ice with.
 */
static int
noderemove(struct puffs_usermount *pu, struct puffs_node *pn)
{
	AUTOVAR(pu);
	struct p9pnode *p9n = pn->pn_data;
	p9pfid_t testfid = NEXTFID(p9p);

	rv = proto_cc_dupfid(pu, p9n->fid_base, testfid);
	if (rv)
		goto out;

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_REMOVE);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, testfid);

	/*
	 * XXX: error handling isn't very robust, but doom is impending
	 * anyway, so just accept we're going belly up and play dead
	 */
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_REMOVE) {
		rv = proto_handle_rerror(pu, pb);
	} else {
		proto_cc_clunkfid(pu, p9n->fid_base, 0);
		p9n->fid_base = P9P_INVALFID;
		puffs_pn_remove(pn);
	}

 out:
	RETURN(rv);
}

int
puffs9p_node_remove(struct puffs_usermount *pu, void *opc, void *targ,
	const struct puffs_cn *pcn)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs_node *pn = targ;
	int rv;

	if (pn->pn_va.va_type == VDIR)
		return EISDIR;

	rv = noderemove(pu, pn);
	if (rv == 0)
		puffs_setback(pcc, PUFFS_SETBACK_NOREF_N2);

	return rv;
}

int
puffs9p_node_rmdir(struct puffs_usermount *pu, void *opc, void *targ,
	const struct puffs_cn *pcn)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs_node *pn = targ;
	int rv;

	if (pn->pn_va.va_type != VDIR)
		return ENOTDIR;

	rv = noderemove(pu, pn);
	if (rv == 0)
		puffs_setback(pcc, PUFFS_SETBACK_NOREF_N2);

	return rv;
}

int
puffs9p_node_rename(struct puffs_usermount *pu,
		    void *src_dir, void *src, const struct puffs_cn *pcn_src,
		    void *targ_dir, void *targ, const struct puffs_cn *pcn_targ)
{
	AUTOVAR(pu);
	struct puffs_node *pn_src = src;
	struct p9pnode *p9n_src = pn_src->pn_data;

	/*
	 * 9P rename can only change the last pathname component.
	 * Return EXDEV for attempts to move a file to a different
	 * directory to make mv(1) fall back to copying.
	 */
	if (src_dir != targ_dir) {
		rv = EXDEV;
		goto out;
	}

	/* 9P doesn't allow to overwrite in rename */
	if (targ) {
		struct puffs_node *pn_targ = targ;

		rv = noderemove(pu, pn_targ);
		if (rv)
			goto out;
	}

	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_WSTAT);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, p9n_src->fid_base);
	proto_make_stat(pu, pb, NULL, pcn_targ->pcn_name, pn_src->pn_va.va_type);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_WSTAT)
		rv = proto_handle_rerror(pu, pb);

 out:
	RETURN(rv);
}

/*
 * - "here's one"
 * - "9P"
 * ~ "i'm not dead"
 * - "you're not fooling anyone you know, you'll be stone dead in a minute
 * - "he says he's not quite dead"
 * - "isn't there anything you could do?"
 * - *clunk*!
 * - "thanks"
 */
int
puffs9p_node_reclaim(struct puffs_usermount *pu, void *opc)
{
	struct puffs_node *pn = opc;
	struct p9pnode *p9n = pn->pn_data;

	assert(LIST_EMPTY(&p9n->dir_openlist));
	assert(p9n->fid_read == P9P_INVALFID && p9n->fid_write == P9P_INVALFID);

	proto_cc_clunkfid(pu, p9n->fid_base, 0);
	free(p9n);
	puffs_pn_put(pn);

	return 0;
}

int
puffs9p_node_pathconf(struct puffs_usermount *pu, void *opc, int name,
    __register_t *retval)
{

	/*
	 * Without this, pathconf(2) and fpathconf(2) fail with
	 * ENOTSUPP instead of EINVAL.
	 */
	return EINVAL;
}
