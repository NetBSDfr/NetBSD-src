/*	$NetBSD: virtio.h,v 1.1 2026-10-26 smolBSD $	*/

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
 * LinuxKPI virtio API emulated on top of the NetBSD virtio(4) bus.
 * Implemented in external/bsd/drm2/virtio/virtio_compat.c.
 *
 * The NetBSD bus structures (struct virtqueue, struct virtio_softc) are
 * hidden behind pointers so that translation-unit-level tag collisions
 * with <dev/pci/virtiovar.h> never arise; only the compat layer sees
 * both.
 */

#ifndef	_LINUX_VIRTIO_H_
#define	_LINUX_VIRTIO_H_

#include <sys/types.h>
#include <sys/device.h>

#include <linux/types.h>
#include <linux/scatterlist.h>

struct virtio_softc;		/* NetBSD, <dev/pci/virtiovar.h> */

struct virtio_device;

struct virtqueue {
	void			 *vq_sc;	/* struct virtio_softc * */
	void			 *vq_nvq;	/* NetBSD struct virtqueue * */
	struct virtio_device	 *vdev;
	const char		 *vq_name;
	void			 (*vq_callback)(struct virtqueue *);
	void			**vq_cookies;	/* per-slot opaque tokens */
	unsigned		 vq_ncookies;
	unsigned		*vq_slotn;	/* descriptors per slot */
	unsigned		 vq_last_slot;
	int			 num_free;
};

typedef void vq_callback_t(struct virtqueue *);

struct virtio_config_ops {
	void	(*reset)(struct virtio_device *);
	void	(*del_vqs)(struct virtio_device *);
};

struct virtio_device {
	device_t		 dev;	/* our NetBSD virtio child device */
	void			*priv;
	void			*sc;	/* struct virtio_softc * */
	const struct virtio_config_ops *config;
	struct virtqueue	**vqs;
	int			 nvqs;
	bool			 is_pci;
	void			(*cfg_cb)(void *);
};

struct device_driver {
	const char	*name;
	void		*owner;
};

struct virtio_device_id {
	uint32_t	 vendor;
	uint32_t	 device;
};

#define	VIRTIO_DEV_ANY_ID	0xffffffffu

struct virtio_driver {
	struct device_driver		 driver;
	const struct virtio_device_id	*id_table;
	const unsigned int		*feature_table;
	unsigned int			 feature_table_size;
	int	(*probe)(struct virtio_device *);
	void	(*remove)(struct virtio_device *);
	void	(*config_changed)(struct virtio_device *);
};

#ifdef __NetBSD__
/* virtiodrm attaches via autoconf; keep the struct referenced. */
#define	module_virtio_driver(d)						\
	static struct virtio_driver *const					\
	    __attribute__((__unused__)) module_virtio_driver_ref = &(d)
#else
#define	module_virtio_driver(d)	/* virtiodrm attaches via autoconf */
#endif

static inline bool
virtio_device_is_pci(const struct virtio_device *vdev)
{

	return vdev->is_pci;
}

/*
 * Bus glue, implemented in virtio_compat.c over the NetBSD virtio API.
 */

struct virtio_device *virtio_create_compat(device_t, void * /* sc */);
void	virtio_destroy_compat(struct virtio_device *);
struct virtio_device *virtio_get_compat(device_t);
struct virtio_device *virtio_get_compat_by_softc(void * /* sc */);
#define	dev_to_virtio(dev)	virtio_get_compat((device_t)(dev))
int	virtio_compat_negotiate(struct virtio_device *, uint64_t features);
int	virtio_compat_finish(struct virtio_device *, void (*)(void *));

int	virtio_find_vqs(struct virtio_device *, unsigned,
	    struct virtqueue **,
	    vq_callback_t **callbacks,
	    const char * const names[], const bool *ctx);
void	virtio_del_vqs(struct virtio_device *);

int	virtqueue_add_sgs(struct virtqueue *, struct scatterlist *sgs[],
	    unsigned int out_sgs, unsigned int in_sgs, void *data, gfp_t);
void	*virtqueue_get_buf(struct virtqueue *, unsigned int *len);
bool	virtqueue_kick(struct virtqueue *);
bool	virtqueue_kick_prepare(struct virtqueue *);
bool	virtqueue_notify(struct virtqueue *);
void	virtqueue_disable_cb(struct virtqueue *);
bool	virtqueue_enable_cb(struct virtqueue *);
bool	virtqueue_enable_cb_prepare(struct virtqueue *);
unsigned int virtqueue_get_vring_size(const struct virtqueue *);
unsigned int virtqueue_get_free(const struct virtqueue *);

bus_dma_tag_t virtio_dev_dmat(struct virtio_device *);

#endif	/* _LINUX_VIRTIO_H_ */
