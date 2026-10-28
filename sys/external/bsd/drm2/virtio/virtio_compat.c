/*	$NetBSD: virtio_compat.c,v 1.1 2026-10-26 smolBSD Exp $	*/

/*-
 * Copyright (c) 2026 smolBSD project
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * LinuxKPI virtio API emulated on the NetBSD virtio(4) bus, for the
 * virtio-gpu DRM driver (external/bsd/drm2/dist/drm/virtio).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: virtio_compat.c,v 1.1 2026-10-26 smolBSD Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <machine/cpu.h>
#include <sys/mutex.h>

/*
 * Include the NetBSD bus first, under a different tag name, so that the
 * Linux-shaped struct virtqueue from <linux/virtio.h> can coexist with
 * the NetBSD one in this translation unit.
 */
#define	virtqueue	nbsd_virtqueue
#include <dev/pci/virtiovar.h>
#undef	virtqueue

#include <dev/pci/virtioreg.h>

#include <drm/bus_dma_hacks.h>		/* bus_dmamap_load_pages */

#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/slab.h>

#include "virtgpu_drv.h"

/*
 * Per-device registry.  There are typically at most a couple of virtio
 * GPU devices, so a singly-linked list with linear lookups is plenty.
 */
struct virtio_compat_dev {
	struct virtio_device	cd_vdev;
	bool			cd_finished;
	TAILQ_ENTRY(virtio_compat_dev) cd_next;
};

static kmutex_t		virtio_compat_lock;
static bool		virtio_compat_inited = false;
static TAILQ_HEAD(, virtio_compat_dev) virtio_compat_devs =
    TAILQ_HEAD_INITIALIZER(virtio_compat_devs);

#define	VIRTIO_VQ_MAXNSEGS	1024	/* descriptors per request */

static struct virtio_compat_dev *
virtio_compat_lookup_locked(device_t dev)
{
	struct virtio_compat_dev *cd;

	TAILQ_FOREACH(cd, &virtio_compat_devs, cd_next)
		if (cd->cd_vdev.dev == dev)
			return cd;
	return NULL;
}

static struct virtio_compat_dev *
virtio_compat_lookup_sc_locked(void *sc)
{
	struct virtio_compat_dev *cd;

	TAILQ_FOREACH(cd, &virtio_compat_devs, cd_next)
		if (cd->cd_vdev.sc == sc)
			return cd;
	return NULL;
}

struct virtio_device *
virtio_get_compat(device_t dev)
{
	struct virtio_compat_dev *cd;

	mutex_enter(&virtio_compat_lock);
	cd = virtio_compat_lookup_locked(dev);
	mutex_exit(&virtio_compat_lock);
	return cd != NULL ? &cd->cd_vdev : NULL;
}

struct virtio_device *
virtio_get_compat_by_softc(void *sc)
{
	struct virtio_compat_dev *cd;

	mutex_enter(&virtio_compat_lock);
	cd = virtio_compat_lookup_sc_locked(sc);
	mutex_exit(&virtio_compat_lock);
	return cd != NULL ? &cd->cd_vdev : NULL;
}

struct virtio_device;

struct virtio_compat_dev;

struct virtio_device;

static void	virtio_compat_reset(struct virtio_device *);

static const struct virtio_config_ops virtio_compat_config_ops = {
	.reset		= virtio_compat_reset,
	.del_vqs	= virtio_del_vqs,
};

struct virtio_device *
virtio_create_compat(device_t self, void *sc)
{
	struct virtio_compat_dev *cd;

	/*
	 * One-time init before the first use of the lock: device attach
	 * (and hence this function) runs serialized under kernel_lock at
	 * boot.  The lock must be a spin mutex (IPL_VM or above): the
	 * vq/config trampolines take it from hard-interrupt context
	 * (viogpu children attach at IPL_VM), which is illegal for an
	 * adaptive mutex and wedges the kernel without LOCKDEBUG.
	 */
	if (!virtio_compat_inited) {
		mutex_init(&virtio_compat_lock, MUTEX_DEFAULT, IPL_VM);
		virtio_compat_inited = true;
	}

	cd = kmem_zalloc(sizeof(*cd), KM_SLEEP);
	mutex_enter(&virtio_compat_lock);
	cd->cd_vdev.dev = self;
	cd->cd_vdev.sc = sc;
	cd->cd_vdev.config = &virtio_compat_config_ops;
	TAILQ_INSERT_TAIL(&virtio_compat_devs, cd, cd_next);
	mutex_exit(&virtio_compat_lock);

	return &cd->cd_vdev;
}

void
virtio_destroy_compat(struct virtio_device *vdev)
{
	struct virtio_compat_dev *cd =
	    container_of(vdev, struct virtio_compat_dev, cd_vdev);

	mutex_enter(&virtio_compat_lock);
	TAILQ_REMOVE(&virtio_compat_devs, cd, cd_next);
	mutex_exit(&virtio_compat_lock);
	kmem_free(cd, sizeof(*cd));
}

/*
 * Bus attachment helpers.
 */

int
virtio_compat_negotiate(struct virtio_device *vdev, uint64_t features)
{
	struct virtio_softc *sc = vdev->sc;

	virtio_child_attach_start(sc, vdev->dev, IPL_VM, features,
	    VIRTIO_COMMON_FLAG_BITS);
	return 0;
}

static int
virtio_compat_cfg_trampoline(struct virtio_softc *sc)
{
	struct virtio_compat_dev *cd;
	void (*cb)(void *);
	void *arg;

	mutex_enter(&virtio_compat_lock);
	cd = virtio_compat_lookup_sc_locked(sc);
	cb = cd != NULL ? cd->cd_vdev.cfg_cb : NULL;
	arg = cd != NULL ? &cd->cd_vdev : NULL;
	mutex_exit(&virtio_compat_lock);

	if (cb != NULL)
		cb(arg);
	return 0;
}

int
virtio_compat_finish(struct virtio_device *vdev, void (*cfgcb)(void *))
{
	struct virtio_compat_dev *cd =
	    container_of(vdev, struct virtio_compat_dev, cd_vdev);

	/*
	 * The NetBSD child attach actually completed in virtio_find_vqs()
	 * (the Linux driver starts sending commands immediately after its
	 * virtqueues are set up, so interrupts must be live by then);
	 * this call only refreshes the config-change callback.
	 */
	vdev->cfg_cb = cfgcb;

	return cd->cd_finished ? 0 : -EIO;
}

/*
 * Virtqueues.
 */

/* NetBSD vq_done trampoline; locate the wrapper by identity. */
static int
virtio_compat_vq_trampoline(struct nbsd_virtqueue *nvq)
{
	struct virtio_softc *sc = nvq->vq_owner;
	struct virtio_compat_dev *cd;
	struct virtqueue *vq = NULL;
	int i;

	mutex_enter(&virtio_compat_lock);
	cd = virtio_compat_lookup_sc_locked(sc);
	if (cd != NULL) {
		for (i = 0; i < cd->cd_vdev.nvqs; i++) {
			if (cd->cd_vdev.vqs[i]->vq_nvq == nvq) {
				vq = cd->cd_vdev.vqs[i];
				break;
			}
		}
	}
	mutex_exit(&virtio_compat_lock);

	if (vq != NULL && vq->vq_callback != NULL)
		vq->vq_callback(vq);
	return 0;
}

int
virtio_find_vqs(struct virtio_device *vdev, unsigned nvqs,
    struct virtqueue **vqs, vq_callback_t **callbacks,
    const char * const names[], const bool *ctx)
{
	struct virtio_softc *sc = vdev->sc;
	struct nbsd_virtqueue *nvqarr;
	unsigned i;

	if (ctx != NULL)
		return -EINVAL;

	vdev->vqs = kmem_zalloc(sizeof(*vdev->vqs) * nvqs, KM_SLEEP);
	vdev->nvqs = nvqs;

	/*
	 * The NetBSD bus indexes the virtqueue structures as an array
	 * (virtio_child_attach_finish takes the base pointer), so they
	 * must be one contiguous allocation.
	 */
	nvqarr = kmem_zalloc(sizeof(*nvqarr) * nvqs, KM_SLEEP);

	for (i = 0; i < nvqs; i++) {
		struct virtqueue *vq;
		struct nbsd_virtqueue *nvq;

		vq = kmem_zalloc(sizeof(*vq), KM_SLEEP);
		nvq = &nvqarr[i];

		virtio_init_vq_vqdone(sc, nvq, i, virtio_compat_vq_trampoline);
		if (virtio_alloc_vq(sc, nvq, NBPG, VIRTIO_VQ_MAXNSEGS,
		    names != NULL ? names[i] : "drm") != 0) {
			kmem_free(vq, sizeof(*vq));
			goto fail;
		}

		vq->vq_sc = sc;
		vq->vq_nvq = nvq;
		vq->vdev = vdev;
		vq->vq_name = names != NULL ? names[i] : NULL;
		vq->vq_callback = callbacks != NULL ? callbacks[i] : NULL;
		vq->vq_ncookies = nvq->vq_num;
		vq->vq_cookies = kmem_zalloc(
		    sizeof(*vq->vq_cookies) * vq->vq_ncookies, KM_SLEEP);
		vq->vq_slotn = kmem_zalloc(
		    sizeof(*vq->vq_slotn) * vq->vq_ncookies, KM_SLEEP);
		vq->num_free = nvq->vq_num;

		vqs[i] = vq;
		vdev->vqs[i] = vq;
	}

	/*
	 * Complete the NetBSD child attach now: the Linux driver may
	 * queue commands as soon as find_vqs returns, and completion of
	 * those commands requires the vq interrupts, which
	 * virtio_child_attach_finish() establishes.  The NetBSD bus
	 * indexes the virtqueue structures as an array, and nvqarr is
	 * that contiguous array.
	 */
	{
		struct virtio_compat_dev *cd =
		    container_of(vdev, struct virtio_compat_dev, cd_vdev);
		int error;

		error = virtio_child_attach_finish(sc, nvqarr, nvqs,
		    virtio_compat_cfg_trampoline,
		    VIRTIO_F_INTR_MPSAFE | VIRTIO_F_INTR_SOFTINT);
		if (error != 0)
			return -EIO;
		cd->cd_finished = true;
	}

	return 0;

fail:
	for (i = 0; i < nvqs; i++) {
		if (vdev->vqs[i] == NULL)
			continue;
		virtio_free_vq(sc, vdev->vqs[i]->vq_nvq);
		kmem_free(vdev->vqs[i]->vq_cookies,
		    sizeof(*vdev->vqs[i]->vq_cookies) *
		    vdev->vqs[i]->vq_ncookies);
		kmem_free(vdev->vqs[i]->vq_slotn,
		    sizeof(*vdev->vqs[i]->vq_slotn) *
		    vdev->vqs[i]->vq_ncookies);
		kmem_free(vdev->vqs[i], sizeof(**vqs));
	}
	kmem_free(nvqarr, sizeof(*nvqarr) * nvqs);
	kmem_free(vdev->vqs, sizeof(*vdev->vqs) * nvqs);
	vdev->vqs = NULL;
	vdev->nvqs = 0;
	return -ENOMEM;
}

void
virtio_del_vqs(struct virtio_device *vdev)
{
	struct virtio_softc *sc = vdev->sc;
	struct nbsd_virtqueue *nvqs;
	int i;

	if (vdev->vqs == NULL)
		return;
	nvqs = vdev->vqs[0]->vq_nvq;

	for (i = 0; i < vdev->nvqs; i++) {
		struct virtqueue *vq = vdev->vqs[i];

		virtio_free_vq(sc, vq->vq_nvq);
		kmem_free(vq->vq_cookies,
		    sizeof(*vq->vq_cookies) * vq->vq_ncookies);
		kmem_free(vq->vq_slotn,
		    sizeof(*vq->vq_slotn) * vq->vq_ncookies);
		kmem_free(vq, sizeof(*vq));
	}
	kmem_free(nvqs, sizeof(*nvqs) * vdev->nvqs);
	kmem_free(vdev->vqs, sizeof(*vdev->vqs) * vdev->nvqs);
	vdev->vqs = NULL;
	vdev->nvqs = 0;
}

static void
virtio_compat_reset(struct virtio_device *vdev)
{

	virtio_reset(vdev->sc);
}

/*
 * Bus DMA tag for a device.
 */
bus_dma_tag_t
virtio_dev_dmat(struct virtio_device *vdev)
{

	return virtio_dmat(vdev->sc);
}

/*
 * Descriptor submission.
 *
 * Map one scatterlist entry and append all of its segments to the
 * request.  Entries may be:
 *   - already mapped by dma_map_sg() (GEM objects): use as-is;
 *   - page arrays (sg_alloc_table[_from_pages] + sg_set_page): map via
 *     bus_dmamap_load_pages(), map owned by the entry and released by
 *     sg_free_table() by the caller once the buffer is retired;
 *   - kernel virtual buffers (sg_init_one): mapped and released around
 *     the submission itself (virtio_enqueue copies the segment list
 *     into the ring; x86-type DMA tags do not bounce kernel buffers).
 */
static int
virtio_compat_map_entry(struct virtio_softc *sc, struct scatterlist *sg,
    bool write)
{
	bus_dma_tag_t dmat = virtio_dmat(sc);
	int flags = write ? BUS_DMA_READ : BUS_DMA_WRITE;
	int error;

	if (sg->sg_dmamap != NULL)
		return 0;

	if (sg->sg_kva != NULL) {
		vaddr_t kva = (vaddr_t)sg->sg_kva;
		size_t len = sg->sg_len, align;
		bus_dmamap_t map;

		/*
		 * _BUS_VIRT_TO_BUS() (kvtop) drops the intra-page
		 * offset and non-contiguous physical chunks become
		 * separate segments, so build the segment list by hand
		 * one page at a time.
		 */
		error = bus_dmamap_create(dmat, len, howmany((kva & (PAGE_SIZE - 1))
		    + len, PAGE_SIZE), len, 0, BUS_DMA_WAITOK,
		    &sg->sg_dmamap);
		if (error != 0)
			return -ENOMEM;
		map = sg->sg_dmamap;
		map->dm_nsegs = 0;
		while (len != 0) {
			align = MIN(PAGE_SIZE - (kva & (PAGE_SIZE - 1)), len);
			if (map->dm_nsegs != 0 &&
			    map->dm_segs[map->dm_nsegs - 1].ds_addr +
			    map->dm_segs[map->dm_nsegs - 1].ds_len ==
			    kvtop((void *)kva))
				map->dm_segs[map->dm_nsegs - 1].ds_len +=
				    align;
			else {
				map->dm_segs[map->dm_nsegs].ds_addr =
				    kvtop((void *)kva);
				map->dm_segs[map->dm_nsegs].ds_len = align;
				map->dm_nsegs++;
			}
			kva += align;
			len -= align;
		}
		map->dm_mapsize = sg->sg_len;
		sg->sg_dmat = dmat;
		return 0;
	}

	if (sg->sg_pgs == NULL || sg->sg_npgs == 0)
		return -EINVAL;

	error = bus_dmamap_create(dmat, (bus_size_t)sg->sg_npgs << PAGE_SHIFT,
	    sg->sg_npgs, PAGE_SIZE, 0, BUS_DMA_WAITOK, &sg->sg_dmamap);
	if (error != 0)
		return -ENOMEM;
	error = bus_dmamap_load_pages(dmat, sg->sg_dmamap, sg->sg_pgs,
	    (bus_size_t)sg->sg_npgs << PAGE_SHIFT, BUS_DMA_WAITOK | flags);
	if (error != 0) {
		bus_dmamap_destroy(dmat, sg->sg_dmamap);
		sg->sg_dmamap = NULL;
		return -ENOMEM;
	}
	sg->sg_dmat = dmat;
	return 0;
}

/*
 * Append an entry's segments to the request.  Maps are established by
 * virtqueue_add_sgs() before the ring reservation; kva
 * (sg_init_one) maps are released by the caller right after the
 * request since the ring copies the segment list and x86-class DMA
 * tags do not bounce kernel buffers; page-array and pre-mapped
 * entries keep their maps until the caller retires the buffer.
 */
static int
virtio_compat_enqueue_entry(struct virtio_softc *sc,
    struct nbsd_virtqueue *nvq, int slot, struct scatterlist *sg, bool write)
{

	virtio_enqueue(sc, nvq, slot, sg->sg_dmamap, write);
	return sg->sg_dmamap->dm_nsegs;
}

int
virtqueue_add_sgs(struct virtqueue *vq, struct scatterlist *sgs[],
    unsigned int out_sgs, unsigned int in_sgs, void *data, gfp_t gfp)
{
	struct virtio_softc *sc = vq->vq_sc;
	struct nbsd_virtqueue *nvq = vq->vq_nvq;
	unsigned int total_segs = 0, i;
	int slot, r, error = 0;

	/*
	 * Map every entry up front: the ring must be reserved with the
	 * exact number of DMA segments (indirect descriptor tables are
	 * sized at reserve time and padded with stale entries).
	 */
	for (i = 0; i < out_sgs + in_sgs; i++) {
		error = virtio_compat_map_entry(sc, sgs[i], i < out_sgs);
		if (error != 0)
			goto unmap;
		total_segs += sgs[i]->sg_dmamap->dm_nsegs;
	}

	r = virtio_enqueue_prep(sc, nvq, &slot);
	if (r != 0)
		return -ENOSPC;

	r = virtio_enqueue_reserve(sc, nvq, slot, total_segs);
	if (r != 0) {
		virtio_enqueue_abort(sc, nvq, slot);
		return -ENOSPC;
	}

	/*
	 * The ring descriptors are filled from each entry's DMA map at
	 * enqueue time.  kva (sg_init_one) entries are unmapped right
	 * after this call; page-array and pre-mapped entries keep their
	 * maps until the caller retires the buffer.
	 */
	for (i = 0; i < out_sgs; i++) {
		r = virtio_compat_enqueue_entry(sc, nvq, slot, sgs[i], true);
		if (r < 0) {
			virtio_enqueue_abort(sc, nvq, slot);
			error = r;
			goto unmap;
		}
	}
	for (i = 0; i < in_sgs; i++) {
		r = virtio_compat_enqueue_entry(sc, nvq, slot, sgs[out_sgs + i],
		    false);
		if (r < 0) {
			virtio_enqueue_abort(sc, nvq, slot);
			error = r;
			goto unmap;
		}
	}

	vq->vq_last_slot = slot;
	if (data != NULL) {
		KASSERTMSG(slot < vq->vq_ncookies,
		    "slot %u cookies %u", slot, vq->vq_ncookies);
		vq->vq_cookies[slot] = data;
		vq->vq_slotn[slot] = total_segs;
	}
	vq->num_free -= total_segs;

	/*
	 * Publish the avail ring entry now (without ringing the doorbell):
	 * Linux semantics are that virtqueue_add_sgs() makes the buffer
	 * visible to the device and virtqueue_notify() is only a kick.
	 * Deferring the entry until notify time collapses to a single
	 * vq_last_slot, so multiple adds before one notify drop every
	 * ring entry but the last.
	 */
	virtio_enqueue_commit(sc, nvq, slot, false);

unmap:
	/* Release kva maps: descriptors copied, buffers stay valid. */
	for (i = 0; i < out_sgs + in_sgs; i++) {
		struct scatterlist *sg = sgs[i];

		if (sg->sg_kva != NULL && sg->sg_dmamap != NULL) {
			bus_dmamap_unload(virtio_dmat(sc), sg->sg_dmamap);
			bus_dmamap_destroy(virtio_dmat(sc), sg->sg_dmamap);
			sg->sg_dmamap = NULL;
			sg->sg_dmat = NULL;
		}
	}

	return error;
}

void *
virtqueue_get_buf(struct virtqueue *vq, unsigned int *len)
{
	struct virtio_softc *sc = vq->vq_sc;
	struct nbsd_virtqueue *nvq = vq->vq_nvq;
	int slot = 0, rlen = 0, r;
	void *data;

	r = virtio_dequeue(sc, nvq, &slot, &rlen);
	if (r != 0) {
		/* No used entry: slot is unset.  Do NOT commit here or
		 * slot 0 is pushed onto the free list once per empty
		 * poll, duplicating it and aliasing concurrent requests.
		 */
		return NULL;
	}

	data = NULL;
	if ((unsigned)slot < vq->vq_ncookies) {
		data = vq->vq_cookies[slot];
		vq->vq_cookies[slot] = NULL;
		if (vq->vq_slotn != NULL) {
			vq->num_free += vq->vq_slotn[slot];
			vq->vq_slotn[slot] = 0;
		}
	}
	virtio_dequeue_commit(sc, nvq, slot);
	if (len != NULL)
		*len = rlen;
	return data;
}

bool
virtqueue_kick_prepare(struct virtqueue *vq)
{

	return true;
}

bool
virtqueue_notify(struct virtqueue *vq)
{

	/* Doorbell only: flush the published avail idx and kick. */
	virtio_enqueue_commit(vq->vq_sc, vq->vq_nvq, -1, true);
	return true;
}

bool
virtqueue_kick(struct virtqueue *vq)
{

	virtqueue_notify(vq);
	return true;
}

void
virtqueue_disable_cb(struct virtqueue *vq)
{

	virtio_stop_vq_intr(vq->vq_sc, vq->vq_nvq);
}

bool
virtqueue_enable_cb(struct virtqueue *vq)
{

	virtio_start_vq_intr(vq->vq_sc, vq->vq_nvq);
	return true;
}

bool
virtqueue_enable_cb_prepare(struct virtqueue *vq)
{

	virtqueue_enable_cb(vq);
	return true;
}

unsigned int
virtqueue_get_vring_size(const struct virtqueue *vq)
{
	const struct nbsd_virtqueue *nvq = vq->vq_nvq;

	return nvq->vq_num;
}

unsigned int
virtqueue_get_free(const struct virtqueue *vq)
{

	return vq->num_free;
}

/*
 * Device config space and features.
 */

bool
virtio_has_feature(const struct virtio_device *vdev, unsigned int bit)
{
	struct virtio_softc *sc = vdev->sc;

	return (virtio_features(sc) & (1ULL << bit)) != 0;
}

void
virtio_read_config_compat(const struct virtio_device *vdev, size_t off,
    void *dst, size_t len)
{
	struct virtio_softc *sc = vdev->sc;
	uint8_t *out = dst;

	switch (len) {
	case 1:
		*out = virtio_read_device_config_1(sc, off);
		break;
	case 2:
		*(uint16_t *)dst = virtio_read_device_config_le_2(sc, off);
		break;
	case 4:
		*(uint32_t *)dst = virtio_read_device_config_le_4(sc, off);
		break;
	case 8:
		*(uint64_t *)dst = virtio_read_device_config_8(sc, off);
		break;
	default:
		panic("virtio_read_config_compat: len %zu", len);
	}
}

void
virtio_write_config_compat(const struct virtio_device *vdev, size_t off,
    const void *src, size_t len)
{
	struct virtio_softc *sc = vdev->sc;
	const uint8_t *in = src;

	switch (len) {
	case 1:
		virtio_write_device_config_1(sc, off, *in);
		break;
	case 2:
		virtio_write_device_config_le_2(sc, off, *(const uint16_t *)src);
		break;
	case 4:
		virtio_write_device_config_le_4(sc, off, *(const uint32_t *)src);
		break;
	case 8:
		virtio_write_device_config_8(sc, off, *(const uint64_t *)src);
		break;
	default:
		panic("virtio_write_config_compat: len %zu", len);
	}
}

/*
 * virtiodrm(4) device front end: match/attach/detach over the virtio bus,
 * calling into the Linux-derived virtio-gpu DRM driver.
 */

int	virtiodrm_match(device_t, cfdata_t, void *);
void	virtiodrm_attach(device_t, device_t, void *);
int	virtiodrm_detach(device_t, int);

int
virtiodrm_match(device_t parent, cfdata_t match, void *aux)
{
	struct virtio_attach_args * const va = aux;

	if (va->sc_childdevid != VIRTIO_DEVICE_ID_GPU)
		return 0;

	/*
	 * Plain virtio GPUs are left to the viogpu(4) wscons console;
	 * only the virgl-capable device (QEMU virtio-gpu-gl-device)
	 * belongs to DRM/KMS.  Outranking viogpu_match() makes the
	 * split deterministic when both drivers are configured.
	 */
	if ((va->sc_device_features & (1ULL << VIRTIO_GPU_F_VIRGL)) == 0)
		return 0;
	return 2;
}

#define	VIRTIODRM_FEATURES						\
	((1ULL << VIRTIO_GPU_F_VIRGL) |					\
	 (1ULL << VIRTIO_GPU_F_EDID) |					\
	 (1ULL << VIRTIO_GPU_F_RESOURCE_UUID))

static void
virtiodrm_config_changed(void *arg)
{
	struct virtio_device *vdev = arg;

	virtio_gpu_config_changed(vdev);
}

void
virtiodrm_attach(device_t parent, device_t self, void *aux)
{
	struct virtio_softc *vsc = device_private(parent);
	struct virtio_device *vdev;

	if (virtio_child(vsc) != NULL) {
		aprint_error_dev(self, "virtio parent already has a child\n");
		return;
	}

	vdev = virtio_create_compat(self, vsc);
	vdev->is_pci = false;		/* virtio-mmio only, for now */

	virtio_compat_negotiate(vdev, VIRTIODRM_FEATURES);

	/* may fire as soon as virtio_find_vqs() completes the attach */
	vdev->cfg_cb = virtiodrm_config_changed;

	if (!virtio_version_1(vsc)) {
		aprint_error_dev(self, "requires virtio version 1\n");
		goto fail;
	}

	if (virtio_gpu_probe(vdev) != 0) {
		aprint_error_dev(self, "probe failed\n");
		goto fail;
	}

	if (virtio_compat_finish(vdev, virtiodrm_config_changed) != 0) {
		aprint_error_dev(self, "attach finish failed\n");
		goto fail;
	}

	aprint_normal_dev(self, "virtio GPU\n");
	return;

fail:
	virtio_child_attach_failed(vsc);
	virtio_destroy_compat(vdev);
}

int
virtiodrm_detach(device_t parent, int flags)
{
	struct virtio_softc *vsc = device_private(parent);
	struct virtio_device *vdev = virtio_get_compat_by_softc(vsc);

	if (vdev == NULL)
		return 0;

	if (vdev->priv != NULL)
		virtio_gpu_remove(vdev);
	virtio_child_detach(vsc);
	virtio_destroy_compat(vdev);
	return 0;
}

CFATTACH_DECL_NEW(virtiodrm, 0,
    virtiodrm_match, virtiodrm_attach, virtiodrm_detach, NULL);
