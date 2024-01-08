#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <machine/bus_private.h>

#include <dev/pv/pvvar.h>

struct x86_bus_dma_tag pvbus_bus_dma_tag = {
	._tag_needs_free	= 0,
	._bounce_thresh		= 0,
	._bounce_alloc_lo	= 0,
	._bounce_alloc_hi	= 0,
	._may_bounce		= NULL,
};

static int
pv_match(device_t parent, cfdata_t match, void *aux)
{
	return 1;
}

static void
pv_attach(device_t parent, device_t self, void *aux) {
	struct pv_attach_args pvaa;

	pvaa.pvaa_memt = x86_bus_space_mem;
	pvaa.pvaa_dmat = &pvbus_bus_dma_tag;

	aprint_naive("\n");

	config_found(self, &pvaa, NULL, CFARGS_NONE);
}

CFATTACH_DECL_NEW(pv, sizeof(struct pv_softc),
		  pv_match, pv_attach, NULL, NULL);
