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
 * virtio-input: supports QEMU virtio-keyboard-device and
 * virtio-mouse-device over any virtio transport.  Events are Linux
 * evdev records (struct virtio_input_event), which map 1:1 onto the
 * evcode space wskbd(4) uses; the mouse side translates events into
 * wsmouse_input() calls at SYN_REPORT time.
 *
 * Queues: 0 = eventq (device -> driver), 1 = statusq (driver ->
 * device, used for LED updates only).
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <dev/pci/virtioreg.h>
#include <dev/pci/virtiovar.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsmousevar.h>

#ifndef VIRTIO_DEVICE_ID_INPUT
#define VIRTIO_DEVICE_ID_INPUT		18
#endif

/*
 * virtio-input config space (offsets from start of device config).
 */
#define VIOIN_CFG_SELECT		0
#define VIOIN_CFG_SUBSEL		1
#define VIOIN_CFG_SIZE			2
#define VIOIN_CFG_DATA			8

#define VIOIN_CFG_ID_DEVICES		0x03
#define VIOIN_CFG_EV_BITS		0x11

/*
 * evdev constants.
 */
#define EV_SYN			0x00
#define EV_KEY			0x01
#define EV_REL			0x02
#define EV_ABS			0x03
#define EV_LED			0x11

#define SYN_REPORT		0

#define REL_X			0x00
#define REL_Y			0x01
#define REL_HWHEEL		0x06
#define REL_WHEEL		0x08

#define ABS_X			0x00
#define ABS_Y			0x01

#define BTN_LEFT		0x110
#define BTN_RIGHT		0x111
#define BTN_MIDDLE		0x112
#define BTN_SIDE		0x113
#define BTN_EXTRA		0x114

#define LED_NUM			0x00
#define LED_CAPS		0x01
#define LED_SCROLL		0x02

struct vioinput_event {
	uint16_t		type;
	uint16_t		code;
	uint32_t		value;
};

/*
 * The DMA event buffer holds one slot per virtqueue descriptor so that
 * (slot * VIOIN_EV_SIZE) is always a valid offset into it, as required
 * by the KASSERTs in virtio_enqueue_p(); the buffer size is therefore
 * derived from the queue length advertised by the device (QEMU uses
 * 1024 descriptors for virtio-input) and stored in sc_ev_bufsz.
 */
#define VIOIN_EV_SIZE			((int)sizeof(struct vioinput_event))

#define	virtio_notify(vsc, vq)	virtio_enqueue_commit(vsc, vq, -1, true)

struct vioinput_softc {
	device_t		sc_dev;
	struct virtio_softc	*sc_virtio;

	struct virtqueue	sc_vqs[2];
#define VIOIN_EVENTQ		0
#define VIOIN_STATUSQ		1

	void			*sc_ev_si;

	bus_dma_segment_t	sc_ev_seg;
	bus_dmamap_t		sc_ev_map;
	u_char			*sc_ev_buf;
	bus_size_t		sc_ev_bufsz;

	bus_dma_segment_t	sc_st_seg;
	bus_dmamap_t		sc_st_map;
	struct vioinput_event	*sc_st_buf;

	device_t		sc_wskbddev;
	device_t		sc_wsmousedev;

	int			sc_mouse;	/* report as wsmouse */
	int			sc_buttons;
	int			sc_dx, sc_dy, sc_dz, sc_dw;
	int			sc_ax, sc_ay;
};

/*
 * Keyboard map: evdev evcodes to US keysyms, in the self-describing
 * stream format consumed by wskbd_get_mapentry() --
 * KS_KEYCODE(code) followed by one to four keysyms (plain, shifted[,
 * altgr, altgr-shifted]).
 */
#define VIOIN_LAYOUT		((kbd_t)0x00000500)	/* private encoding */

#define KC(c)			KS_KEYCODE(c)

static const keysym_t vioinput_keymap[] = {
	KC(1),		KS_Escape,
	KC(2),		KS_1,		KS_exclam,
	KC(3),		KS_2,		KS_at,
	KC(4),		KS_3,		KS_numbersign,
	KC(5),		KS_4,		KS_dollar,
	KC(6),		KS_5,		KS_percent,
	KC(7),		KS_6,		KS_asciicircum,
	KC(8),		KS_7,		KS_ampersand,
	KC(9),		KS_8,		KS_asterisk,
	KC(10),		KS_9,		KS_parenleft,
	KC(11),		KS_0,		KS_parenright,
	KC(12),		KS_minus,	KS_underscore,
	KC(13),		KS_equal,	KS_plus,
	KC(14),		KS_BackSpace,
	KC(15),		KS_Tab,
	KC(16),		KS_q,
	KC(17),		KS_w,
	KC(18),		KS_e,
	KC(19),		KS_r,
	KC(20),		KS_t,
	KC(21),		KS_y,
	KC(22),		KS_u,
	KC(23),		KS_i,
	KC(24),		KS_o,
	KC(25),		KS_p,
	KC(26),		KS_bracketleft,	KS_braceleft,
	KC(27),		KS_bracketright,	KS_braceright,
	KC(28),		KS_Return,
	KC(29),		KS_Control_L,
	KC(30),		KS_a,
	KC(31),		KS_s,
	KC(32),		KS_d,
	KC(33),		KS_f,
	KC(34),		KS_g,
	KC(35),		KS_h,
	KC(36),		KS_j,
	KC(37),		KS_k,
	KC(38),		KS_l,
	KC(39),		KS_semicolon,	KS_colon,
	KC(40),		KS_apostrophe,	KS_quotedbl,
	KC(41),		KS_grave,	KS_asciitilde,
	KC(42),		KS_Shift_L,
	KC(43),		KS_backslash,	KS_bar,
	KC(44),		KS_z,
	KC(45),		KS_x,
	KC(46),		KS_c,
	KC(47),		KS_v,
	KC(48),		KS_b,
	KC(49),		KS_n,
	KC(50),		KS_m,
	KC(51),		KS_comma,	KS_less,
	KC(52),		KS_period,	KS_greater,
	KC(53),		KS_slash,	KS_question,
	KC(54),		KS_Shift_R,
	KC(55),		KS_KP_Multiply,
	KC(56),		KS_Alt_L,
	KC(57),		KS_space,
	KC(58),		KS_Caps_Lock,
	KC(59),		KS_F1,
	KC(60),		KS_F2,
	KC(61),		KS_F3,
	KC(62),		KS_F4,
	KC(63),		KS_F5,
	KC(64),		KS_F6,
	KC(65),		KS_F7,
	KC(66),		KS_F8,
	KC(67),		KS_F9,
	KC(68),		KS_F10,
	KC(69),		KS_voidSymbol,		/* ScrollLock */
	KC(70),		KS_Pause,
	KC(71),		KS_KP_7,
	KC(72),		KS_KP_8,
	KC(73),		KS_KP_9,
	KC(74),		KS_KP_Subtract,
	KC(75),		KS_KP_4,
	KC(76),		KS_KP_5,
	KC(77),		KS_KP_6,
	KC(78),		KS_KP_Add,
	KC(79),		KS_KP_1,
	KC(80),		KS_KP_2,
	KC(81),		KS_KP_3,
	KC(82),		KS_KP_0,
	KC(83),		KS_KP_Decimal,
	KC(87),		KS_F11,
	KC(88),		KS_F12,
	KC(96),		KS_KP_Enter,
	KC(97),		KS_KP_Divide,
	KC(98),		KS_Execute,		/* SysRq */
	KC(99),		KS_Control_R,
	KC(100),	KS_KP_Multiply,
	KC(101),	KS_Alt_R,
	KC(102),	KS_Home,
	KC(103),	KS_Up,
	KC(104),	KS_Prior,
	KC(105),	KS_Left,
	KC(106),	KS_Right,
	KC(107),	KS_End,
	KC(108),	KS_Down,
	KC(109),	KS_Next,
	KC(110),	KS_Insert,
	KC(111),	KS_Delete,
	KC(125),	KS_Meta_L,
	KC(126),	KS_Meta_L,		/* right: same class */
	KC(127),	KS_Multi_key,		/* Compose/Menu */
};

static void	vioinput_set_leds(void *, int);

static const struct wscons_keydesc vioinput_keydesc[] = {
	{
		.name		= VIOIN_LAYOUT,
		.base		= 0,
		.map_size	= __arraycount(vioinput_keymap),
		.map		= vioinput_keymap,
	},
	{ 0, 0, 0, NULL },
};

static const struct wskbd_mapdata vioinput_keymapdata = {
	.keydesc	= vioinput_keydesc,
	.layout		= VIOIN_LAYOUT,
};

static int	vioinput_match(device_t, cfdata_t, void *);
static void	vioinput_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(vioinput, sizeof(struct vioinput_softc),
    vioinput_match, vioinput_attach, NULL, NULL);

static void	vioinput_event_fill(struct vioinput_softc *);
static int	vioinput_status_purge(struct vioinput_softc *);
static int	vioinput_event_intr(struct virtqueue *);
static void	vioinput_event_soft(void *);

/*
 * Synchronous device configuration read: QEMU updates the selected
 * config record when the select/subsel bytes are written, so a write
 * followed by byte reads is consistent.
 */
static int
vioinput_cfg_read(struct vioinput_softc *sc, uint8_t select, uint8_t subsel,
    uint8_t *buf, int len)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int i;

	virtio_write_device_config_1(vsc, VIOIN_CFG_SELECT, select);
	virtio_write_device_config_1(vsc, VIOIN_CFG_SUBSEL, subsel);

	if (buf != NULL)
		for (i = 0; i < len; i++)
			buf[i] = virtio_read_device_config_1(vsc,
			    VIOIN_CFG_DATA + i);

	return virtio_read_device_config_1(vsc, VIOIN_CFG_SIZE);
}

static int
vioinput_has_evbit(struct vioinput_softc *sc, uint8_t ev)
{
	uint8_t bits[8];
	int i;

	vioinput_cfg_read(sc, VIOIN_CFG_EV_BITS, ev, bits, sizeof(bits));
	for (i = 0; i < (int)sizeof(bits); i++)
		if (bits[i] != 0)
			return 1;
	return 0;
}

static int
vioinput_match(device_t parent, cfdata_t match, void *aux)
{
	struct virtio_attach_args *va = aux;

	if (va->sc_childdevid == VIRTIO_DEVICE_ID_INPUT)
		return 1;

	return 0;
}

/*
 * wskbd glue.
 */
static int
vioinput_kbenable(void *cookie, int on)
{
	return 0;
}

/*
 * wskbd and wsmouse call accessops->ioctl unguarded for unknown ioctls.
 */
static int
vioinput_ioctl_stub(void *cookie, u_long cmd, void *data, int flag,
    struct lwp *l)
{

	return EPASSTHROUGH;
}

static const struct wskbd_accessops vioinput_wskbdops = {
	.enable		= vioinput_kbenable,
	.set_leds	= vioinput_set_leds,
	.ioctl		= vioinput_ioctl_stub,
};

/*
 * wsmouse glue.
 */
static int
vioinput_msenable(void *cookie)
{
	return 0;
}

static void
vioinput_msdisable(void *cookie)
{
}

static const struct wsmouse_accessops vioinput_wsmouseops = {
	.enable		= vioinput_msenable,
	.ioctl		= vioinput_ioctl_stub,
	.disable	= vioinput_msdisable,
};

static void
vioinput_mem_init(struct vioinput_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int error, nsegs;

	error = bus_dmamem_alloc(virtio_dmat(vsc), sc->sc_ev_bufsz,
	    VIOIN_EV_SIZE, 0, &sc->sc_ev_seg, 1, &nsegs, BUS_DMA_WAITOK);
	if (error)
		panic("vioinput: can't alloc event dma");
	error = bus_dmamem_map(virtio_dmat(vsc), &sc->sc_ev_seg, nsegs,
	    sc->sc_ev_bufsz, (void **)&sc->sc_ev_buf, BUS_DMA_WAITOK);
	if (error)
		panic("vioinput: can't map event dma");
	error = bus_dmamap_create(virtio_dmat(vsc), sc->sc_ev_bufsz, 1,
	    sc->sc_ev_bufsz, 0, BUS_DMA_NOWAIT, &sc->sc_ev_map);
	if (error)
		panic("vioinput: can't create event dma map");
	error = bus_dmamap_load(virtio_dmat(vsc), sc->sc_ev_map,
	    sc->sc_ev_buf, sc->sc_ev_bufsz, NULL, BUS_DMA_WAITOK);
	if (error)
		panic("vioinput: can't load event dma map");

	error = bus_dmamem_alloc(virtio_dmat(vsc), VIOIN_EV_SIZE,
	    VIOIN_EV_SIZE, 0, &sc->sc_st_seg, 1, &nsegs, BUS_DMA_WAITOK);
	if (error)
		panic("vioinput: can't alloc status dma");
	error = bus_dmamem_map(virtio_dmat(vsc), &sc->sc_st_seg, nsegs,
	    VIOIN_EV_SIZE, (void **)&sc->sc_st_buf, BUS_DMA_WAITOK);
	if (error)
		panic("vioinput: can't map status dma");
	error = bus_dmamap_create(virtio_dmat(vsc), VIOIN_EV_SIZE, 1,
	    VIOIN_EV_SIZE, 0, BUS_DMA_NOWAIT, &sc->sc_st_map);
	if (error)
		panic("vioinput: can't create status dma map");
	error = bus_dmamap_load(virtio_dmat(vsc), sc->sc_st_map,
	    sc->sc_st_buf, VIOIN_EV_SIZE, NULL, BUS_DMA_WAITOK);
	if (error)
		panic("vioinput: can't load status dma map");
}

static void
vioinput_event_fill(struct vioinput_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOIN_EVENTQ];
	int r, slot, ndone = 0;

	while ((r = virtio_enqueue_prep(vsc, vq, &slot)) == 0) {
		if (virtio_enqueue_reserve(vsc, vq, slot, 1) != 0)
			break;
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_ev_map,
		    slot * VIOIN_EV_SIZE, VIOIN_EV_SIZE,
		    BUS_DMASYNC_PREREAD);
		virtio_enqueue_p(vsc, vq, slot, sc->sc_ev_map,
		    slot * VIOIN_EV_SIZE, VIOIN_EV_SIZE, 0);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		ndone++;
	}
	KASSERT(r == 0 || r == EAGAIN);
	if (ndone > 0)
		virtio_notify(vsc, vq);
}

static int
vioinput_status_drain(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vioinput_softc *sc = device_private(virtio_child(vsc));

	return vioinput_status_purge(sc);
}

/*
 * Consume status messages written back by the device on the status
 * queue (driver -> device requests, e.g. LED state) and re-prime one
 * buffer so the device always has a place to write.  Called from
 * process/softint context via vioinput_event_soft(); the core does not
 * reliably deliver status queue interrupts.
 */
static int
vioinput_status_purge(struct vioinput_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOIN_STATUSQ];
	int slot, len, ndone = 0;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0)
		virtio_dequeue_commit(vsc, vq, slot), ndone++;

	if (ndone > 0 && virtio_enqueue_prep(vsc, vq, &slot) == 0 &&
	    virtio_enqueue_reserve(vsc, vq, slot, 1) == 0) {
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_st_map, 0,
		    VIOIN_EV_SIZE, BUS_DMASYNC_PREREAD);
		virtio_enqueue_p(vsc, vq, slot, sc->sc_st_map, 0,
		    VIOIN_EV_SIZE, 0);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		virtio_notify(vsc, vq);
	}
	return ndone;
}

static int
vioinput_event_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vioinput_softc *sc = device_private(virtio_child(vsc));

	softint_schedule(sc->sc_ev_si);
	return 1;
}

static void
vioinput_event_send(struct vioinput_softc *sc, uint16_t type, uint16_t code,
    uint32_t value)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOIN_STATUSQ];
	int slot;

	if (virtio_enqueue_prep(vsc, vq, &slot) != 0)
		return;
	if (virtio_enqueue_reserve(vsc, vq, slot, 1) != 0)
		return;
	sc->sc_st_buf->type = type;
	sc->sc_st_buf->code = code;
	sc->sc_st_buf->value = value;
	bus_dmamap_sync(virtio_dmat(vsc), sc->sc_st_map, 0, VIOIN_EV_SIZE,
	    BUS_DMASYNC_PREWRITE);
	virtio_enqueue_p(vsc, vq, slot, sc->sc_st_map, 0, VIOIN_EV_SIZE,
	    true);	/* read-only for the device: driver -> device */
	virtio_notify(vsc, vq);
}

/*
 * Indicate the CapsLock (bit 0), NumLock (bit 1) and ScrollLock (bit 2)
 * states requested by wskbd.  The device may or may not have LEDs for
 * them; QEMU's virtio-input simply accepts the events.
 */
static void
vioinput_set_leds(void *cookie, int led)
{
	static const uint16_t codes[] = { LED_CAPS, LED_NUM, LED_SCROLL };
	struct vioinput_softc *sc = cookie;
	unsigned i;

	for (i = 0; i < __arraycount(codes); i++)
		vioinput_event_send(sc, EV_LED, codes[i], (led >> i) & 1);
	vioinput_event_send(sc, EV_SYN, SYN_REPORT, 0);
}

static void
vioinput_handle_event(struct vioinput_softc *sc, struct vioinput_event *ev)
{

	switch (ev->type) {
	case EV_SYN:
		if (ev->code == SYN_REPORT && sc->sc_wsmousedev != NULL) {
			wsmouse_input(sc->sc_wsmousedev, sc->sc_buttons,
			    sc->sc_dx, sc->sc_dy, sc->sc_dz, sc->sc_dw,
			    WSMOUSE_INPUT_DELTA);
			sc->sc_dx = sc->sc_dy = sc->sc_dz = sc->sc_dw = 0;
		}
		break;
	case EV_KEY:
		if (sc->sc_wskbddev != NULL) {
			wskbd_input(sc->sc_wskbddev,
			    ev->value ? WSCONS_EVENT_KEY_DOWN :
			    WSCONS_EVENT_KEY_UP, ev->code);
			break;
		}
		switch (ev->code) {
		case BTN_LEFT:
			sc->sc_buttons = (sc->sc_buttons & ~0x1) |
			    (ev->value ? 0x1 : 0);
			break;
		case BTN_MIDDLE:
			sc->sc_buttons = (sc->sc_buttons & ~0x2) |
			    (ev->value ? 0x2 : 0);
			break;
		case BTN_RIGHT:
			sc->sc_buttons = (sc->sc_buttons & ~0x4) |
			    (ev->value ? 0x4 : 0);
			break;
		case BTN_SIDE:
			sc->sc_buttons = (sc->sc_buttons & ~0x8) |
			    (ev->value ? 0x8 : 0);
			break;
		case BTN_EXTRA:
			sc->sc_buttons = (sc->sc_buttons & ~0x10) |
			    (ev->value ? 0x10 : 0);
			break;
		}
		break;
	case EV_REL:
		switch (ev->code) {
		case REL_X:
			sc->sc_dx += ev->value;
			break;
		case REL_Y:
			/* evdev: positive is down; wscons: positive is up */
			sc->sc_dy -= ev->value;
			break;
		case REL_WHEEL:
			/* evdev: positive is up; wscons: positive is down */
			sc->sc_dz -= ev->value;
			break;
		case REL_HWHEEL:
			sc->sc_dw += ev->value;
			break;
		}
		break;
	case EV_LED:
		/* device reports its LED state; nothing to do */
		break;
	}
}

static void
vioinput_event_soft(void *arg)
{
	struct vioinput_softc *sc = arg;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOIN_EVENTQ];
	struct vioinput_event *ev;
	int slot, len;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		bus_dmamap_sync(virtio_dmat(vsc), sc->sc_ev_map,
		    slot * VIOIN_EV_SIZE, VIOIN_EV_SIZE,
		    BUS_DMASYNC_POSTREAD);
		ev = (struct vioinput_event *)(sc->sc_ev_buf +
		    slot * VIOIN_EV_SIZE);
		if (len >= VIOIN_EV_SIZE)
			vioinput_handle_event(sc, ev);
		virtio_dequeue_commit(vsc, vq, slot);
	}

	if (virtio_features(vsc) & VIRTIO_F_RING_EVENT_IDX)
		virtio_start_vq_intr(vsc, vq);
	vioinput_event_fill(sc);
	vioinput_status_purge(sc);
}

static void
vioinput_attach(device_t parent, device_t self, void *aux)
{
	struct vioinput_softc *sc = device_private(self);
	struct virtio_softc *vsc = device_private(parent);
	struct wskbddev_attach_args wa;
	struct wsmousedev_attach_args ma;

	if (virtio_child(vsc) != NULL) {
		aprint_error("child already attached for %s\n",
		    device_xname(parent));
		return;
	}

	sc->sc_dev = self;
	sc->sc_virtio = vsc;

	virtio_child_attach_start(vsc, self, IPL_TTY,
	    0, VIRTIO_COMMON_FLAG_BITS);

	/*
	 * Classify the device from its advertised event types:
	 * relative or absolute motion => mouse; keys only => keyboard.
	 */
	if (vioinput_has_evbit(sc, EV_REL) || vioinput_has_evbit(sc, EV_ABS))
		sc->sc_mouse = 1;

	sc->sc_ev_si = softint_establish(SOFTINT_SERIAL, vioinput_event_soft,
	    sc);
	if (sc->sc_ev_si == NULL) {
		aprint_error(": can't establish softint\n");
		goto fail;
	}

	virtio_init_vq_vqdone(vsc, &sc->sc_vqs[VIOIN_EVENTQ], VIOIN_EVENTQ,
	    vioinput_event_intr);
	if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOIN_EVENTQ], VIOIN_EV_SIZE,
	    1, "event") != 0) {
		aprint_error(": can't alloc event virtqueue\n");
		goto fail;
	}
	/*
	 * One event slot per ring descriptor; see the comment at
	 * VIOIN_EV_SIZE above.  Sized from the device-advertised queue
	 * length, then the DMA pool can be built.
	 */
	sc->sc_ev_bufsz = (bus_size_t)sc->sc_vqs[VIOIN_EVENTQ].vq_num *
	    VIOIN_EV_SIZE;
	vioinput_mem_init(sc);
	virtio_init_vq_vqdone(vsc, &sc->sc_vqs[VIOIN_STATUSQ], VIOIN_STATUSQ,
	    vioinput_status_drain);
	if (virtio_alloc_vq(vsc, &sc->sc_vqs[VIOIN_STATUSQ], VIOIN_EV_SIZE,
	    1, "status") != 0) {
		aprint_error(": can't alloc status virtqueue\n");
		goto fail;
	}

	if (virtio_child_attach_finish(vsc, sc->sc_vqs, 2, NULL, 0) != 0) {
		aprint_error(": can't finish attach\n");
		goto fail;
	}

	aprint_normal(": %s\n", sc->sc_mouse ? "virtio mouse" :
	    "virtio keyboard");

	vioinput_event_fill(sc);
	virtio_start_vq_intr(vsc, &sc->sc_vqs[VIOIN_EVENTQ]);

	if (sc->sc_mouse) {
		memset(&ma, 0, sizeof(ma));
		ma.accessops = &vioinput_wsmouseops;
		ma.accesscookie = sc;
		sc->sc_wsmousedev = config_found(self, &ma, wsmousedevprint,
		    CFARGS(.iattr = "wsmousedev"));
	} else {
		memset(&wa, 0, sizeof(wa));
		wa.console = 0;
		wa.keymap = &vioinput_keymapdata;
		wa.accessops = &vioinput_wskbdops;
		wa.accesscookie = sc;
		sc->sc_wskbddev = config_found(self, &wa, wskbddevprint,
		    CFARGS(.iattr = "wskbddev"));
	}

	return;
fail:
	virtio_child_attach_failed(vsc);
}
