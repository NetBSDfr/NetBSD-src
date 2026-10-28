/*	$NetBSD: virtio_config.h,v 1.1 2026-10-26 smolBSD $	*/

/*
 * LinuxKPI virtio config-space access emulated over the NetBSD virtio
 * device-config accessors.  Feature-bit definitions come from the Linux
 * uapi header.
 */

#ifndef	_LINUX_VIRTIO_CONFIG_H_
#define	_LINUX_VIRTIO_CONFIG_H_

#include <sys/stddef.h>

#include <uapi/linux/virtio_config.h>

#include <linux/types.h>
#include <linux/virtio.h>

bool	virtio_has_feature(const struct virtio_device *, unsigned int);

void	virtio_read_config_compat(const struct virtio_device *, size_t off,
	    void *dst, size_t len);
void	virtio_write_config_compat(const struct virtio_device *, size_t off,
	    const void *src, size_t len);

#define	virtio_cread(vdev, structtype, member, ptr)			      \
	virtio_read_config_compat((vdev),				      \
	    offsetof(structtype, member), (ptr),			      \
	    sizeof(((structtype *)NULL)->member))

#define	virtio_cread_byte(vdev, byteoff, ptr)				      \
	virtio_read_config_compat((vdev), (byteoff), (ptr), 1)
#define	virtio_cread16(vdev, byteoff, ptr)				      \
	virtio_read_config_compat((vdev), (byteoff), (ptr), 2)
#define	virtio_cread32(vdev, byteoff, ptr)				      \
	virtio_read_config_compat((vdev), (byteoff), (ptr), 4)
#define	virtio_cread64(vdev, byteoff, ptr)				      \
	virtio_read_config_compat((vdev), (byteoff), (ptr), 8)

#define	virtio_cwrite(vdev, structtype, member, val)			      \
	do {								      \
		__typeof__(val) _virtio_cwrite_val = (val);		      \
		virtio_write_config_compat((vdev),			      \
		    offsetof(structtype, member), &_virtio_cwrite_val,	      \
		    sizeof(((structtype *)NULL)->member));		      \
	} while (0)

#define	virtio_cwrite_byte(vdev, byteoff, val)				      \
	do {								      \
		uint8_t _virtio_cwrite_val = (val);			      \
		virtio_write_config_compat((vdev), (byteoff),		      \
		    &_virtio_cwrite_val, 1);				      \
	} while (0)
#define	virtio_cwrite16(vdev, byteoff, val)				      \
	do {								      \
		uint16_t _virtio_cwrite_val = (val);			      \
		virtio_write_config_compat((vdev), (byteoff),		      \
		    &_virtio_cwrite_val, 2);				      \
	} while (0)
#define	virtio_cwrite32(vdev, byteoff, val)				      \
	do {								      \
		uint32_t _virtio_cwrite_val = (val);			      \
		virtio_write_config_compat((vdev), (byteoff),		      \
		    &_virtio_cwrite_val, 4);				      \
	} while (0)

#define	virtio_device_ready(vdev)	/* the NetBSD bus did this for us */

/*
 * x86-class DMA tags without an IOMMU: the device and the guest share
 * physical addresses, so the driver's explicit dma_map_sg()/sync
 * shortcut is unnecessary — the compat virtqueue_add_sgs() maps
 * buffers through bus_dma regardless.
 */
#define	virtio_has_iommu_quirk(vdev)	true

#endif	/* _LINUX_VIRTIO_CONFIG_H_ */
