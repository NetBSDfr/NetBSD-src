/* $NetBSD: virtio_mmiovar.h,v 1.7 2024/01/06 06:59:33 thorpej Exp $ */
/*
 * Copyright (c) 2018 Jonathan A. Kollasch
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _VIRTIO_MMIOVAR_H_
#define _VIRTIO_MMIOVAR_H_

#define VIRTIO_PRIVATE

#include <dev/pci/virtiovar.h> /* XXX: move to non-pci */

#define VIRTIO_MMIO_MAGIC		('v' | 'i' << 8 | 'r' << 16 | 't' << 24)

#define VIRTIO_MMIO_MAGIC_VALUE		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010	/* "HostFeatures" in v1 */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014	/* "HostFeaturesSel" in v1 */
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020	/* "GuestFeatures" in v1 */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024	/* "GuestFeaturesSel" in v1 */
#define VIRTIO_MMIO_V1_GUEST_PAGE_SIZE	0x028
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_V1_QUEUE_ALIGN	0x03c
#define VIRTIO_MMIO_V1_QUEUE_PFN	0x040
#define	VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define	VIRTIO_MMIO_V2_QUEUE_DESC_LOW	0x080
#define	VIRTIO_MMIO_V2_QUEUE_DESC_HIGH	0x084
#define	VIRTIO_MMIO_V2_QUEUE_AVAIL_LOW	0x090
#define	VIRTIO_MMIO_V2_QUEUE_AVAIL_HIGH	0x094
#define	VIRTIO_MMIO_V2_QUEUE_USED_LOW	0x0a0
#define	VIRTIO_MMIO_V2_QUEUE_USED_HIGH	0x0a4
#define	VIRTIO_MMIO_V2_CONFIG_GEN	0x0fc
#define VIRTIO_MMIO_CONFIG		0x100

struct virtio_mmio_softc {
	struct virtio_softc	sc_sc;

	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_size_t		sc_iosize;
	uint32_t		sc_mmio_vers;
	bool			sc_le_regs;

	void			*sc_ih;

	int			(*sc_alloc_interrupts)(struct virtio_mmio_softc *);
	void			(*sc_free_interrupts)(struct virtio_mmio_softc *);

};

struct mmio_args {
	bus_space_tag_t		bst;
	uint64_t		sz;
	paddr_t			baseaddr;
	uint64_t		irq;
	uint64_t		id;
};

uint32_t virtio_mmio_reg_read(struct virtio_mmio_softc *, bus_addr_t);
bool virtio_mmio_common_probe_present(struct virtio_mmio_softc *);
void virtio_mmio_common_attach(struct virtio_mmio_softc *);
int virtio_mmio_common_detach(struct virtio_mmio_softc *, int);
int virtio_mmio_intr(void *);

extern int (*enumerate_mmio_devices)(struct mmio_args *);

#endif /* _VIRTIO_MMIOVAR_H_ */
