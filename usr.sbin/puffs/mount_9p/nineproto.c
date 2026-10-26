/*	$NetBSD: nineproto.c,v 1.14 2021/09/03 21:55:01 andvar Exp $	*/

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
__RCSID("$NetBSD: nineproto.c,v 1.14 2021/09/03 21:55:01 andvar Exp $");
#endif /* !lint */

#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <puffs.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ninepuffs.h"
#include "nineproto.h"

int
proto_getqid(struct puffs_framebuf *pb, struct qid9p *qid)
{

	if (puffs_framebuf_remaining(pb) < 1+4+8)
		return ENOBUFS;

	p9pbuf_get_1(pb, &qid->qidtype);
	p9pbuf_get_4(pb, &qid->qidvers);
	p9pbuf_get_8(pb, &qid->qidpath);

	return 0;
}

/*
 * Map a stat owner/group string to a numeric id.  Servers send either a
 * login name or (for ids unknown on the server) the numeric value as a
 * decimal string, so fall back to numeric parsing before giving up.
 */
static uid_t
ustr2uid(const char *uid)
{
	struct passwd *pw;
	char *endp;
	unsigned long n;

	if (*uid == '\0')
		return 0;

	pw = getpwnam(uid);
	if (pw != NULL)
		return pw->pw_uid;

	if (isdigit((unsigned char)uid[0])) {
		n = strtoul(uid, &endp, 10);
		if (*endp == '\0' && n < (unsigned long)INT32_MAX)
			return (uid_t)n;
	}

	return 0;
}

static gid_t
gstr2gid(const char *gid)
{
	struct group *grr;
	char *endp;
	unsigned long n;

	if (*gid == '\0')
		return 0;

	grr = getgrnam(gid);
	if (grr != NULL)
		return grr->gr_gid;

	if (isdigit((unsigned char)gid[0])) {
		n = strtoul(gid, &endp, 10);
		if (*endp == '\0' && n < (unsigned long)INT32_MAX)
			return (gid_t)n;
	}

	return 0;
}

/*
 * Map a numeric id to the string representation used in stat.  Send the
 * numeric value back for ids unknown on the client (mapping them to a
 * name valid only on the client would be wrong on the server).
 */
static const char *
uid2ustr(uid_t uid)
{
	static char buf[16];
	struct passwd *pw;

	pw = getpwuid(uid);
	if (pw == NULL) {
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)uid);
		return buf;
	}

	return pw->pw_name;
}

static const char *
gid2gstr(gid_t gid)
{
	static char buf[16];
	struct group *grr;

	grr = getgrgid(gid);
	if (grr == NULL) {
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)gid);
		return buf;
	}

	return grr->gr_name;
}

#define GETFIELD(a,b,unitsize)						\
do {									\
	if (size < unitsize) return EPROTO;				\
	if ((rv = (a(pb, b)))) return rv;				\
	size -= unitsize;						\
} while (/*CONSTCOND*/0)
#define GETSTR(val,strsize)						\
do {									\
	if ((rv = p9pbuf_get_str(pb, val, strsize))) return rv;		\
	if (*strsize > size) return EPROTO;				\
	size -= *strsize;						\
} while (/*CONSTCOND*/0)
int
proto_getstat(struct puffs_usermount *pu, struct puffs_framebuf *pb, struct vattr *vap,
	char **name, uint16_t *rs)
{
	struct puffs9p *p9p = puffs_getspecific(pu);
	char *uid, *gid;
	struct qid9p qid;
	uint64_t flen;
	uint32_t rdev, mode, atime, mtime;
	uint16_t size, v16;
	int rv;

	/* check size */
	if ((rv = p9pbuf_get_2(pb, &size)))
		return rv;
	if (puffs_framebuf_remaining(pb) < size)
		return ENOBUFS;

	if (rs)
		*rs = size+2; /* compensate for size field itself */

	GETFIELD(p9pbuf_get_2, &v16, 2);
	GETFIELD(p9pbuf_get_4, &rdev, 4);
	GETFIELD(proto_getqid, &qid, 13);
	GETFIELD(p9pbuf_get_4, &mode, 4);
	GETFIELD(p9pbuf_get_4, &atime, 4);
	GETFIELD(p9pbuf_get_4, &mtime, 4);
	GETFIELD(p9pbuf_get_8, &flen, 8);
	GETSTR(name, &v16);
	GETSTR(&uid, &v16);
	GETSTR(&gid, &v16);

	vap->va_rdev = rdev;
	vap->va_mode = mode & 0777; /* may contain other uninteresting bits */
	vap->va_atime.tv_sec = atime;
	vap->va_mtime.tv_sec = mtime;
	vap->va_ctime.tv_sec = mtime;
	vap->va_atime.tv_nsec=vap->va_mtime.tv_nsec=vap->va_ctime.tv_nsec = 0;
	vap->va_birthtime.tv_sec = vap->va_birthtime.tv_nsec = 0;
	vap->va_size = vap->va_bytes = flen;
	vap->va_uid = ustr2uid(uid);
	vap->va_gid = gstr2gid(gid);
	free(uid);
	free(gid);
	qid2vattr(vap, &qid);

	/* some defaults */
	if (vap->va_type == VDIR)
		vap->va_nlink = 1906;
	else
		vap->va_nlink = 1;
	vap->va_blocksize = 512;
	vap->va_flags = vap->va_vaflags = 0;
	vap->va_filerev = PUFFS_VNOVAL;

	/* muid, not used */
	GETSTR(NULL, &v16);
	if (p9p->protover == P9PROTO_VERSION_U) {
		uint32_t dummy, n_uid, n_gid;

		GETSTR(NULL, &v16); /* extension[s], not used */
		GETFIELD(p9pbuf_get_4, &n_uid, 4);
		GETFIELD(p9pbuf_get_4, &n_gid, 4);
		GETFIELD(p9pbuf_get_4, &dummy, 4); /* n_muid[4], not used */

		/* numeric ids take precedence over the name mapping */
		if (n_uid != P9PROTO_NUNAME_UNSPECIFIED)
			vap->va_uid = n_uid;
		if (n_gid != P9PROTO_NUNAME_UNSPECIFIED)
			vap->va_gid = n_gid;
	}

	return 0;
}

/*
 * Convert a legacy 9P2000 Rerror string (the server's strerror(3) output,
 * i.e. either NetBSD- or Linux-flavored) to an errno value.
 */
static int
errstr2errno(const char *str)
{
	static const struct {
		int err;
		const char *str;
	} errs[] = {
		{ EPERM,	"operation not permitted" },
		{ ENOENT,	"no such file or directory" },
		{ ESRCH,	"no such process" },
		{ EINTR,	"interrupted system call" },
		{ EIO,		"input/output error" },
		{ ENXIO,	"no such device or address" },
		{ EBADF,	"bad file descriptor" },
		{ EAGAIN,	"resource temporarily unavailable" },
		{ ENOMEM,	"cannot allocate memory" },
		{ EACCES,	"permission denied" },
		{ EFAULT,	"bad address" },
		{ EBUSY,	"resource busy" },
		{ EBUSY,	"device or resource busy" },
		{ EEXIST,	"file exists" },
		{ EXDEV,	"cross-device link" },
		{ EXDEV,	"invalid cross-device link" },
		{ ENODEV,	"no such device" },
		{ ENODEV,	"operation not supported by device" },
		{ ENOTDIR,	"not a directory" },
		{ EISDIR,	"is a directory" },
		{ EINVAL,	"invalid argument" },
		{ ENFILE,	"too many open files in system" },
		{ EMFILE,	"too many open files" },
		{ EFBIG,	"file too large" },
		{ ENOSPC,	"no space left on device" },
		{ ESPIPE,	"illegal seek" },
		{ EROFS,	"read-only file system" },
		{ EMLINK,	"too many links" },
		{ EPIPE,	"broken pipe" },
		{ ENAMETOOLONG,	"file name too long" },
		{ ENOSYS,	"function not implemented" },
		{ ENOTEMPTY,	"directory not empty" },
		{ ELOOP,	"too many levels of symbolic links" },
		{ EMSGSIZE,	"message too long" },
		{ EPROTO,	"protocol error" },
		{ EREMOTE,	"remote error" },
		{ ETIMEDOUT,	"connection timed out" },
		{ EDQUOT,	"disk quota exceeded" },
		{ EDQUOT,	"disc quota exceeded" },
		{ ENOTSUP,	"operation not supported" },
	};
	size_t i;

	for (i = 0; i < __arraycount(errs); i++)
		if (strcasecmp(str, errs[i].str) == 0)
			return errs[i].err;

	return EIO;		/* unrecognized server error */
}

static int
proto_rerror(struct puffs_usermount *pu, struct puffs_framebuf *pb,
    uint32_t *_errno)
{
	struct puffs9p *p9p = puffs_getspecific(pu);
	uint16_t size;
	int rv;
	char *name;

	/* Skip size[4] Rerror tag[2] */
	rv = puffs_framebuf_seekset(pb,
	    sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint16_t));
	if (rv == -1)
		return EPROTO;

	/* rfc9p2000.u: size[4] Rerror tag[2] ename[s] errno[4] */
	rv = p9pbuf_get_str(pb, &name, &size);
	if (rv != 0)
		return rv;

	if (p9p->protover == P9PROTO_VERSION_U) {
		uint32_t errn = 0;

		(void) p9pbuf_get_4(pb, &errn);

		/*
		 * The spec mandates preferring the error string over
		 * the numeric errno: error numbers are not portable
		 * between unix flavors, e.g. Linux ELOOP (40) would
		 * otherwise surface as NetBSD EMSGSIZE (40).
		 */
		rv = errstr2errno(name);
		if (rv == EIO && errn != 0 && errn != 0xffffffff)
			rv = (int) errn;
	} else {
		rv = errstr2errno(name);
	}
	*_errno = rv;
	free(name);

	return 0;
}

int
proto_handle_rerror(struct puffs_usermount *pu, struct puffs_framebuf *pb)
{
	int rv;
	uint32_t _errno;

	if (p9pbuf_get_type(pb) != P9PROTO_R_ERROR)
		return EPROTO;

	rv = proto_rerror(pu, pb, &_errno);
	if (rv == 0)
		rv = _errno != 0 ? _errno : EIO;
	return rv;
}

int
proto_cc_dupfid(struct puffs_usermount *pu, p9pfid_t oldfid, p9pfid_t newfid)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct puffs_framebuf *pb;
	p9ptag_t tag;
	uint16_t qids;
	int rv = 0;

	pb = p9pbuf_makeout();
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_WALK);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, oldfid);
	p9pbuf_put_4(pb, newfid);
	p9pbuf_put_2(pb, 0);
	GETRESPONSE(pb);

	rv = proto_expect_walk_nqids(pu, pb, &qids);
	if (rv == 0 && qids != 0)
		rv = EPROTO;

 out:
	puffs_framebuf_destroy(pb);
	return rv;
}

int
proto_cc_clunkfid(struct puffs_usermount *pu, p9pfid_t fid, int waitforit)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct puffs_framebuf *pb;
	p9ptag_t tag;
	int rv = 0;

	pb = p9pbuf_makeout();
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_CLUNK);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, fid);

	if (waitforit) {
		if (puffs_framev_enqueue_cc(pcc, p9p->servsock, pb, 0) == 0) {
			if (p9pbuf_get_type(pb) != P9PROTO_R_CLUNK)
				rv = proto_handle_rerror(pu, pb);
		} else {
			rv = errno;
		}
		puffs_framebuf_destroy(pb);
	} else {
		JUSTSEND(pb);
	}

 out:
	return rv;
}

/*
 * walk a new fid, then open it
 */
int
proto_cc_open(struct puffs_usermount *pu, p9pfid_t fid,
	p9pfid_t newfid, int mode)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct puffs_framebuf *pb;
	p9ptag_t tag;
	int rv;

	rv = proto_cc_dupfid(pu, fid, newfid);
	if (rv)
		return rv;

	pb = p9pbuf_makeout();
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_OPEN);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, newfid);
	p9pbuf_put_1(pb, mode);
	GETRESPONSE(pb);
	if (p9pbuf_get_type(pb) != P9PROTO_R_OPEN)
		rv = proto_handle_rerror(pu, pb);

 out:
	puffs_framebuf_destroy(pb);
	return rv;
}

/*
 * 9P2000.u: for a symlink, the extension field of its stat entry
 * holds the link target (rfc9p2000.u).  Use as readlink fallback on
 * servers that reject T_L_READLINK and refuse to open() or TREAD a
 * symlink fid (qemu in 9P2000.u mode does both).
 */
int
proto_cc_symlink_target(struct puffs_usermount *pu, p9pfid_t fid,
    char **targetp)
{
	struct puffs_cc *pcc = puffs_cc_getcc(pu);
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct puffs_framebuf *pb;
	p9ptag_t tag;
	uint16_t v16;
	char *str;
	int rv;

	*targetp = NULL;

	pb = p9pbuf_makeout();
	tag = NEXTTAG(p9p);
	p9pbuf_put_1(pb, P9PROTO_T_STAT);
	p9pbuf_put_2(pb, tag);
	p9pbuf_put_4(pb, fid);
	GETRESPONSE(pb);

	if (p9pbuf_get_type(pb) != P9PROTO_R_STAT) {
		rv = proto_handle_rerror(pu, pb);
		goto out;
	}
	rv = 0;

	/*
	 * stat[n] = count[2] followed by the stat entry itself (size[2]
	 * type[2] dev[4] qid[13] mode[4] atime[4] mtime[4] length[8]
	 * name[s] uid[s] gid[s] muid[s] extension[s] ...), mirroring
	 * the two 16-bit reads of proto_expect_stat plus proto_getstat.
	 */
	if ((rv = p9pbuf_get_2(pb, &v16)))	/* count */
		goto out;
	if ((rv = p9pbuf_get_2(pb, &v16)))	/* stat size */
		goto out;
	if (puffs_framebuf_telloff(pb) + 2+4+13+4+4+4+8
	    > puffs_framebuf_tellsize(pb)) {
		rv = EPROTO;
		goto out;
	}
	puffs_framebuf_seekset(pb, puffs_framebuf_telloff(pb) + 2+4+13+4+4+4+8);

	/* name[s] uid[s] gid[s] muid[s] */
	for (unsigned i = 0; i < 4; i++) {
		rv = p9pbuf_get_str(pb, &str, &v16);
		if (rv != 0)
			goto out;
		if (i == 0)
			*targetp = str;
		else
			free(str);
	}
	/* NB: targetp now holds name, restore below */
	str = *targetp;

	rv = p9pbuf_get_str(pb, targetp, &v16);	/* extension[s] */
	free(str);
	if (rv == 0 && **targetp == '\0') {
		free(*targetp);
		*targetp = NULL;
		rv = ENOENT;	/* not a symlink or no target info */
	}

 out:
	puffs_framebuf_destroy(pb);
	return rv;
}

void
proto_make_stat(struct puffs_usermount *pu, struct puffs_framebuf *pb,
    const struct vattr *vap, const char *filename, enum vtype vt)
{
	struct puffs9p *p9p = puffs_getspecific(pu);
	struct vattr fakeva;
	uint32_t mode, atime, mtime;
	uint32_t n_uid, n_gid;
	uint64_t flen;
	const char *owner, *group;
	int startoff, curoff;

	if (vap == NULL) {
		puffs_vattr_null(&fakeva);
		vap = &fakeva;
	}

	startoff = puffs_framebuf_telloff(pb);
	puffs_framebuf_seekset(pb, startoff + 2+2); /* stat[n] incl. stat[2] */

	if (vap->va_mode != (mode_t)PUFFS_VNOVAL)
		mode = vap->va_mode | (vt == VDIR ? P9PROTO_CPERM_DIR : 0);
	else
		mode = P9PROTO_STAT_NOVAL4;
	if (vap->va_atime.tv_sec != (time_t)PUFFS_VNOVAL)
		atime = vap->va_atime.tv_sec;
	else
		atime = P9PROTO_STAT_NOVAL4;
	if (vap->va_mtime.tv_sec != (time_t)PUFFS_VNOVAL)
		mtime = vap->va_mtime.tv_sec;
	else
		mtime = P9PROTO_STAT_NOVAL4;
	if (vap->va_size != (u_quad_t)PUFFS_VNOVAL)
		flen = vap->va_size;
	else
		flen = P9PROTO_STAT_NOVAL8;
	if (vap->va_uid != (uid_t)PUFFS_VNOVAL) {
		owner = uid2ustr(vap->va_uid);
		n_uid = vap->va_uid;
	} else {
		owner = "";
		n_uid = P9PROTO_NUNAME_UNSPECIFIED;
	}
	if (vap->va_gid != (gid_t)PUFFS_VNOVAL) {
		group = gid2gstr(vap->va_gid);
		n_gid = vap->va_gid;
	} else {
		group = "";
		n_gid = P9PROTO_NUNAME_UNSPECIFIED;
	}

	p9pbuf_put_2(pb, P9PROTO_STAT_NOVAL2);	/* kernel type	*/
	p9pbuf_put_4(pb, P9PROTO_STAT_NOVAL4);	/* dev		*/
	p9pbuf_put_1(pb, P9PROTO_STAT_NOVAL1);	/* type		*/
	p9pbuf_put_4(pb, P9PROTO_STAT_NOVAL4);	/* version	*/
	p9pbuf_put_8(pb, P9PROTO_STAT_NOVAL8);	/* path		*/
	p9pbuf_put_4(pb, mode);
	p9pbuf_put_4(pb, atime);
	p9pbuf_put_4(pb, mtime);
	p9pbuf_put_8(pb, flen);
	p9pbuf_put_str(pb, filename ? filename : "");
	p9pbuf_put_str(pb, owner);
	p9pbuf_put_str(pb, group);
	p9pbuf_put_str(pb, "");			/* muid		*/
	if (p9p->protover == P9PROTO_VERSION_U) {
		p9pbuf_put_str(pb, P9PROTO_STAT_NOSTR);	/* extensions[s] */
		p9pbuf_put_4(pb, n_uid);			/* n_uid[4] */
		p9pbuf_put_4(pb, n_gid);			/* n_gid[4] */
		p9pbuf_put_4(pb, P9PROTO_STAT_NOVAL4);	/* n_muid[4] */
	}

	curoff = puffs_framebuf_telloff(pb);
	puffs_framebuf_seekset(pb, startoff);
	p9pbuf_put_2(pb, curoff-(startoff+2));	/* stat[n] size	*/
	p9pbuf_put_2(pb, curoff-(startoff+4));	/* size[2] stat	*/

	puffs_framebuf_seekset(pb, curoff);
}

int
proto_expect_walk_nqids(struct puffs_usermount *pu, struct puffs_framebuf *pb,
    uint16_t *nqids)
{

	if (p9pbuf_get_type(pb) != P9PROTO_R_WALK)
		return proto_handle_rerror(pu, pb);
	return p9pbuf_get_2(pb, nqids);
}

int
proto_expect_qid(struct puffs_usermount *pu, struct puffs_framebuf *pb,
    uint8_t op, struct qid9p *qid)
{

	if (p9pbuf_get_type(pb) != op)
		return proto_handle_rerror(pu, pb);
	return proto_getqid(pb, qid);
}

int
proto_expect_stat(struct puffs_usermount *pu, struct puffs_framebuf *pb,
    struct vattr *va)
{
	uint16_t dummy;
	int rv;

	if (p9pbuf_get_type(pb) != P9PROTO_R_STAT)
		return proto_handle_rerror(pu, pb);
	if ((rv = p9pbuf_get_2(pb, &dummy)))
		return rv;
	return proto_getstat(pu, pb, va, NULL, NULL);
}
