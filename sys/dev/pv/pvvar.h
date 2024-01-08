#ifndef _PVBUS_PVVAR_H_
#define _PVBUS_PVVAR_H_

struct pv_softc {
	device_t		sc_dev;
};

struct pvbus_attach_args {
	const char		*pvba_busname;
};

struct pv_attach_args {
	bus_space_tag_t		pvaa_memt;
	bus_dma_tag_t		pvaa_dmat;
};

#endif
