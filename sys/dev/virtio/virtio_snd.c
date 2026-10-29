/* $NetBSD$ */

/*-
 * Copyright (c) 2026 The smolBSD Project
 * All rights reserved.
 *
 * This code is derived from software contributed to The smolBSD Project
 * by the smolBSD project.
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
 * THIS SOFTWARE IS PROVIDED BY THE smolBSD PROJECT AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE smolBSD PROJECT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * virtio-snd: audio(4) driver for the virtio-sound device (device ID
 * 19) over any virtio transport.  Playback only; capture streams are
 * left untouched.
 *
 * Queues: 0 = controlq, 1 = eventq, 2 = tx.  The device advertises a
 * fourth (rx) queue which is never used or allocated.
 *
 * The QEMU virtio-sound model (tested against 10.2) implements a
 * reduced subset of the virtio sound spec: the event queue is unused,
 * jack and chmap queries return NOT_SUPP, and -- important -- the host
 * voice is created when PREPARE runs for a stream that has no stream
 * object yet (the device resets its streams to none), and PREPARE on
 * an already existing stream is a no-op.  Changing sample rate or
 * channel count across opens therefore requires a device reset, so
 * this driver pins one format for the lifetime of the attach:
 * S16LE, 2 channels, 48 kHz.  Apps requesting anything else are
 * served by the audio core (rate is negotiated back via AUDIO_SETINFO,
 * mono/8-bit/mulaw are converted by the core).
 *
 * Playback is driven by the circular output ring that the audio core
 * allocates through allocm(9).  The ring is split into fixed-size
 * blocks; each block is submitted to the tx queue as one message
 * (4-byte stream header + block data, device writes an 8-byte status
 * block).  The device consumes blocks in submission order, paced by
 * the host voice; on each completion the core's hole callback refills
 * the block and it is resubmitted.
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/audio/audio_if.h>

#include <dev/pci/virtioreg.h>
#include <dev/pci/virtiovar.h>

#ifndef VIRTIO_DEVICE_ID_SOUND
#define VIRTIO_DEVICE_ID_SOUND		25
#endif

/*
 * Device configuration space.
 */
#define VIOSND_CFG_JACKS		0
#define VIOSND_CFG_STREAMS		4
#define VIOSND_CFG_CHMAPS		8

/*
 * Virtqueue indices.
 */
#define VIOSND_CONTROLQ			0
#define VIOSND_EVENTQ			1
#define VIOSND_TXQ			2
#define VIOSND_NVQ			3

/* Always the first (stereo output) stream. */
#define VIOSND_STREAM_OUT		0

/*
 * Control request codes.
 */
#define VIOSND_R_PCM_SET_PARAMS		0x0101
#define VIOSND_R_PCM_PREPARE		0x0102
#define VIOSND_R_PCM_RELEASE		0x0103
#define VIOSND_R_PCM_START		0x0104
#define VIOSND_R_PCM_STOP		0x0105
#define VIOSND_S_OK			0x8000

/*
 * Pinned stream format: S16LE stereo 48 kHz.  VIOSND_PCM_FMT_S16 and
 * VIOSND_PCM_RATE_48000 are the virtio-snd wire enums.
 */
#define VIOSND_PCM_FMT_S16		5
#define VIOSND_PCM_RATE_48000		7
#define VIOSND_RATE			48000
#define VIOSND_CHANNELS			2

/*
 * Ring geometry.  The audio core is forced (round_blocksize,
 * round_buffersize) to this exact layout so the block count is static.
 */
#define VIOSND_BLKSIZE			2048
#define VIOSND_RINGSZ			16384
#define VIOSND_NSLOT			(VIOSND_RINGSZ / VIOSND_BLKSIZE)

struct viosnd_pcm_hdr {
	uint32_t	code;
	uint32_t	stream_id;
};

struct viosnd_pcm_set_params {
	uint32_t	code;
	uint32_t	stream_id;
	uint32_t	buffer_bytes;
	uint32_t	period_bytes;
	uint32_t	features;
	uint8_t		channels;
	uint8_t		format;
	uint8_t		rate;
	uint8_t		padding;
};

struct viosnd_pcm_xfer {
	uint32_t	stream_id;
};

struct viosnd_pcm_status {
	uint32_t	status;
	uint32_t	latency_bytes;
};

/*
 * Single-segment DMA region; the virtio enqueue interface requires
 * dm_nsegs == 1.
 */
struct viosnd_dma {
	bus_dmamap_t	dm_map;
	bus_dma_segment_t dm_seg;
	void		*dm_va;
	size_t		dm_size;
};

struct viosnd_softc {
	device_t	sc_dev;
	struct virtio_softc *sc_virtio;

	struct virtqueue sc_vqs[VIOSND_NVQ];

	kmutex_t	sc_lock;	/* audio core proc lock */
	kmutex_t	sc_intr_lock;	/* audio core intr lock */

	/* control queue: one request in flight at a time */
	struct viosnd_dma sc_creq;
	struct viosnd_dma sc_cresp;
	kmutex_t	sc_ctrl_lock;
	volatile bool	sc_ctrl_done;
	volatile uint32_t sc_ctrl_code;

	/* tx per-slot header and status arrays */
	struct viosnd_dma sc_xhdr;
	struct viosnd_dma sc_xstat;

	/* output ring, owned by the audio core, streamed by us */
	struct viosnd_dma sc_ring;
	bool		sc_ring_valid;
	void		(*sc_hole)(void *);
	void		*sc_hole_arg;
	volatile bool	sc_playing;
	volatile int	sc_tx_ack;	/* next slot completing */
	uint32_t	sc_errors;

	struct audio_format sc_formats[1];
};

#define	virtio_notify(vsc, vq)	virtio_enqueue_commit(vsc, vq, -1, true)

static int	viosnd_match(device_t, cfdata_t, void *);
static void	viosnd_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(viosnd, sizeof(struct viosnd_softc),
    viosnd_match, viosnd_attach, NULL, NULL);

static void	viosnd_dma_alloc(struct viosnd_softc *, struct viosnd_dma *,
		    size_t);
static int	viosnd_ctrl_req(struct viosnd_softc *, const void *,
		    bus_size_t);
static int	viosnd_ctrl_drain(struct virtqueue *);
static int	viosnd_event_drain(struct virtqueue *);
static int	viosnd_tx_fill_slot(struct viosnd_softc *, int);
static int	viosnd_tx_intr(struct virtqueue *);

static int	viosnd_open(void *, int);
static void	viosnd_close(void *);
static int	viosnd_query_format(void *, audio_format_query_t *);
static int	viosnd_set_format(void *, int, const audio_params_t *,
		    const audio_params_t *, audio_filter_reg_t *,
		    audio_filter_reg_t *);
static int	viosnd_round_blocksize(void *, int, int,
		    const audio_params_t *);
static int	viosnd_trigger_output(void *, void *, void *, int,
		    void (*)(void *), void *, const audio_params_t *);
static int	viosnd_halt_output(void *);
static int	viosnd_getdev(void *, struct audio_device *);
static int	viosnd_set_port(void *, mixer_ctrl_t *);
static int	viosnd_get_port(void *, mixer_ctrl_t *);
static int	viosnd_query_devinfo(void *, mixer_devinfo_t *);
static void	*viosnd_allocm(void *, int, size_t);
static void	viosnd_freem(void *, void *, size_t);
static size_t	viosnd_round_buffersize(void *, int, size_t);
static int	viosnd_get_props(void *);
static int	viosnd_trigger_input(void *, void *, void *, int,
		    void (*)(void *), void *, const audio_params_t *);
static int	viosnd_dev_ioctl(void *, u_long, void *, int, struct lwp *);
static void	viosnd_get_locks(void *, kmutex_t **, kmutex_t **);

static const struct audio_hw_if viosnd_hw_if = {
	.open			= viosnd_open,
	.close			= viosnd_close,
	.query_format		= viosnd_query_format,
	.set_format		= viosnd_set_format,
	.round_blocksize	= viosnd_round_blocksize,
	.halt_output		= viosnd_halt_output,
	.getdev			= viosnd_getdev,
	.set_port		= viosnd_set_port,
	.get_port		= viosnd_get_port,
	.query_devinfo		= viosnd_query_devinfo,
	.allocm			= viosnd_allocm,
	.freem			= viosnd_freem,
	.round_buffersize	= viosnd_round_buffersize,
	.get_props		= viosnd_get_props,
	.trigger_output		= viosnd_trigger_output,
	.trigger_input		= viosnd_trigger_input,
	.dev_ioctl		= viosnd_dev_ioctl,
	.get_locks		= viosnd_get_locks,
};

static int
viosnd_match(device_t parent, cfdata_t match, void *aux)
{
	struct virtio_attach_args *va = aux;

	if (va->sc_childdevid == VIRTIO_DEVICE_ID_SOUND)
		return 1;

	return 0;
}

/*
 * Allocate and map a contiguous, single-segment DMA region.  Failure
 * is fatal: the regions are small and allocated at attach.
 */
static void
viosnd_dma_alloc(struct viosnd_softc *sc, struct viosnd_dma *dm, size_t size)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int error, nsegs;

	dm->dm_size = size;

	error = bus_dmamem_alloc(virtio_dmat(vsc), size, 8, 0, &dm->dm_seg,
	    1, &nsegs, BUS_DMA_WAITOK);
	if (error)
		panic("viosnd: can't alloc %zu DMA", size);

	error = bus_dmamem_map(virtio_dmat(vsc), &dm->dm_seg, nsegs, size,
	    &dm->dm_va, BUS_DMA_WAITOK);
	if (error)
		panic("viosnd: can't map %zu DMA", size);

	error = bus_dmamap_create(virtio_dmat(vsc), size, 1, size, 0,
	    BUS_DMA_NOWAIT, &dm->dm_map);
	if (error)
		panic("viosnd: can't create %zu DMA map", size);

	error = bus_dmamap_load(virtio_dmat(vsc), dm->dm_map, dm->dm_va,
	    size, NULL, BUS_DMA_WAITOK);
	if (error)
		panic("viosnd: can't load %zu DMA map", size);

	if (dm->dm_map->dm_nsegs != 1)
		panic("viosnd: %zu DMA not contiguous", size);
}

/*
 * Drain the control queue: the device pushes one response per request;
 * the payload beyond the 4-byte status code is not used (no INFO
 * queries are issued) but any length is accepted.
 */
static int
viosnd_ctrl_drain(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct viosnd_softc *sc = device_private(virtio_child(vsc));
	int slot, len, ndone = 0;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		if (len >= (int)sizeof(uint32_t)) {
			bus_dmamap_sync(virtio_dmat(vsc), sc->sc_cresp.dm_map,
			    0, sizeof(uint32_t), BUS_DMASYNC_POSTREAD);
			sc->sc_ctrl_code =
			    *(uint32_t *)sc->sc_cresp.dm_va;
			sc->sc_ctrl_done = true;
		}
		virtio_dequeue_commit(vsc, vq, slot);
		ndone++;
	}
	return ndone;
}

/*
 * The QEMU model does not post events; pop and discard whatever
 * appears so the queue cannot wedge the interrupt handler.
 */
static int
viosnd_event_drain(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	int slot, len, ndone = 0;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		virtio_dequeue_commit(vsc, vq, slot);
		ndone++;
	}
	return ndone;
}

/*
 * Submit one control request and wait for its response.  The QEMU
 * model services the control queue synchronously from the notification
 * MMIO write, so the response is normally already posted when the
 * notify returns; the interrupt path and the bounded poll here both
 * race for the pop, which is fine (viosnd_ctrl_drain is serialized by
 * the virtio core interrupt lock).  Must not be called from interrupt
 * context.
 */
static int
viosnd_ctrl_req(struct viosnd_softc *sc, const void *req, bus_size_t reqlen)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOSND_CONTROLQ];
	int slot, error, i;

	mutex_spin_enter(&sc->sc_ctrl_lock);

	memcpy(sc->sc_creq.dm_va, req, reqlen);
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_creq.dm_map, 0, reqlen,
	    BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_cresp.dm_map, 0,
	    sizeof(uint32_t), BUS_DMASYNC_PREREAD);

	sc->sc_ctrl_done = false;
	sc->sc_ctrl_code = ~0U;

	error = virtio_enqueue_prep(vsc, vq, &slot);
	if (error == 0) {
		error = virtio_enqueue_reserve(vsc, vq, slot, 2);
		if (error != 0) {
			virtio_enqueue_abort(vsc, vq, slot);
		} else {
			virtio_enqueue_p(vsc, vq, slot, sc->sc_creq.dm_map,
			    0, reqlen, true);
			virtio_enqueue_p(vsc, vq, slot, sc->sc_cresp.dm_map,
			    0, sizeof(uint32_t), false);
			virtio_enqueue_commit(vsc, vq, slot, true);
		}
	}

	for (i = 0; i < 2000 && !sc->sc_ctrl_done; i++) {
		viosnd_ctrl_drain(vq);
		if (!sc->sc_ctrl_done)
			delay(100);
	}
	if (!sc->sc_ctrl_done)
		error = ETIMEDOUT;
	else if (sc->sc_ctrl_code != VIOSND_S_OK)
		error = EIO;
	if (error)
		aprint_error_dev(sc->sc_dev, "ctrl 0x%x failed: %d code "
		    "0x%x\n", *(const uint32_t *)req, error, sc->sc_ctrl_code);

	mutex_spin_exit(&sc->sc_ctrl_lock);
	return error;
}

static int
viosnd_pcm_cmd(struct viosnd_softc *sc, uint32_t code)
{
	struct viosnd_pcm_hdr hdr = {
		.code = code,
		.stream_id = VIOSND_STREAM_OUT,
	};

	return viosnd_ctrl_req(sc, &hdr, sizeof(hdr));
}

static int
viosnd_pcm_set_params(struct viosnd_softc *sc)
{
	struct viosnd_pcm_set_params p = {
		.code = VIOSND_R_PCM_SET_PARAMS,
		.stream_id = VIOSND_STREAM_OUT,
		.buffer_bytes = htole32(VIOSND_RINGSZ),
		.period_bytes = htole32(VIOSND_BLKSIZE),
		.features = 0,
		.channels = VIOSND_CHANNELS,
		.format = VIOSND_PCM_FMT_S16,
		.rate = VIOSND_PCM_RATE_48000,
	};

	return viosnd_ctrl_req(sc, &p, sizeof(p));
}

/*
 * Submit one playback block: header (device-readable), block data
 * (device-readable) and status (device-writable).
 */
static int
viosnd_tx_fill_slot(struct viosnd_softc *sc, int idx)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOSND_TXQ];
	struct viosnd_pcm_xfer *hdr;
	int slot, error;

	hdr = (struct viosnd_pcm_xfer *)sc->sc_xhdr.dm_va;
	hdr[idx].stream_id = htole32(VIOSND_STREAM_OUT);

	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_xhdr.dm_map,
	    idx * sizeof(struct viosnd_pcm_xfer),
	    sizeof(struct viosnd_pcm_xfer), BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_ring.dm_map,
	    (bus_size_t)idx * VIOSND_BLKSIZE, VIOSND_BLKSIZE,
	    BUS_DMASYNC_PREREAD);
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_xstat.dm_map,
	    idx * sizeof(struct viosnd_pcm_status),
	    sizeof(struct viosnd_pcm_status), BUS_DMASYNC_PREREAD);

	error = virtio_enqueue_prep(vsc, vq, &slot);
	if (error != 0)
		return error;
	error = virtio_enqueue_reserve(vsc, vq, slot, 3);
	if (error != 0) {
		virtio_enqueue_abort(vsc, vq, slot);
		return error;
	}
	virtio_enqueue_p(vsc, vq, slot, sc->sc_xhdr.dm_map,
	    idx * sizeof(struct viosnd_pcm_xfer),
	    sizeof(struct viosnd_pcm_xfer), true);
	virtio_enqueue_p(vsc, vq, slot, sc->sc_ring.dm_map,
	    (bus_size_t)idx * VIOSND_BLKSIZE, VIOSND_BLKSIZE, true);
	virtio_enqueue_p(vsc, vq, slot, sc->sc_xstat.dm_map,
	    idx * sizeof(struct viosnd_pcm_status),
	    sizeof(struct viosnd_pcm_status), false);
	virtio_enqueue_commit(vsc, vq, slot, 0);
	return 0;
}

/*
 * Tx completion.  The device consumes blocks in submission order, so
 * completions are mapped back to ring slots with a counter.  For each
 * completion the audio core is told one block has been consumed (hole
 * callback refills the ring block) and the slot is resubmitted.
 */
static int
viosnd_tx_intr(struct virtqueue *vq)
{
	struct viosnd_softc *sc = device_private(virtio_child(vq->vq_owner));
	struct virtio_softc *vsc = vq->vq_owner;
	struct viosnd_pcm_status *stat;
	int slot, len, ndone = 0, rearm = 0;

	mutex_spin_enter(&sc->sc_intr_lock);

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		int idx = sc->sc_tx_ack;

		stat = (struct viosnd_pcm_status *)sc->sc_xstat.dm_va;
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_xstat.dm_map,
		    idx * sizeof(*stat), sizeof(*stat),
		    BUS_DMASYNC_POSTREAD);
		if (stat[idx].status != 0)
			sc->sc_errors++;
		virtio_dequeue_commit(vsc, vq, slot);
		ndone++;

		if (sc->sc_playing) {
			if (++sc->sc_tx_ack == VIOSND_NSLOT)
				sc->sc_tx_ack = 0;
			if (sc->sc_hole != NULL)
				sc->sc_hole(sc->sc_hole_arg);
			if (viosnd_tx_fill_slot(sc, idx) == 0)
				rearm++;
		}
	}

	if (ndone > 0 && rearm > 0)
		virtio_notify(vsc, vq);

	mutex_spin_exit(&sc->sc_intr_lock);
	return ndone;
}

/*
 * Audio core entry points.
 */

static int
viosnd_open(void *v, int flags)
{
	return 0;
}

static void
viosnd_close(void *v)
{
	struct viosnd_softc *sc = v;

	viosnd_halt_output(sc);
}

static int
viosnd_query_format(void *v, audio_format_query_t *afp)
{
	struct viosnd_softc *sc = v;

	return audio_query_format(sc->sc_formats, __arraycount(sc->sc_formats),
	    afp);
}

static int
viosnd_set_format(void *v, int setmode,
    const audio_params_t *play, const audio_params_t *rec,
    audio_filter_reg_t *pfil, audio_filter_reg_t *rfil)
{
	struct viosnd_softc *sc = v;

	if ((setmode & AUMODE_PLAY) == 0)
		return EINVAL;
	if (audio_indexof_format(sc->sc_formats,
	    __arraycount(sc->sc_formats), AUMODE_PLAY, play) < 0)
		return EINVAL;

	return 0;
}

static int
viosnd_round_blocksize(void *v, int blk, int mode,
    const audio_params_t *param)
{
	return VIOSND_BLKSIZE;
}

/*
 * Start playback: configure the stream, prime every ring block, then
 * start it.  The audio core only triggers once per open/close pair and
 * the stream configuration is pinned, so SET_PARAMS + PREPARE are
 * idempotent no-ops across re-opens (see the header comment on the
 * device's voice lifecycle).
 */
static int
viosnd_trigger_output(void *v, void *start, void *end, int blksize,
    void (*intr)(void *), void *arg, const audio_params_t *param)
{
	struct viosnd_softc *sc = v;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOSND_TXQ];
	int i, filled = 0;

	if (!sc->sc_ring_valid || start != sc->sc_ring.dm_va ||
	    (char *)end - (char *)start != VIOSND_RINGSZ ||
	    blksize != VIOSND_BLKSIZE) {
		aprint_error_dev(sc->sc_dev, "trigger: bad ring %p-%p "
		    "blk %d\n", start, end, blksize);
		return EINVAL;
	}

	if (viosnd_pcm_set_params(sc) != 0 ||
	    viosnd_pcm_cmd(sc, VIOSND_R_PCM_PREPARE) != 0)
		return EIO;

	/* The audio core calls us with sc_intr_lock already held. */
	sc->sc_hole = intr;
	sc->sc_hole_arg = arg;
	sc->sc_tx_ack = 0;
	sc->sc_playing = true;

	for (i = 0; i < VIOSND_NSLOT; i++) {
		if (viosnd_tx_fill_slot(sc, i) != 0)
			break;
		filled++;
	}
	if (filled > 0)
		virtio_notify(vsc, vq);

	if (viosnd_pcm_cmd(sc, VIOSND_R_PCM_START) != 0) {
		sc->sc_playing = false;
		return EIO;
	}

	return 0;
}

static int
viosnd_halt_output(void *v)
{
	struct viosnd_softc *sc = v;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOSND_TXQ];
	int slot, len;

	/* The audio core calls us with sc_intr_lock already held. */
	if (!sc->sc_playing)
		return 0;
	sc->sc_playing = false;
	sc->sc_hole = NULL;

	viosnd_pcm_cmd(sc, VIOSND_R_PCM_STOP);

	/*
	 * Discard completions queued before the stop, then release the
	 * stream, which flushes whatever the device had not consumed
	 * yet (their used entries land in the queue too).
	 */
	while (virtio_dequeue(vsc, vq, &slot, &len) == 0)
		virtio_dequeue_commit(vsc, vq, slot);

	viosnd_pcm_cmd(sc, VIOSND_R_PCM_RELEASE);

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0)
		virtio_dequeue_commit(vsc, vq, slot);

	return 0;
}

static int
viosnd_trigger_input(void *v, void *start, void *end, int blksize,
    void (*intr)(void *), void *arg, const audio_params_t *param)
{
	return EINVAL;		/* capture not supported */
}

static int
viosnd_getdev(void *v, struct audio_device *adp)
{
	strlcpy(adp->name, "viosnd", sizeof(adp->name));
	strlcpy(adp->version, "virtio-snd", sizeof(adp->version));
	strlcpy(adp->config, "", sizeof(adp->config));
	return 0;
}

static int
viosnd_set_port(void *v, mixer_ctrl_t *cp)
{
	return 0;
}

static int
viosnd_get_port(void *v, mixer_ctrl_t *cp)
{
	return 0;
}

static int
viosnd_query_devinfo(void *v, mixer_devinfo_t *dip)
{
	return EPASSTHROUGH;
}

/*
 * The audio core allocates the output ring (size already rounded by
 * round_buffersize) and hands the same buffer back to trigger_output.
 */
static void *
viosnd_allocm(void *v, int dir, size_t size)
{
	struct viosnd_softc *sc = v;
	struct virtio_softc *vsc = sc->sc_virtio;
	int error, nsegs;

	if (sc->sc_ring_valid)
		return NULL;
	if (size != VIOSND_RINGSZ)
		return NULL;

	sc->sc_ring.dm_size = size;
	error = bus_dmamem_alloc(virtio_dmat(vsc), size, PAGE_SIZE, 0,
	    &sc->sc_ring.dm_seg, 1, &nsegs, BUS_DMA_WAITOK);
	if (error)
		return NULL;
	error = bus_dmamem_map(virtio_dmat(vsc), &sc->sc_ring.dm_seg, nsegs,
	    size, &sc->sc_ring.dm_va, BUS_DMA_WAITOK);
	if (error) {
		bus_dmamem_free(virtio_dmat(vsc), &sc->sc_ring.dm_seg, nsegs);
		return NULL;
	}
	error = bus_dmamap_create(virtio_dmat(vsc), size, 1, size, 0,
	    BUS_DMA_NOWAIT, &sc->sc_ring.dm_map);
	if (error)
		goto unmap;
	error = bus_dmamap_load(virtio_dmat(vsc), sc->sc_ring.dm_map,
	    sc->sc_ring.dm_va, size, NULL, BUS_DMA_WAITOK);
	if (error)
		goto destroy;
	if (sc->sc_ring.dm_map->dm_nsegs != 1)
		goto unload;

	sc->sc_ring_valid = true;
	return sc->sc_ring.dm_va;

unload:
	bus_dmamap_unload(virtio_dmat(vsc), sc->sc_ring.dm_map);
destroy:
	bus_dmamap_destroy(virtio_dmat(vsc), sc->sc_ring.dm_map);
unmap:
	bus_dmamem_unmap(virtio_dmat(vsc), sc->sc_ring.dm_va, size);
	bus_dmamem_free(virtio_dmat(vsc), &sc->sc_ring.dm_seg, 1);
	return NULL;
}

static void
viosnd_freem(void *v, void *ptr, size_t size)
{
	struct viosnd_softc *sc = v;
	struct virtio_softc *vsc = sc->sc_virtio;

	if (!sc->sc_ring_valid || ptr != sc->sc_ring.dm_va)
		return;

	bus_dmamap_unload(virtio_dmat(vsc), sc->sc_ring.dm_map);
	bus_dmamap_destroy(virtio_dmat(vsc), sc->sc_ring.dm_map);
	bus_dmamem_unmap(virtio_dmat(vsc), sc->sc_ring.dm_va, size);
	bus_dmamem_free(virtio_dmat(vsc), &sc->sc_ring.dm_seg, 1);
	sc->sc_ring_valid = false;
}

static size_t
viosnd_round_buffersize(void *v, int direction, size_t size)
{
	return VIOSND_RINGSZ;
}

static int
viosnd_get_props(void *v)
{
	return AUDIO_PROP_PLAYBACK;
}

static int
viosnd_dev_ioctl(void *v, u_long cmd, void *data, int flag, struct lwp *l)
{
	return EPASSTHROUGH;
}

static void
viosnd_get_locks(void *v, kmutex_t **iplk, kmutex_t **prcl)
{
	struct viosnd_softc *sc = v;

	*iplk = &sc->sc_intr_lock;
	*prcl = &sc->sc_lock;
}

static void
viosnd_attach(device_t parent, device_t self, void *aux)
{
	struct viosnd_softc *sc = device_private(self);
	struct virtio_softc *vsc = device_private(parent);
	uint32_t streams;

	if (virtio_child(vsc) != NULL) {
		aprint_error("child already attached for %s\n",
		    device_xname(parent));
		return;
	}

	sc->sc_dev = self;
	sc->sc_virtio = vsc;
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_AUDIO);
	mutex_init(&sc->sc_ctrl_lock, MUTEX_DEFAULT, IPL_AUDIO);

	sc->sc_formats[0].mode = AUMODE_PLAY;
	sc->sc_formats[0].encoding = AUDIO_ENCODING_SLINEAR_LE;
	sc->sc_formats[0].validbits = 16;
	sc->sc_formats[0].precision = 16;
	sc->sc_formats[0].channels = VIOSND_CHANNELS;
	sc->sc_formats[0].channel_mask = 0x3;	/* FL | FR */
	sc->sc_formats[0].frequency_type = 1;
	sc->sc_formats[0].frequency[0] = VIOSND_RATE;

	virtio_child_attach_start(vsc, self, IPL_AUDIO, 0,
	    VIRTIO_COMMON_FLAG_BITS);

	streams = virtio_read_device_config_le_4(vsc, VIOSND_CFG_STREAMS);
	if (streams < 1) {
		aprint_error(": no pcm streams\n");
		goto fail;
	}

	viosnd_dma_alloc(sc, &sc->sc_creq, 64);
	viosnd_dma_alloc(sc, &sc->sc_cresp, 64);
	viosnd_dma_alloc(sc, &sc->sc_xhdr, VIOSND_NSLOT *
	    sizeof(struct viosnd_pcm_xfer));
	viosnd_dma_alloc(sc, &sc->sc_xstat, VIOSND_NSLOT *
	    sizeof(struct viosnd_pcm_status));

	virtio_init_vq_vqdone(vsc, &sc->sc_vqs[VIOSND_CONTROLQ],
	    VIOSND_CONTROLQ, viosnd_ctrl_drain);
	if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOSND_CONTROLQ], 64, 2,
	    "control") != 0) {
		aprint_error(": can't alloc control virtqueue\n");
		goto fail;
	}
	virtio_init_vq_vqdone(vsc, &sc->sc_vqs[VIOSND_EVENTQ],
	    VIOSND_EVENTQ, viosnd_event_drain);
	if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOSND_EVENTQ], 16, 1,
	    "event") != 0) {
		aprint_error(": can't alloc event virtqueue\n");
		goto fail;
	}
	virtio_init_vq_vqdone(vsc, &sc->sc_vqs[VIOSND_TXQ], VIOSND_TXQ,
	    viosnd_tx_intr);
	if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOSND_TXQ], VIOSND_BLKSIZE,
	    3, "tx") != 0) {
		aprint_error(": can't alloc tx virtqueue\n");
		goto fail;
	}
	if (sc->sc_vqs[VIOSND_TXQ].vq_num < VIOSND_NSLOT) {
		aprint_error(": tx queue too short (%u)\n",
		    sc->sc_vqs[VIOSND_TXQ].vq_num);
		goto fail;
	}

	if (virtio_child_attach_finish(vsc, sc->sc_vqs, VIOSND_NVQ, NULL,
	    0) != 0) {
		aprint_error(": can't finish attach\n");
		goto fail;
	}

	virtio_start_vq_intr(vsc, &sc->sc_vqs[VIOSND_CONTROLQ]);
	virtio_start_vq_intr(vsc, &sc->sc_vqs[VIOSND_TXQ]);

	aprint_normal(": virtio-snd, %u streams\n", streams);

	audio_attach_mi(&viosnd_hw_if, sc, self);

	return;

fail:
	virtio_child_attach_failed(vsc);
}
