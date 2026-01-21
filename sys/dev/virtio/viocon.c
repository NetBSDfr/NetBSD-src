/*	$NetBSD: viocon.c,v 1.10 2024/08/05 19:13:34 riastradh Exp $	*/
/*	$OpenBSD: viocon.c,v 1.8 2021/11/05 11:38:29 mpi Exp $	*/

/*
 * Copyright (c) 2013-2015 Stefan Fritsch <sf@sfritsch.de>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: viocon.c,v 1.10 2024/08/05 19:13:34 riastradh Exp $");

#include <sys/param.h>
#include <sys/types.h>

#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/kauth.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/fcntl.h>
#include <sys/poll.h>

#include <dev/cons.h>
#include <dev/virtio/virtio_vioconvar.h>
#include <dev/virtio/virtio_mmioreg.h>

#define VIRTIO_PRIVATE
#include <dev/pci/virtioreg.h>
#include <dev/pci/virtiovar.h>

#include <prop/proplib.h>

#include "ioconf.h"

/* OpenBSD compat shims */
#define	ttymalloc(speed)	tty_alloc()
#define	splassert(ipl)		__nothing
#define	virtio_notify(vsc, vq)	virtio_enqueue_commit(vsc, vq, -1, true)
#define	ttwakeupwr(tp)		__nothing

/* features */
#define	VIRTIO_CONSOLE_F_SIZE		(1ULL<<0)
#define	VIRTIO_CONSOLE_F_MULTIPORT	(1ULL<<1)
#define	VIRTIO_CONSOLE_F_EMERG_WRITE 	(1ULL<<2)

#define VIRTIO_CONSOLE_BAD_ID		(~(uint32_t)0)

/* config space */
#define VIRTIO_CONSOLE_COLS		0	/* 16 bits */
#define VIRTIO_CONSOLE_ROWS		2	/* 16 bits */
#define VIRTIO_CONSOLE_MAX_NR_PORTS	4	/* 32 bits */
#define VIRTIO_CONSOLE_EMERG_WR		8	/* 32 bits */

#define VIOCON_DEBUG	0

#if VIOCON_DEBUG
#define DPRINTF(x...) printf(x)
#else
#define DPRINTF(x...)
#endif

#define	VIRTIO_CONSOLE_FLAG_BITS					      \
	VIRTIO_COMMON_FLAG_BITS						      \
	"b\x00" "SIZE\0"						      \
	"b\x01" "MULTIPORT\0"						      \
	"b\x02" "EMERG_WRITE\0"

/* The control virtqueues are after the first port */
#define VIOCON_CTRL_RX_IDX 2
#define VIOCON_CTRL_TX_IDX 3
struct virtio_console_control {
	uint32_t id;	/* Port number */

#define	VIRTIO_CONSOLE_DEVICE_READY	0
#define	VIRTIO_CONSOLE_DEVICE_ADD	1
#define	VIRTIO_CONSOLE_DEVICE_REMOVE	2
#define	VIRTIO_CONSOLE_PORT_READY	3
#define	VIRTIO_CONSOLE_CONSOLE_PORT	4
#define	VIRTIO_CONSOLE_RESIZE		5
#define	VIRTIO_CONSOLE_PORT_OPEN	6
#define	VIRTIO_CONSOLE_PORT_NAME	7
	uint16_t event;

	uint16_t value;
} __packed;

struct virtio_console_control_resize {
	/* yes, the order is different than in config space */
	uint16_t rows;
	uint16_t cols;
};

#define	BUFSIZE		128

#define VIOCONDEV(u,p)	makedev(cdevsw_lookup_major(&viocon_cdevsw),	\
    ((u & 0xff) << 4) | (p & 0xf))
#define VIOCONUNIT(x)	(TTUNIT(x) >> 4)
#define VIOCONPORT(x)	(TTUNIT(x) & 0x0f)

struct viocon_port {
	struct viocon_softc	*vp_sc;
	struct virtqueue	*vp_rx;
	struct virtqueue	*vp_tx;
	void			*vp_si;
	struct tty		*vp_tty;
	char 			*vp_name;
	bus_dma_segment_t	 vp_dmaseg;
	bus_dmamap_t		 vp_dmamap;

	unsigned int		 vp_host_open:1;
	unsigned int		 vp_guest_open:1;
	unsigned int		 vp_is_console:1;

	unsigned int		 vp_iflow:1;		/* rx flow control */
	uint16_t		 vp_rows;
	uint16_t		 vp_cols;
	u_char			*vp_rx_buf;
	u_char			*vp_tx_buf;

	struct consdev		 vp_cntab;
	unsigned int		 vp_pollpos;
	unsigned int		 vp_polllen;
	bool			 vp_polling;
};

struct viocon_softc {
	struct device		*sc_dev;
	struct virtio_softc	*sc_virtio;
	struct virtqueue	*sc_vqs;
#define VIOCON_PORT_RX	0
#define VIOCON_PORT_TX	1
#define VIOCON_PORT_NQS	2

	struct virtqueue        *sc_c_vq_rx;
	struct virtqueue        *sc_c_vq_tx;

	unsigned int		 sc_max_ports;
	struct viocon_port	**sc_ports;

	bool			has_multiport;

	bus_dma_segment_t	 sc_dmaseg;
	bus_dmamap_t		 sc_dmamap;
	struct {
		struct virtio_console_control ctrl;
		uint8_t buf[BUFSIZE-8];
	}			*sc_ctrl_rx;
	bus_dmamap_t		 sc_dmamap_tx;
	union {
		struct virtio_console_control sc_tx;
		uint8_t		sc_tx_buf[16];
	};
};

int	viocon_match(struct device *, struct cfdata *, void *);
void	viocon_attach(struct device *, struct device *, void *);
int	viocon_control_rx_intr(struct virtqueue *);
int	viocon_control_tx_intr(struct virtqueue *);
int	viocon_tx_intr(struct virtqueue *);
int	viocon_tx_drain(struct viocon_port *, struct virtqueue *vq);
int	viocon_rx_intr(struct virtqueue *);
void	viocon_rx_soft(void *);
void	viocon_rx_fill(struct viocon_port *);
int	viocon_port_create(struct viocon_softc *, int);
void	vioconstart(struct tty *);
int	vioconhwiflow(struct tty *, int);
int	vioconparam(struct tty *, struct termios *);
int	vioconopen(dev_t, int, int, struct lwp *);
int	vioconclose(dev_t, int, int, struct lwp *);
int	vioconread(dev_t, struct uio *, int);
int	vioconwrite(dev_t, struct uio *, int);
void	vioconstop(struct tty *, int);
int	vioconioctl(dev_t, u_long, void *, int, struct lwp *);
static int viocon_ports_vq_alloc(struct viocon_softc *, int);
struct tty *viocontty(dev_t dev);
static dev_type_poll(vioconpoll);
static void viocon_port_destroy(struct viocon_softc *, int);
static void viocon_control_rx_fill(struct viocon_softc *);
static void viocon_control_dmamap(struct viocon_softc *);
static int viocon_control_send(struct viocon_softc *, uint32_t, uint16_t,
    uint16_t );

static void viocon_console(struct viocon_softc *, int);
static void viocon_cnpollc(dev_t, int);
static int viocon_cngetc(dev_t);
static void viocon_cnputc(dev_t, int);
static void viocon_early_putc(dev_t, int);

CFATTACH_DECL_NEW(viocon, sizeof(struct viocon_softc),
    viocon_match, viocon_attach, /*detach*/NULL, /*activate*/NULL);

const struct cdevsw viocon_cdevsw = {
	.d_open = vioconopen,
	.d_close = vioconclose,
	.d_read = vioconread,
	.d_write = vioconwrite,
	.d_ioctl = vioconioctl,
	.d_stop = vioconstop,
	.d_tty = viocontty,
	.d_poll = vioconpoll,
	.d_mmap = nommap,
	.d_kqfilter = ttykqfilter,
	.d_discard = nodiscard,
	.d_flag = D_TTY,
};

static inline struct viocon_softc *
dev2sc(dev_t dev)
{
	return device_lookup_private(&viocon_cd, VIOCONUNIT(dev));
}

static inline struct viocon_port *
dev2port(dev_t dev)
{
	return dev2sc(dev)->sc_ports[VIOCONPORT(dev)];
}

static inline int
viocon_vqidx2portidx(int vq)
{
	return (vq >= 4) ? (vq - VIOCON_PORT_NQS) / VIOCON_PORT_NQS : 0;
}

static struct {
	bus_space_tag_t		ec_bst;
	bus_space_handle_t	ec_bsh;
	bus_addr_t		ec_reg_offset;
} early_console;

static void
viocon_early_putc(dev_t dev, int c)
{
	bus_space_write_4(early_console.ec_bst, early_console.ec_bsh,
	    early_console.ec_reg_offset, c);
}

int
viocon_earlyinit(bus_space_tag_t bst, bus_space_handle_t bsh)
{
	static struct consdev viocon_early_consdev = {
	    .cn_putc = viocon_early_putc,
	    .cn_getc = NULL,
	    .cn_pollc = NULL,
	    .cn_dev = NODEV,
	    .cn_pri = CN_NORMAL
	};

	if (bus_space_read_4(bst, bsh, VIRTIO_MMIO_MAGIC_VALUE) !=
	    VIRTIO_MMIO_MAGIC) {
		return -1;
	}

	if (bus_space_read_4(bst, bsh, VIRTIO_MMIO_DEVICE_ID) !=
	    VIRTIO_DEVICE_ID_CONSOLE) {
		return -1;
	}

	if (!(bus_space_read_4(bst, bsh, VIRTIO_MMIO_DEVICE_FEATURES) &
	    VIRTIO_CONSOLE_F_EMERG_WRITE)) {
		return -1;
	}

	/*
	 * https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html#x1-3250004
	 * emerg_wr is 32 bits long
	 */
	if (bus_space_subregion(bst, bsh,
	    VIRTIO_MMIO_CONFIG + VIRTIO_CONSOLE_EMERG_WR_OFFSET,
	    sizeof(uint32_t), &early_console.ec_bsh) != 0) {
		return -1;
	}

	early_console.ec_bst = bst;
	early_console.ec_reg_offset = 0; /* already adjusted by subregion */

	cn_tab = &viocon_early_consdev;

	return 0;
}

int
viocon_match(struct device *parent, struct cfdata *match, void *aux)
{
	struct virtio_attach_args *va = aux;
	if (va->sc_childdevid == VIRTIO_DEVICE_ID_CONSOLE)
		return 1;
	return 0;
}

void
viocon_attach(struct device *parent, struct device *self, void *aux)
{
	struct viocon_softc *sc = device_private(self);
	struct virtio_softc *vsc = device_private(parent);
	prop_dictionary_t dict = device_properties(self);
	prop_array_t namearray;
	int maxports = 1;
	size_t nvqs;

	sc->sc_dev = self;
	if (virtio_child(vsc) != NULL) {
		aprint_error(": parent %s already has a child\n",
		    device_xname(parent));
		return;
	}

	virtio_child_attach_start(vsc, self, IPL_TTY,
	    /*req_features*/VIRTIO_CONSOLE_F_SIZE | VIRTIO_CONSOLE_F_MULTIPORT
	    | VIRTIO_CONSOLE_F_EMERG_WRITE | VIRTIO_F_RING_EVENT_IDX,
	    VIRTIO_CONSOLE_FLAG_BITS);

	if (vsc->sc_active_features & VIRTIO_CONSOLE_F_MULTIPORT) {
		maxports = virtio_read_device_config_4(vsc,
		    VIRTIO_CONSOLE_MAX_NR_PORTS);
		aprint_verbose_dev(self,
		    "has multiport feature, max_ports: %u\n", maxports);
		sc->has_multiport = true;
	} else
		sc->has_multiport = false;

	sc->sc_virtio = vsc;
	sc->sc_max_ports = maxports;
	nvqs = VIOCON_PORT_NQS * maxports;
	if (sc->has_multiport)
		nvqs += 2; /* rx and tx control */

	prop_dictionary_set_uint32(dict, "max_ports", sc->sc_max_ports);
	namearray = prop_array_create_with_capacity(sc->sc_max_ports);
	prop_dictionary_set(dict, "port_names", namearray);

	sc->sc_vqs = kmem_zalloc(nvqs * sizeof(sc->sc_vqs[0]),
	    KM_SLEEP);
	sc->sc_ports = kmem_zalloc(maxports * sizeof(sc->sc_ports[0]),
	    KM_SLEEP);

	if (viocon_ports_vq_alloc(sc, maxports) != 0)
		goto err;

	if (!sc->has_multiport) {
		DPRINTF("%s: softc: %p\n", __func__, sc);
		/* virtqueue index handled in port creation */
		if (viocon_port_create(sc, 0) != 0) {
			printf("\n%s: viocon_port_create failed\n", __func__);
			goto err;
		}
		viocon_console(sc, 0);
	}

	if (virtio_child_attach_finish(vsc, sc->sc_vqs, nvqs,
	    /*config_change*/NULL, /*req_flags*/0) != 0)
		goto err;

	if (sc->has_multiport) {
		viocon_control_rx_fill(sc);

		viocon_control_send(sc, VIRTIO_CONSOLE_BAD_ID,
		    VIRTIO_CONSOLE_DEVICE_READY, 1);

		virtio_start_vq_intr(vsc, sc->sc_c_vq_rx);
		virtio_start_vq_intr(vsc, sc->sc_c_vq_tx);
	}

	return;
err:
	kmem_free(sc->sc_vqs, nvqs * sizeof(sc->sc_vqs[0]));
	kmem_free(sc->sc_ports, maxports * sizeof(sc->sc_ports[0]));
	virtio_child_attach_failed(vsc);
}

static void
viocon_control_dmamap(struct viocon_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int nsegs;
	void *kva;

	if (bus_dmamap_create(virtio_dmat(vsc), BUFSIZE, 1, BUFSIZE, 0,
	    BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &sc->sc_dmamap) != 0) {
		goto err;
	}

	if (bus_dmamap_create(virtio_dmat(vsc), 16, 1, 16, 0,
	    BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &sc->sc_dmamap_tx) != 0) {
		goto err;
	}

	if (bus_dmamem_alloc(virtio_dmat(vsc), BUFSIZE,
	    sizeof(struct virtio_console_control), 0, &sc->sc_dmaseg,
	    1, &nsegs, BUS_DMA_NOWAIT) != 0)
		goto err;

	if (bus_dmamem_map(virtio_dmat(vsc), &sc->sc_dmaseg, nsegs,
	    BUFSIZE, &kva, BUS_DMA_NOWAIT) != 0)
		goto err;

	memset(kva, 0, BUFSIZE);
	sc->sc_ctrl_rx = kva;

	if (bus_dmamap_load(virtio_dmat(vsc), sc->sc_dmamap, sc->sc_ctrl_rx,
	    BUFSIZE, NULL, BUS_DMA_NOWAIT|BUS_DMA_READ) != 0)
		goto err;

	return;
err:
	panic("%s failed", __func__);
}

static int
viocon_ports_vq_alloc(struct viocon_softc *sc, int maxports)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int rxidx, txidx, i;
	char name[6];
	struct viocon_port *vp;

	for (i = 0; i < maxports; i++) {
		/* The control virtqueues are after the first port. */
		if (sc->has_multiport && i == 1) {
			/* allocate control virtqueues */
			sc->sc_c_vq_rx = &sc->sc_vqs[VIOCON_CTRL_RX_IDX];
			sc->sc_c_vq_tx = &sc->sc_vqs[VIOCON_CTRL_TX_IDX];

			virtio_init_vq_vqdone(vsc, sc->sc_c_vq_rx, VIOCON_CTRL_RX_IDX,
			    viocon_control_rx_intr);
			virtio_init_vq_vqdone(vsc, sc->sc_c_vq_tx, VIOCON_CTRL_TX_IDX,
			    viocon_control_tx_intr);

			if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOCON_CTRL_RX_IDX],
			    BUFSIZE, 1, "control_rx"))
				goto err;
			if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOCON_CTRL_TX_IDX],
			    16, 1, "control_tx"))
				goto err;

			viocon_control_dmamap(sc);
		}

		vp = kmem_zalloc(sizeof(*vp), KM_SLEEP);

		sc->sc_ports[i] = vp;
		vp->vp_sc = sc;
		DPRINTF("%s: vp: %p\n", __func__, vp);

		rxidx = (i * VIOCON_PORT_NQS) + VIOCON_PORT_RX;
		txidx = (i * VIOCON_PORT_NQS) + VIOCON_PORT_TX;

		if (i > 0) {
			rxidx += VIOCON_CTRL_RX_IDX;
			txidx += VIOCON_CTRL_RX_IDX;
		}

		snprintf(name, sizeof(name), "p%drx", i);
		virtio_init_vq_vqdone(vsc, &sc->sc_vqs[rxidx], rxidx,
		    viocon_rx_intr);
		if (virtio_alloc_vq(vsc, &sc->sc_vqs[rxidx], BUFSIZE, 1,
		    name) != 0) {
			printf("\nCan't alloc %s virtqueue\n", name);
			goto err;
		}
		vp->vp_rx = &sc->sc_vqs[rxidx];
		vp->vp_si = softint_establish(SOFTINT_SERIAL, viocon_rx_soft, vp);
		DPRINTF("%s: rx: %p\n", __func__, vp->vp_rx);

		snprintf(name, sizeof(name), "p%dtx", i);
		virtio_init_vq_vqdone(vsc, &sc->sc_vqs[txidx], txidx,
		    viocon_tx_intr);
		if (virtio_alloc_vq(vsc, &sc->sc_vqs[txidx], BUFSIZE, 1,
		    name) != 0) {
			printf("\nCan't alloc %s virtqueue\n", name);
			goto err;
		}
		vp->vp_tx = &sc->sc_vqs[txidx];
		DPRINTF("%s: tx: %p\n", __func__, vp->vp_tx);
	}

	return 0;
err:
	panic("%s failed", __func__);
	return -1;
}

static void
viocon_control_rx_fill(struct viocon_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = sc->sc_c_vq_rx;
	int r, slot, ndone = 0;

	while ((r = virtio_enqueue_prep(vsc, vq, &slot)) == 0) {
		if (virtio_enqueue_reserve(vsc, vq, slot, 1) != 0)
			break;
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_dmamap, slot * BUFSIZE,
		    BUFSIZE, BUS_DMASYNC_PREREAD);
		virtio_enqueue_p(vsc, vq, slot, sc->sc_dmamap, slot * BUFSIZE,
		    BUFSIZE, 0);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		ndone++;
	}
	KASSERT(r == 0 || r == EAGAIN);
	if (ndone > 0)
		virtio_notify(vsc, vq);
}

int
viocon_control_tx_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct viocon_softc *sc = device_private(virtio_child(vsc));
	int ndone = 0, slot, len;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_dmamap_tx,
		    0, 8, BUS_DMASYNC_POSTWRITE);
		virtio_dequeue_commit(vsc, vq, slot);
		ndone++;
	}
	return ndone;
}

int
viocon_control_rx_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct viocon_softc *sc = device_private(virtio_child(vsc));
	struct viocon_port *vp;
	prop_dictionary_t properties = device_properties(sc->sc_dev);
	prop_array_t namearray;
	int ndone = 0, slot, len, namelen;
	uint32_t id;
	uint16_t event, value __unused;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_dmamap,
		    slot * BUFSIZE, BUFSIZE, BUS_DMASYNC_POSTREAD);
		id = virtio_rw32(vsc, sc->sc_ctrl_rx[slot].ctrl.id);
		event = virtio_rw16(vsc, sc->sc_ctrl_rx[slot].ctrl.event);
		value = virtio_rw16(vsc, sc->sc_ctrl_rx[slot].ctrl.value);

		if (id != VIRTIO_CONSOLE_BAD_ID && id >= sc->sc_max_ports) {
			DPRINTF("%s: invalid port id %d\n", __func__, id);
			return 0;
		}

		switch (event) {
		/* https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html#x1-3290006 */
		case VIRTIO_CONSOLE_DEVICE_ADD:
			aprint_verbose_dev(sc->sc_dev, "adding port %u\n", id);
			if (viocon_port_create(sc, id) == 0)
				viocon_control_send(sc, id,
				    VIRTIO_CONSOLE_PORT_READY, 1);
			break;
		case VIRTIO_CONSOLE_CONSOLE_PORT:
			aprint_verbose_dev(sc->sc_dev, "%u is a console port\n", id);
			sc->sc_ports[id]->vp_is_console = 1;
			viocon_console(sc, id);
			break;
		case VIRTIO_CONSOLE_PORT_OPEN:
			aprint_verbose_dev(sc->sc_dev, "host opened port %u\n", id);
			sc->sc_ports[id]->vp_host_open = 1;
			break;
		case VIRTIO_CONSOLE_PORT_NAME:
			namelen = len - sizeof(sc->sc_ctrl_rx[slot].ctrl);
			if (namelen > 0) {
				vp = sc->sc_ports[id];
				if (vp->vp_name != NULL)
					kmem_free(vp->vp_name, strlen(vp->vp_name) + 1);
				vp->vp_name = kmem_zalloc(namelen + 1, KM_SLEEP);
				memcpy(vp->vp_name, sc->sc_ctrl_rx[slot].buf, namelen);
				vp->vp_name[namelen] = '\0';
				aprint_normal_dev(sc->sc_dev, "port %u name: %s\n",
				    id, vp->vp_name);
				namearray = prop_dictionary_get(properties, "port_names");
				prop_array_set_string_nocopy(namearray, id, vp->vp_name);
			}
			break;
		case VIRTIO_CONSOLE_DEVICE_REMOVE:
			if (sc->sc_ports[id] != NULL)
				viocon_port_destroy(sc, id);
			break;
		default:
			aprint_verbose("unsupported event: %u\n", event);
		}

		virtio_dequeue_commit(vsc, vq, slot);
		ndone++;
	}

	viocon_control_rx_fill(sc);

	return ndone;
}

static int
viocon_control_send(struct viocon_softc *sc, uint32_t id, uint16_t event,
    uint16_t value)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int slot;

	sc->sc_tx.id = virtio_rw32(vsc, id);
	sc->sc_tx.event = virtio_rw16(vsc, event);
	sc->sc_tx.value = virtio_rw16(vsc, value);

	if (bus_dmamap_load(virtio_dmat(vsc), sc->sc_dmamap_tx, sc->sc_tx_buf,
	    8, NULL, BUS_DMA_NOWAIT|BUS_DMA_WRITE) != 0)
		goto err;
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_dmamap_tx,
	    0, 8, BUS_DMASYNC_PREWRITE);

	if (virtio_enqueue_prep(vsc, sc->sc_c_vq_tx, &slot)) {
		goto err;
	}
	if (virtio_enqueue_reserve(vsc, sc->sc_c_vq_tx, slot, 1)) {
		goto err;
	}

	virtio_enqueue(vsc, sc->sc_c_vq_tx, slot, sc->sc_dmamap_tx, true);
	virtio_enqueue_commit(vsc, sc->sc_c_vq_tx, slot, 1);

	return 0;
err:
	return -1;
}

int
viocon_port_create(struct viocon_softc *sc, int portidx)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct viocon_port *vp = sc->sc_ports[portidx];
	struct tty *tp;
	prop_dictionary_t properties = device_properties(sc->sc_dev);
	prop_array_t namearray;
	int allocsize, nsegs;
	void *kva;

	allocsize = (vp->vp_rx->vq_num + vp->vp_tx->vq_num) * BUFSIZE;

	if (bus_dmamap_create(virtio_dmat(vsc), allocsize, 1, allocsize, 0,
	    BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &vp->vp_dmamap) != 0)
		goto err;
	if (bus_dmamem_alloc(virtio_dmat(vsc), allocsize, 8, 0, &vp->vp_dmaseg,
	    1, &nsegs, BUS_DMA_NOWAIT) != 0)
		goto err;
	if (bus_dmamem_map(virtio_dmat(vsc), &vp->vp_dmaseg, nsegs,
	    allocsize, &kva, BUS_DMA_NOWAIT) != 0)
		goto err;
	memset(kva, 0, allocsize);
	if (bus_dmamap_load(virtio_dmat(vsc), vp->vp_dmamap, kva,
	    allocsize, NULL, BUS_DMA_NOWAIT) != 0)
		goto err;
	vp->vp_rx_buf = (unsigned char *)kva;
	/*
	 * XXX use only a small circular tx buffer instead of many BUFSIZE buffers?
	 */
	vp->vp_tx_buf = vp->vp_rx_buf + vp->vp_rx->vq_num * BUFSIZE;

	if (virtio_features(vsc) & VIRTIO_CONSOLE_F_SIZE) {
		vp->vp_cols = virtio_read_device_config_2(vsc,
		    VIRTIO_CONSOLE_COLS);
		vp->vp_rows = virtio_read_device_config_2(vsc,
		    VIRTIO_CONSOLE_ROWS);
	}

	tp = ttymalloc(1000000);
	tp->t_oproc = vioconstart;
	tp->t_param = vioconparam;
	tp->t_hwiflow = vioconhwiflow;
	tp->t_dev = VIOCONDEV(device_unit(sc->sc_dev), portidx);
	vp->vp_tty = tp;
	DPRINTF("%s: tty: %p\n", __func__, tp);
	vp->vp_name = kmem_zalloc(7, KM_SLEEP);
	snprintf(vp->vp_name, 7, "port%02d", portidx);

	tty_attach(tp);

	virtio_start_vq_intr(vsc, vp->vp_rx);
	virtio_start_vq_intr(vsc, vp->vp_tx);

	viocon_rx_fill(sc->sc_ports[portidx]);

	namearray = prop_dictionary_get(properties, "port_names");
	prop_array_set_string_nocopy(namearray, portidx, vp->vp_name);

	return 0;
err:
	panic("%s failed", __func__);
	return -1;
}

static void
viocon_port_destroy(struct viocon_softc *sc, int portidx)
{
	struct viocon_port *vp = sc->sc_ports[portidx];

	if (vp == NULL)
		return;

	viocon_control_send(sc, portidx, VIRTIO_CONSOLE_PORT_READY, 0);

	if (vp->vp_name != NULL)
		kmem_free(vp->vp_name, strlen(vp->vp_name) + 1);
	tty_free(vp->vp_tty);
	vp = NULL;
}

static void
viocon_console(struct viocon_softc *sc, int portidx)
{
	struct viocon_port *vp = sc->sc_ports[portidx];

	if (cn_tab != NULL && cn_tab->cn_dev != NODEV)
		return;

	sc->sc_ports[portidx]->vp_cntab = (struct consdev) {
		.cn_pollc = viocon_cnpollc,
		.cn_getc = viocon_cngetc,
		.cn_putc = viocon_cnputc,
		.cn_dev = vp->vp_tty->t_dev,
		.cn_pri = CN_REMOTE,
	};
	aprint_normal_dev(sc->sc_dev, "console\n");
	cn_tab = &sc->sc_ports[portidx]->vp_cntab;
}

int
viocon_tx_drain(struct viocon_port *vp, struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	int ndone = 0, len, slot;

	splassert(IPL_TTY);
	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		bus_dmamap_sync(virtio_dmat(vsc), vp->vp_dmamap,
		    vp->vp_tx_buf - vp->vp_rx_buf + slot * BUFSIZE, BUFSIZE,
		    BUS_DMASYNC_POSTWRITE);
		virtio_dequeue_commit(vsc, vq, slot);
		ndone++;
	}
	return ndone;
}

int
viocon_tx_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct viocon_softc *sc = device_private(virtio_child(vsc));
	int portidx = viocon_vqidx2portidx(vq->vq_index);
	struct viocon_port *vp = sc->sc_ports[portidx];
	struct tty *tp = vp->vp_tty;
	int ndone = 0;

	splassert(IPL_TTY);
	ndone = viocon_tx_drain(vp, vq);
	ttylock(tp);
	if (ndone && ISSET(tp->t_state, TS_BUSY)) {
		CLR(tp->t_state, TS_BUSY);
		(*tp->t_linesw->l_start)(tp);
	}
	ttyunlock(tp);

	return 1;
}

void
viocon_rx_fill(struct viocon_port *vp)
{
	struct virtqueue *vq = vp->vp_rx;
	struct virtio_softc *vsc = vp->vp_sc->sc_virtio;
	int r, slot, ndone = 0;

	while ((r = virtio_enqueue_prep(vsc, vq, &slot)) == 0) {
		if (virtio_enqueue_reserve(vsc, vq, slot, 1) != 0)
			break;
		bus_dmamap_sync(virtio_dmat(vsc), vp->vp_dmamap, slot * BUFSIZE,
		    BUFSIZE, BUS_DMASYNC_PREREAD);
		virtio_enqueue_p(vsc, vq, slot, vp->vp_dmamap, slot * BUFSIZE,
		    BUFSIZE, 0);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		ndone++;
	}
	KASSERTMSG(r == 0 || r == EAGAIN, "r=%d", r);
	if (ndone > 0)
		virtio_notify(vsc, vq);
}

int
viocon_rx_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct viocon_softc *sc = device_private(virtio_child(vsc));
	int portidx = viocon_vqidx2portidx(vq->vq_index);
	struct viocon_port *vp = sc->sc_ports[portidx];

	softint_schedule(vp->vp_si);
	return 1;
}

void
viocon_rx_soft(void *arg)
{
	struct viocon_port *vp = arg;
	struct virtqueue *vq = vp->vp_rx;
	struct virtio_softc *vsc = vq->vq_owner;
	struct tty *tp = vp->vp_tty;
	int slot, len, i;
	u_char *p;

	while (!vp->vp_iflow && virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		bus_dmamap_sync(virtio_dmat(vsc), vp->vp_dmamap,
		    slot * BUFSIZE, BUFSIZE, BUS_DMASYNC_POSTREAD);
		p = vp->vp_rx_buf + slot * BUFSIZE;
		for (i = 0; i < len; i++)
			(*tp->t_linesw->l_rint)(*p++, tp);
		virtio_dequeue_commit(vsc, vq, slot);
	}

	if (virtio_features(vsc) & VIRTIO_F_RING_EVENT_IDX)
		virtio_start_vq_intr(vsc, vp->vp_rx);

	viocon_rx_fill(vp);

	return;
}

void
vioconstart(struct tty *tp)
{
	struct viocon_softc *sc = dev2sc(tp->t_dev);
	struct virtio_softc *vsc;
	struct viocon_port *vp = dev2port(tp->t_dev);
	struct virtqueue *vq;
	u_char *buf;
	int s, cnt, slot, ret, ndone;

	vsc = sc->sc_virtio;
	vq = vp->vp_tx;

	s = spltty();

	ndone = viocon_tx_drain(vp, vq);
	if (ISSET(tp->t_state, TS_BUSY)) {
		if (ndone > 0)
			CLR(tp->t_state, TS_BUSY);
		else
			goto out;
	}
	if (ISSET(tp->t_state, TS_TIMEOUT | TS_TTSTOP))
		goto out;

	if (!ttypull(tp))
		goto out;

	if (tp->t_outq.c_cc == 0)
		goto out;
	ndone = 0;

	if (virtio_features(vsc) & VIRTIO_F_RING_EVENT_IDX)
		virtio_start_vq_intr(vsc, vp->vp_tx);

	while (tp->t_outq.c_cc > 0) {
		ret = virtio_enqueue_prep(vsc, vq, &slot);
		if (ret == EAGAIN) {
			SET(tp->t_state, TS_BUSY);
			break;
		}
		KASSERTMSG(ret == 0, "ret=%d", ret);
		ret = virtio_enqueue_reserve(vsc, vq, slot, 1);
		KASSERTMSG(ret == 0, "ret=%d", ret);
		buf = vp->vp_tx_buf + slot * BUFSIZE;
		cnt = q_to_b(&tp->t_outq, buf, BUFSIZE);
		bus_dmamap_sync(virtio_dmat(vsc), vp->vp_dmamap,
		    vp->vp_tx_buf - vp->vp_rx_buf + slot * BUFSIZE, cnt,
		    BUS_DMASYNC_PREWRITE);
		virtio_enqueue_p(vsc, vq, slot, vp->vp_dmamap,
		    vp->vp_tx_buf - vp->vp_rx_buf + slot * BUFSIZE, cnt, 1);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		ndone++;
		ttypull(tp);
	}
	if (ndone > 0)
		virtio_notify(vsc, vq);
	ttwakeupwr(tp);
out:
	splx(s);
}

int
vioconhwiflow(struct tty *tp, int stop)
{
	struct viocon_port *vp = dev2port(tp->t_dev);
	int s;

	s = spltty();
	vp->vp_iflow = stop;
	if (stop) {
		virtio_stop_vq_intr(vp->vp_sc->sc_virtio, vp->vp_rx);
	} else {
		virtio_start_vq_intr(vp->vp_sc->sc_virtio, vp->vp_rx);
		softint_schedule(vp->vp_si);
	}
	splx(s);
	return 1;
}

int
vioconparam(struct tty *tp, struct termios *t)
{
	tp->t_ispeed = t->c_ispeed;
	tp->t_ospeed = t->c_ospeed;
	tp->t_cflag = t->c_cflag;

	vioconstart(tp);
	return 0;
}

int
vioconopen(dev_t dev, int flag, int mode, struct lwp *l)
{
	int unit = VIOCONUNIT(dev);
	int port = VIOCONPORT(dev);
	struct viocon_softc *sc;
	struct viocon_port *vp;
	struct tty *tp;
	int s, error;

	sc = device_lookup_private(&viocon_cd, unit);
	if (sc == NULL)
		return (ENXIO);
	if (!device_is_active(sc->sc_dev))
		return (ENXIO);

	s = spltty();
	if (port >= sc->sc_max_ports) {
		splx(s);
		return (ENXIO);
	}
	vp = sc->sc_ports[port];
	tp = vp->vp_tty;
	vp->vp_guest_open = 1;

	splx(s);

	if (kauth_authorize_device_tty(l->l_cred, KAUTH_DEVICE_TTY_OPEN, tp))
		return (EBUSY);

	s = spltty();
	if (!ISSET(tp->t_state, TS_ISOPEN) && tp->t_wopen == 0) {
		ttychars(tp);
		tp->t_ispeed = 1000000;
		tp->t_ospeed = 1000000;
		tp->t_cflag = TTYDEF_CFLAG|CLOCAL|CRTSCTS;
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		if (vp->vp_cols != 0) {
			tp->t_winsize.ws_col = vp->vp_cols;
			tp->t_winsize.ws_row = vp->vp_rows;
		}

		vioconparam(tp, &tp->t_termios);
		ttsetwater(tp);
	}
	splx(s);

	error = ttyopen(tp, TTDIALOUT(dev), ISSET(flag, O_NONBLOCK));
	if (error)
		goto bad;

	error = (*tp->t_linesw->l_open)(dev, tp);
	if (error)
		goto bad;

	if ((virtio_features(sc->sc_virtio) & VIRTIO_CONSOLE_F_MULTIPORT) != 0) {
		viocon_control_send(sc, port, VIRTIO_CONSOLE_PORT_OPEN, 1);
	}
bad:
	return error;
}

int
vioconclose(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct viocon_port *vp = dev2port(dev);
	struct tty *tp = vp->vp_tty;
	int s;

	if (!ISSET(tp->t_state, TS_ISOPEN))
		return 0;

	(*tp->t_linesw->l_close)(tp, flag);
	s = spltty();
	vp->vp_guest_open = 0;

	CLR(tp->t_state, TS_BUSY | TS_FLUSH);
	ttyclose(tp);
	splx(s);

	if ((virtio_features(vp->vp_sc->sc_virtio) &
	    VIRTIO_CONSOLE_F_MULTIPORT) != 0) {
		viocon_control_send(vp->vp_sc, VIOCONPORT(dev),
		    VIRTIO_CONSOLE_PORT_OPEN, 0);
	}

	return 0;
}

int
vioconread(dev_t dev, struct uio *uio, int flag)
{
	struct viocon_port *vp = dev2port(dev);
	struct tty *tp = vp->vp_tty;

	return (*tp->t_linesw->l_read)(tp, uio, flag);
}

int
vioconwrite(dev_t dev, struct uio *uio, int flag)
{
	struct viocon_port *vp = dev2port(dev);
	struct tty *tp = vp->vp_tty;

	return (*tp->t_linesw->l_write)(tp, uio, flag);
}

struct tty *
viocontty(dev_t dev)
{
	struct viocon_port *vp = dev2port(dev);

	return vp->vp_tty;
}

void
vioconstop(struct tty *tp, int flag)
{
	int s;

	s = spltty();
	if (ISSET(tp->t_state, TS_BUSY))
		if (!ISSET(tp->t_state, TS_TTSTOP))
			SET(tp->t_state, TS_FLUSH);
	splx(s);
}

int
vioconioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct viocon_port *vp = dev2port(dev);
	struct tty *tp;
	int error1, error2;

	tp = vp->vp_tty;

	error1 = (*tp->t_linesw->l_ioctl)(tp, cmd, data, flag, l);
	if (error1 >= 0)
		return error1;
	error2 = ttioctl(tp, cmd, data, flag, l);
	if (error2 >= 0)
		return error2;
	return ENOTTY;
}

static void
viocon_cnpollc(dev_t dev, int on)
{
	struct viocon_port *vp = dev2port(dev);
	int s;

	KASSERT((bool)on != vp->vp_polling);

	s = spltty();
	vp->vp_polling = on;
	vioconhwiflow(vp->vp_tty, on);
	splx(s);
}

static int
viocon_cngetc(dev_t dev)
{
	struct viocon_softc *sc = dev2sc(dev);
	struct viocon_port *vp = dev2port(dev);
	struct virtqueue *vq = vp->vp_rx;
	struct virtio_softc *vsc = sc->sc_virtio;
	int slot, len;

	KASSERT(vp->vp_polling);
	while (vp->vp_polllen == 0) {
		if (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
			KASSERTMSG(slot >= 0, "slot=%d", slot);
			KASSERTMSG(slot < vq->vq_num, "slot=%d", slot);
			KASSERTMSG(len > 0, "len=%d", len);
			KASSERTMSG(len <= BUFSIZE, "len=%d", len);
			bus_dmamap_sync(virtio_dmat(vsc), vp->vp_dmamap,
			    slot * BUFSIZE, BUFSIZE, BUS_DMASYNC_POSTREAD);
			vp->vp_polllen = len;
			vp->vp_pollpos = slot * BUFSIZE;
		}
	}
	KASSERT(vp->vp_pollpos <= vq->vq_num * BUFSIZE);
	vp->vp_polllen--;
	return vp->vp_rx_buf[vp->vp_pollpos++];
}

static void
viocon_cnputc(dev_t dev, int c)
{
	struct viocon_softc *sc = dev2sc(dev);
	struct viocon_port *vp = dev2port(dev);
	struct virtqueue *vq = vp->vp_tx;
	struct virtio_softc *vsc = sc->sc_virtio;
	int slot;
	int s, error;

	s = spltty();
	KERNEL_LOCK(1, NULL);
	(void)viocon_tx_drain(vp, vq);
	error = virtio_enqueue_prep(vsc, vq, &slot);
	if (error == 0) {
		error = virtio_enqueue_reserve(vsc, vq, slot, 1);
		KASSERTMSG(error == 0, "error=%d", error);
		vp->vp_tx_buf[slot * BUFSIZE] = c;
		bus_dmamap_sync(virtio_dmat(vsc), vp->vp_dmamap,
		    vp->vp_tx_buf - vp->vp_rx_buf + slot * BUFSIZE, 1,
		    BUS_DMASYNC_PREWRITE);
		virtio_enqueue_p(vsc, vq, slot, vp->vp_dmamap,
		    vp->vp_tx_buf - vp->vp_rx_buf + slot * BUFSIZE, 1, 1);
		virtio_enqueue_commit(vsc, vq, slot, 1);
	}
	KERNEL_UNLOCK_ONE(NULL);
	splx(s);
}

static int
vioconpoll(dev_t dev, int events, struct lwp *l)
{
	struct viocon_port *vp = dev2port(dev);
	struct tty *tp = vp->vp_tty;
	int revents;

	revents = tp->t_linesw->l_poll(tp, events, l);

	return revents;
}
