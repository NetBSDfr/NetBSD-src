/*	$NetBSD: vio9p.c,v 1.12 2025/04/22 05:56:25 ozaki-r Exp $	*/

/*
 * Copyright (c) 2019 Internet Initiative Japan, Inc.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: vio9p.c,v 1.12 2025/04/22 05:56:25 ozaki-r Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/condvar.h>
#include <sys/device.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/module.h>
#include <sys/syslog.h>
#include <sys/select.h>
#include <sys/kmem.h>

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/uio.h>

#include <dev/pci/virtioreg.h>
#include <dev/pci/virtiovar.h>

#include "ioconf.h"

//#define VIO9P_DEBUG	1
//#define VIO9P_DUMP	1
#ifdef VIO9P_DEBUG
#define DLOG(fmt, args...) \
	do { log(LOG_DEBUG, "%s: " fmt "\n", __func__, ##args); } while (0)
#else
#define DLOG(fmt, args...) __nothing
#endif

/* Device-specific feature bits */
#define VIO9P_F_MOUNT_TAG	(UINT64_C(1) << 0) /* mount tag specified */

/* Configuration registers */
#define VIO9P_CONFIG_TAG_LEN	0 /* 16bit */
#define VIO9P_CONFIG_TAG	2

#define VIO9P_FLAG_BITS				\
	VIRTIO_COMMON_FLAG_BITS			\
	"b\x00" "MOUNT_TAG\0"


// Must be the same as P9P_DEFREQLEN of usr.sbin/puffs/mount_9p/ninepuffs.h
#define VIO9P_MAX_REQLEN	(16 * 1024)
#define VIO9P_SEGSIZE		PAGE_SIZE
#define VIO9P_N_SEGMENTS	(VIO9P_MAX_REQLEN / VIO9P_SEGSIZE)

/*
 * QEMU defines this as 32 but includes the final zero byte into the
 * limit.  The code below counts the final zero byte separately, so
 * adjust this define to match.
 */
#define P9_MAX_TAG_LEN		31

CTASSERT((PAGE_SIZE) == (VIRTIO_PAGE_SIZE)); /* XXX */

struct vio9p_softc {
	device_t		sc_dev;

	struct virtio_softc	*sc_virtio;
	struct virtqueue	sc_vq[1];

	uint16_t		sc_taglen;
	uint8_t			sc_tag[P9_MAX_TAG_LEN + 1];

	int			sc_flags;
#define VIO9P_INUSE		__BIT(0)

	/*
	 * The transport supports a few concurrent requests.  Each
	 * virtqueue cookie gets its own tx/rx buffer pair (a pipelined
	 * client, e.g. puffs framev, keeps requests outstanding while
	 * earlier replies are still unread; qemu may also complete
	 * requests out of order).  Completions are pumped out of the
	 * used ring into the REPLIED state so virtqueue slots are
	 * recycled even when the client reads replies late or not at
	 * all (fire-and-forget clunks); write(2) returns EAGAIN when
	 * the pool is exhausted instead of blocking, because the
	 * single-threaded file server is also the only thread that
	 * drains it -- blocking there would self-deadlock.  Replies
	 * are delivered in completion order; puffs routes them by the
	 * 9p tag embedded in each message.  A completion whose tag is
	 * not one we have outstanding (previous session's teardown,
	 * stale clunk) is discarded.
	 */
#define VIO9P_NREQ		8
#define VIO9P_P_FREE		0
#define VIO9P_P_INFLIGHT	1
#define VIO9P_P_REPLIED		2
	bus_dmamap_t		sc_map_tx[VIO9P_NREQ];
	bus_dmamap_t		sc_map_rx[VIO9P_NREQ];
	char			*sc_buf_tx[VIO9P_NREQ];
	char			*sc_buf_rx[VIO9P_NREQ];
	int			sc_pstate[VIO9P_NREQ];
	size_t			sc_replen[VIO9P_NREQ];
	int			sc_ninflight;
	int			sc_nreplied;
	int8_t			*sc_slot2pool;	/* cookie slot -> pool idx */
	unsigned int		sc_vqnum;

	kcondvar_t		sc_wait;
	struct selinfo		sc_sel;
	kmutex_t		sc_lock;

	/* staging for the reply being copied out to the client */
	char			*sc_stage;
	size_t			sc_buf_rx_len;
	off_t			sc_buf_rx_offset;
	uint16_t		sc_itag[VIO9P_NREQ];
	uint32_t		sc_gen;
};

/*
 * Locking notes:
 * - all sc_* fields below sc_lock are protected by sc_lock
 *
 * Per-pool-slot state machine:
 *   FREE =(write/pick slot)=> INFLIGHT =(completion pumped from used
 *   ring)=> REPLIED =(copied to stage for readout)=> FREE.
 * A reply may be copied out to the client by several read(2)s; the
 * remainder lives in sc_stage while sc_buf_rx_len > 0.
 */

static int	vio9p_match(device_t, cfdata_t, void *);
static void	vio9p_attach(device_t, device_t, void *);
static void	vio9p_read_config(struct vio9p_softc *);
static int	vio9p_request_done(struct virtqueue *);

static int	vio9p_read(struct file *, off_t *, struct uio *, kauth_cred_t,
		    int);
static int	vio9p_write(struct file *, off_t *, struct uio *,
		    kauth_cred_t, int);
static int	vio9p_ioctl(struct file *, u_long, void *);
static int	vio9p_close(struct file *);
static int	vio9p_kqfilter(struct file *, struct knote *);

static const struct fileops vio9p_fileops = {
	.fo_name = "vio9p",
	.fo_read = vio9p_read,
	.fo_write = vio9p_write,
	.fo_ioctl = vio9p_ioctl,
	.fo_fcntl = fnullop_fcntl,
	.fo_poll = fnullop_poll,
	.fo_stat = fbadop_stat,
	.fo_close = vio9p_close,
	.fo_kqfilter = vio9p_kqfilter,
	.fo_restart = fnullop_restart,
};

static dev_type_open(vio9p_dev_open);

const struct cdevsw vio9p_cdevsw = {
	.d_open = vio9p_dev_open,
	.d_read = noread,
	.d_write = nowrite,
	.d_ioctl = noioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER | D_MPSAFE,
};

static int
vio9p_dev_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct vio9p_softc *sc;
	struct file *fp;
	int error, fd;

	sc = device_lookup_private(&vio9p_cd, minor(dev));
	if (sc == NULL)
		return ENXIO;

	/* FIXME TOCTOU */
	if (ISSET(sc->sc_flags, VIO9P_INUSE))
		return EBUSY;

	/* falloc() will fill in the descriptor for us. */
	error = fd_allocfile(&fp, &fd);
	if (error != 0)
		return error;

	sc->sc_flags |= VIO9P_INUSE;
	sc->sc_gen++;  /* invalidates any in-flight drain of a prior close */

	return fd_clone(fp, fd, flag, &vio9p_fileops, sc);
}

static int
vio9p_ioctl(struct file *fp, u_long cmd, void *addr)
{
	int error = 0;

	switch (cmd) {
	case FIONBIO:
		break;
	default:
		error = EINVAL;
		break;
	}

	return error;
}

/*
 * Move completions out of the used ring into REPLIED pool slots.
 * This recycles virtqueue cookies and pool slots independently of
 * when (or whether) the client reads the replies: a reply sitting
 * in the used ring must not keep its virtqueue descriptor claimed,
 * nor keep its pool slot INFLIGHT (which would block new writes and
 * self-deadlock the single-threaded server that is the only reader).
 * Unknown tags (previous session leftovers, stale clunks) are
 * discarded here.  Caller holds sc_lock.
 */
static void
vio9p_pump(struct vio9p_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vq[0];
	int slot, len, pool;
	uint16_t rtag;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		pool = (slot >= 0 && (unsigned)slot < sc->sc_vqnum) ?
		    sc->sc_slot2pool[slot] : -1;
		if (pool < 0 || pool >= VIO9P_NREQ ||
		    sc->sc_pstate[pool] != VIO9P_P_INFLIGHT) {
			DLOG("bogus slot %d len %d", slot, len);
			virtio_dequeue_commit(vsc, vq, slot);
			continue;
		}
		rtag = 0;
		if (len >= 7) {
			rtag = (uint8_t)sc->sc_buf_rx[pool][5] |
			    (uint16_t)(uint8_t)sc->sc_buf_rx[pool][6] << 8;
		}
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_map_rx[pool], 0,
		    VIO9P_MAX_REQLEN, BUS_DMASYNC_POSTREAD);
		virtio_dequeue_commit(vsc, vq, slot);
		sc->sc_slot2pool[slot] = -1;
		KASSERT(sc->sc_ninflight > 0);
		sc->sc_ninflight--;
		if (len < 7 || sc->sc_itag[pool] != rtag) {
			sc->sc_pstate[pool] = VIO9P_P_FREE;
			DLOG("discard rtag %d len %d pool %d", (int)rtag,
			    len, pool);
			continue;
		}
		sc->sc_replen[pool] = MIN((size_t)len,
		    (size_t)VIO9P_MAX_REQLEN);
		sc->sc_pstate[pool] = VIO9P_P_REPLIED;
		sc->sc_nreplied++;
	}
}

static int
vio9p_read(struct file *fp, off_t *offp, struct uio *uio,
    kauth_cred_t cred, int flags)
{
	struct vio9p_softc *sc = fp->f_data;
	int error, len, i;

	DLOG("enter");

	mutex_enter(&sc->sc_lock);

	if (sc->sc_buf_rx_len > 0) {
		/* We already have some remaining, consume it. */
		len = sc->sc_buf_rx_len - sc->sc_buf_rx_offset;
		goto consume;
	}

	if (uio->uio_resid > VIO9P_MAX_REQLEN) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Wait for a reply.
	 *
	 * Poll the completion ring in addition to the interrupt-driven
	 * wakeup (completions can be coalesced such that an interrupt
	 * gets spent waking some other state transition and sleeping
	 * for the "next edge" would miss a completion already sitting
	 * in the used ring forever): the interrupt is a wakeup hint
	 * only, the ring is the source of truth.
	 *
	 * A timeout merely re-polls: the host may legitimately take a
	 * long time and returning early stalls the edge-triggered
	 * puffs kqueue.  A real signal (e.g. the SIGTERM puffs sends
	 * to the file server at unmount) aborts the read, otherwise
	 * the dying server cannot exit, close() never runs, and the
	 * next mount inherits a wedged session.  vio9p_close()
	 * broadcasts to break this loop when the session goes away.
	 */
	for (;;) {
		int w;

		vio9p_pump(sc);

		for (i = 0; i < VIO9P_NREQ; i++)
			if (sc->sc_pstate[i] == VIO9P_P_REPLIED)
				break;
		if (i < VIO9P_NREQ) {
			len = sc->sc_replen[i];
			memcpy(sc->sc_stage, sc->sc_buf_rx[i], len);
			sc->sc_pstate[i] = VIO9P_P_FREE;
			sc->sc_nreplied--;
			sc->sc_buf_rx_len = len;
			sc->sc_buf_rx_offset = 0;
			cv_broadcast(&sc->sc_wait);
			selnotify(&sc->sc_sel, 0, 1);
			DLOG("reply len %d pool %d", len, i);
			goto consume;
		}

		if (sc->sc_ninflight == 0) {
			/* nothing requested (or session torn down) */
			error = EAGAIN;
			goto out;
		}

		w = cv_timedwait_sig(&sc->sc_wait, &sc->sc_lock, hz);
		if (w != 0 && w != EWOULDBLOCK) {
			DLOG("interrupted %d", w);
			error = EAGAIN;
			goto out;
		}
		DLOG("repoll: no reply yet, inflight %d", sc->sc_ninflight);
	}

consume:
	DLOG("uio_resid=%lu", uio->uio_resid);
	len = MIN(len, uio->uio_resid);
	error = uiomove(sc->sc_stage + sc->sc_buf_rx_offset, len, uio);
	if (error != 0)
		goto out;

	sc->sc_buf_rx_offset += len;
	if (sc->sc_buf_rx_offset == sc->sc_buf_rx_len) {
		sc->sc_buf_rx_len = 0;
		sc->sc_buf_rx_offset = 0;
		selnotify(&sc->sc_sel, 0, 1);
	}

out:
	mutex_exit(&sc->sc_lock);
	return error;
}

static int
vio9p_write(struct file *fp, off_t *offp, struct uio *uio,
    kauth_cred_t cred, int flags)
{
	struct vio9p_softc *sc = fp->f_data;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vq[0];
	int error, i, slot, pool = -1;
	size_t len;
	uint16_t tag = 0;

	DLOG("enter");

	if (uio->uio_resid == 0)
		return 0;

	if (uio->uio_resid > VIO9P_MAX_REQLEN)
		return EINVAL;

	mutex_enter(&sc->sc_lock);

	/*
	 * Recycle completed replies first, then take a free pool slot.
	 * If the pool is exhausted return EAGAIN rather than blocking:
	 * the single-threaded file server is also the only thread that
	 * drains the ring, so blocking here against its own unread
	 * replies would self-deadlock.  puffs framev retries on the
	 * EVFILT_WRITE edge, which selnotify() from completion/delivery
	 * generates.
	 */
	vio9p_pump(sc);
	for (i = 0; i < VIO9P_NREQ; i++)
		if (sc->sc_pstate[i] == VIO9P_P_FREE) {
			pool = i;
			break;
		}
	if (pool == -1) {
		DLOG("pool full, inflight %d replied %d",
		    sc->sc_ninflight, sc->sc_nreplied);
		error = EAGAIN;
		goto out;
	}

	len = uio->uio_resid;
	error = uiomove(sc->sc_buf_tx[pool], len, uio);
	if (error != 0)
		goto out;

	if (len >= 7)
		tag = (uint16_t)(unsigned char)sc->sc_buf_tx[pool][5] |
		    ((uint16_t)(unsigned char)sc->sc_buf_tx[pool][6] << 8);

	error = virtio_enqueue_prep(vsc, vq, &slot);
	if (error != 0) {
		log(LOG_ERR, "%s: virtio_enqueue_prep failed: %d\n",
		    device_xname(sc->sc_dev), error);
		goto out;
	}
	error = virtio_enqueue_reserve(vsc, vq, slot,
	    sc->sc_map_tx[pool]->dm_nsegs + sc->sc_map_rx[pool]->dm_nsegs);
	if (error != 0) {
		log(LOG_ERR, "%s: virtio_enqueue_reserve failed\n",
		    device_xname(sc->sc_dev));
		virtio_enqueue_abort(vsc, vq, slot);
		goto out;
	}
	sc->sc_slot2pool[slot] = pool;

	/* Tx */
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_map_tx[pool], 0,
	    len, BUS_DMASYNC_PREWRITE);
	virtio_enqueue(vsc, vq, slot, sc->sc_map_tx[pool], true);
	/* Rx */
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_map_rx[pool], 0,
	    VIO9P_MAX_REQLEN, BUS_DMASYNC_PREREAD);
	virtio_enqueue(vsc, vq, slot, sc->sc_map_rx[pool], false);

	/*
	 * Publish the bookkeeping before making the request visible to
	 * the host, so a completion pumped on another CPU always finds
	 * the slot mapped and tagged.
	 */
	sc->sc_itag[pool] = tag;
	sc->sc_pstate[pool] = VIO9P_P_INFLIGHT;
	sc->sc_ninflight++;
	virtio_enqueue_commit(vsc, vq, slot, true);

	DLOG("tx type %d tag %d len %zu slot %d pool %d",
	    (int)(unsigned char)sc->sc_buf_tx[pool][4], (int)tag, len,
	    slot, pool);
	error = 0;
out:
	mutex_exit(&sc->sc_lock);
	return error;
}

static int
vio9p_close(struct file *fp)
{
	struct vio9p_softc *sc = fp->f_data;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vq[0];
	int slot;
	int dlen;
	int round;
	int i;
	uint32_t mygen;

	KASSERT(ISSET(sc->sc_flags, VIO9P_INUSE));

	mutex_enter(&sc->sc_lock);
	mygen = sc->sc_gen;
	sc->sc_flags &= ~VIO9P_INUSE;
	/*
	 * The session is gone.  Reset the bookkeeping so that a
	 * subsequent open() starts fresh even if we closed with
	 * requests in flight or a reply half consumed; otherwise the
	 * next mount inherits a stuck session.
	 */
	if (sc->sc_gen == mygen) {
		sc->sc_buf_rx_len = 0;
		sc->sc_buf_rx_offset = 0;
		for (i = 0; i < VIO9P_NREQ; i++)
			sc->sc_pstate[i] = VIO9P_P_FREE;
		sc->sc_ninflight = 0;
		sc->sc_nreplied = 0;
		memset(sc->sc_slot2pool, -1, sc->sc_vqnum);
	}
	cv_broadcast(&sc->sc_wait);

	/*
	 * Drop any already-completed replies from the used ring; the
	 * new session must not inherit them (the first reply of the
	 * next mount would be a stale message).  The device may
	 * still complete teardown requests (e.g. Rflush) a moment
	 * after we drained, so wait briefly and repeat until the ring
	 * stays quiet.  A new open() bumps the generation: our close()
	 * may run after umount() returned and the next mount_9p
	 * already started, so stop touching the ring the moment it
	 * belongs to a new session.  Anything that still slips
	 * through is caught by the inflight-tag discard in
	 * vio9p_read().
	 */
	for (round = 0; round < 10; round++) {
		int drained = 0;
		while (sc->sc_gen == mygen &&
		    virtio_dequeue(vsc, vq, &slot, &dlen) == 0) {
			if (slot >= 0 && (unsigned)slot < sc->sc_vqnum)
				sc->sc_slot2pool[slot] = -1;
			virtio_dequeue_commit(vsc, vq, slot);
			drained++;
		}
		if (sc->sc_gen != mygen || (drained == 0 && round > 0))
			break;
		mutex_exit(&sc->sc_lock);
		tsleep(sc, PRIBIO, "vio9pcl", 2);
		mutex_enter(&sc->sc_lock);
	}

	mutex_exit(&sc->sc_lock);

	return 0;
}

static void
filt_vio9p_detach(struct knote *kn)
{
	struct vio9p_softc *sc = kn->kn_hook;

	mutex_enter(&sc->sc_lock);
	selremove_knote(&sc->sc_sel, kn);
	mutex_exit(&sc->sc_lock);
}

static int
filt_vio9p_read(struct knote *kn, long hint)
{
	struct vio9p_softc *sc = kn->kn_hook;
	int rv;

	kn->kn_data = sc->sc_buf_rx_len;
	/* XXX need sc_lock? */
	rv = (kn->kn_data > 0) || (sc->sc_nreplied > 0) ||
	    (sc->sc_ninflight > 0);

	return rv;
}

static const struct filterops vio9p_read_filtops = {
	.f_flags = FILTEROP_ISFD,
	.f_attach = NULL,
	.f_detach = filt_vio9p_detach,
	.f_event = filt_vio9p_read,
};

static int
filt_vio9p_write(struct knote *kn, long hint)
{
	struct vio9p_softc *sc = kn->kn_hook;

	/* XXX need sc_lock? */
	return sc->sc_ninflight + sc->sc_nreplied < VIO9P_NREQ;
}

static const struct filterops vio9p_write_filtops = {
	.f_flags = FILTEROP_ISFD,
	.f_attach = NULL,
	.f_detach = filt_vio9p_detach,
	.f_event = filt_vio9p_write,
};

static int
vio9p_kqfilter(struct file *fp, struct knote *kn)
{
	struct vio9p_softc *sc = fp->f_data;

	switch (kn->kn_filter) {
	case EVFILT_READ:
		kn->kn_fop = &vio9p_read_filtops;
		break;

	case EVFILT_WRITE:
		kn->kn_fop = &vio9p_write_filtops;
		break;

	default:
		log(LOG_ERR, "%s: kn_filter=%u\n", __func__, kn->kn_filter);
		return EINVAL;
	}

	kn->kn_hook = sc;

	mutex_enter(&sc->sc_lock);
	selrecord_knote(&sc->sc_sel, kn);
	mutex_exit(&sc->sc_lock);

	return 0;
}

CFATTACH_DECL_NEW(vio9p, sizeof(struct vio9p_softc),
    vio9p_match, vio9p_attach, NULL, NULL);

static int
vio9p_match(device_t parent, cfdata_t match, void *aux)
{
	struct virtio_attach_args *va = aux;

	if (va->sc_childdevid == VIRTIO_DEVICE_ID_9P)
		return 1;

	return 0;
}

static void
vio9p_attach(device_t parent, device_t self, void *aux)
{
	struct vio9p_softc *sc = device_private(self);
	struct virtio_softc *vsc = device_private(parent);
	uint64_t features;
	int error, i;
	const struct sysctlnode *node;

	if (virtio_child(vsc) != NULL) {
		aprint_normal(": child already attached for %s; "
			      "something wrong...\n", device_xname(parent));
		return;
	}

	sc->sc_dev = self;
	sc->sc_virtio = vsc;

	virtio_child_attach_start(vsc, self, IPL_VM,
	    VIO9P_F_MOUNT_TAG, VIO9P_FLAG_BITS);

	features = virtio_features(vsc);
	if ((features & VIO9P_F_MOUNT_TAG) == 0)
		goto err_none;

	virtio_init_vq_vqdone(vsc, &sc->sc_vq[0], 0, vio9p_request_done);
	error = virtio_alloc_vq(vsc, &sc->sc_vq[0], VIO9P_MAX_REQLEN,
	    VIO9P_N_SEGMENTS * 2, "vio9p");
	if (error != 0)
		goto err_vq;

	sc->sc_vqnum = sc->sc_vq[0].vq_num;
	sc->sc_slot2pool = kmem_alloc(sc->sc_vqnum * sizeof(int8_t), KM_SLEEP);
	for (i = 0; i < sc->sc_vqnum; i++)
		sc->sc_slot2pool[i] = -1;
	sc->sc_stage = kmem_alloc(VIO9P_MAX_REQLEN, KM_SLEEP);

	for (i = 0; i < VIO9P_NREQ; i++) {
		sc->sc_buf_tx[i] = kmem_alloc(VIO9P_MAX_REQLEN, KM_SLEEP);
		sc->sc_buf_rx[i] = kmem_alloc(VIO9P_MAX_REQLEN, KM_SLEEP);
		error = bus_dmamap_create(virtio_dmat(vsc), VIO9P_MAX_REQLEN,
		    VIO9P_N_SEGMENTS, VIO9P_SEGSIZE, 0, BUS_DMA_WAITOK,
		    &sc->sc_map_tx[i]);
		if (error != 0) {
			aprint_error_dev(sc->sc_dev,
			    "bus_dmamap_create failed: %d\n", error);
			goto err_dmamap;
		}
		error = bus_dmamap_create(virtio_dmat(vsc), VIO9P_MAX_REQLEN,
		    VIO9P_N_SEGMENTS, VIO9P_SEGSIZE, 0, BUS_DMA_WAITOK,
		    &sc->sc_map_rx[i]);
		if (error != 0) {
			aprint_error_dev(sc->sc_dev,
			    "bus_dmamap_create failed: %d\n", error);
			goto err_dmamap;
		}
		error = bus_dmamap_load(virtio_dmat(vsc), sc->sc_map_tx[i],
		    sc->sc_buf_tx[i], VIO9P_MAX_REQLEN, NULL,
		    BUS_DMA_WAITOK | BUS_DMA_WRITE);
		if (error != 0) {
			aprint_error_dev(sc->sc_dev,
			    "bus_dmamap_load failed: %d\n", error);
			goto err_dmamap;
		}
		error = bus_dmamap_load(virtio_dmat(vsc), sc->sc_map_rx[i],
		    sc->sc_buf_rx[i], VIO9P_MAX_REQLEN, NULL,
		    BUS_DMA_WAITOK | BUS_DMA_READ);
		if (error != 0) {
			aprint_error_dev(sc->sc_dev,
			    "bus_dmamap_load failed: %d\n", error);
			goto err_dmamap;
		}
		sc->sc_pstate[i] = VIO9P_P_FREE;
	}
	sc->sc_ninflight = 0;
	sc->sc_nreplied = 0;

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&sc->sc_wait, "vio9p");

	vio9p_read_config(sc);
	aprint_normal_dev(self, "tagged as %s\n", sc->sc_tag);

	sysctl_createv(NULL, 0, NULL, &node, 0, CTLTYPE_NODE,
	    "vio9p", SYSCTL_DESCR("VirtIO 9p toplevel"),
	    NULL, 0, NULL, 0,
	    CTL_HW, CTL_CREATE, CTL_EOL);
	sysctl_createv(NULL, 0, &node, &node, 0, CTLTYPE_NODE,
	    device_xname(self), SYSCTL_DESCR("VirtIO 9p device"),
	    NULL, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(NULL, 0, &node, NULL, 0, CTLTYPE_STRING,
	    "tag", SYSCTL_DESCR("VirtIO 9p tag value"),
	    NULL, 0, sc->sc_tag, 0,
	    CTL_CREATE, CTL_EOL);

	error = virtio_child_attach_finish(vsc, sc->sc_vq,
	    __arraycount(sc->sc_vq), NULL,
	    VIRTIO_F_INTR_MPSAFE | VIRTIO_F_INTR_SOFTINT);
	if (error != 0)
		goto err_mutex;

	return;

err_mutex:
	cv_destroy(&sc->sc_wait);
	mutex_destroy(&sc->sc_lock);
err_dmamap:
	for (i = 0; i < VIO9P_NREQ; i++) {
		if (sc->sc_map_tx[i] != NULL) {
			(void)bus_dmamap_unload(virtio_dmat(vsc),
			    sc->sc_map_tx[i]);
			bus_dmamap_destroy(virtio_dmat(vsc), sc->sc_map_tx[i]);
		}
		if (sc->sc_map_rx[i] != NULL) {
			(void)bus_dmamap_unload(virtio_dmat(vsc),
			    sc->sc_map_rx[i]);
			bus_dmamap_destroy(virtio_dmat(vsc), sc->sc_map_rx[i]);
		}
	}
err_vq:
	virtio_free_vq(vsc, &sc->sc_vq[0]);
err_none:
	virtio_child_attach_failed(vsc);
	return;
}

static void
vio9p_read_config(struct vio9p_softc *sc)
{
	device_t dev = sc->sc_dev;
	uint8_t reg;
	int i;

	/* these values are explicitly specified as little-endian */
	sc->sc_taglen = virtio_read_device_config_le_2(sc->sc_virtio,
		VIO9P_CONFIG_TAG_LEN);

	if (sc->sc_taglen > P9_MAX_TAG_LEN) {
		aprint_error_dev(dev, "warning: tag is trimmed from %u to %u\n",
		    sc->sc_taglen, P9_MAX_TAG_LEN);
		sc->sc_taglen = P9_MAX_TAG_LEN;
	}

	for (i = 0; i < sc->sc_taglen; i++) {
		reg = virtio_read_device_config_1(sc->sc_virtio,
		    VIO9P_CONFIG_TAG + i);
		sc->sc_tag[i] = reg;
	}
	sc->sc_tag[i] = '\0';
}

static int
vio9p_request_done(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vio9p_softc *sc = device_private(virtio_child(vsc));

	DLOG("enter");

	mutex_enter(&sc->sc_lock);
	/*
	 * A completion only wakes sleepers and knotes; the reader
	 * dequeues and validates tags itself (the interrupt is a hint,
	 * the ring is the source of truth), so there is no state to
	 * flip here.
	 */
	cv_broadcast(&sc->sc_wait);
	selnotify(&sc->sc_sel, 0, 1);
	mutex_exit(&sc->sc_lock);

	return 1;
}

MODULE(MODULE_CLASS_DRIVER, vio9p, "virtio");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
vio9p_modcmd(modcmd_t cmd, void *opaque)
{
#ifdef _MODULE
	devmajor_t bmajor = NODEVMAJOR, cmajor = NODEVMAJOR;
#endif
	int error = 0;

#ifdef _MODULE
	switch (cmd) {
	case MODULE_CMD_INIT:
		devsw_attach(vio9p_cd.cd_name, NULL, &bmajor,
		    &vio9p_cdevsw, &cmajor);
		error = config_init_component(cfdriver_ioconf_vio9p,
		    cfattach_ioconf_vio9p, cfdata_ioconf_vio9p);
		break;
	case MODULE_CMD_FINI:
		error = config_fini_component(cfdriver_ioconf_vio9p,
		    cfattach_ioconf_vio9p, cfdata_ioconf_vio9p);
		devsw_detach(NULL, &vio9p_cdevsw);
		break;
	default:
		error = ENOTTY;
		break;
	}
#endif

	return error;
}
