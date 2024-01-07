#ifndef _VIRTIO_CMDLINEBUSVAR_H_
#define _VIRTIO_CMDLINEBUSVAR_H_

struct cmdline_attach_args {
	bus_space_tag_t memt;
	bus_dma_tag_t dmat;
};

#endif
