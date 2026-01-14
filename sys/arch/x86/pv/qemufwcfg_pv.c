/* $NetBSD$ */

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Emile 'iMil' Heitor.
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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/bus.h>

#include <machine/cpufunc.h>

#include <x86/pv/pvvar.h>

#include <dev/ic/qemufwcfgvar.h>
#include <dev/ic/qemufwcfgio.h>

static int	fwcfg_pv_match(device_t, cfdata_t, void *);
static void	fwcfg_pv_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(qemufwcfg_pv, sizeof(struct fwcfg_softc),
    fwcfg_pv_match,
    fwcfg_pv_attach,
    NULL,
    NULL
);


static int
fwcfg_pv_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pv_attach_args *pvaa = (struct pv_attach_args *)aux;
	int fwcfg_found = 0;
	bus_space_tag_t iot = pvaa->pvaa_iot;
	bus_space_handle_t ioh;
	char sig[4];

	/* Temp mapping to check for FWCFG port */
	if (bus_space_map(iot, FWCFG_IO_BASE, FWCFG_IO_SIZE, 0, &ioh))
		return 0;

	bus_space_write_2(iot, ioh, FWCFG_SEL_REG,
	    FWCFG_SEL_SWAP(FW_CFG_SIGNATURE));

	bus_space_read_multi_1(iot, ioh, FWCFG_DATA_REG, sig, 4);

	if (memcmp(sig, "QEMU", 4) == 0) {
		aprint_verbose("qemufwcfg: found signature at 0x%x\n",
		    FWCFG_IO_BASE);
		fwcfg_found = 1;
	}

	bus_space_unmap(iot, ioh, FWCFG_IO_SIZE);

	return fwcfg_found;
}

static void
fwcfg_pv_attach(device_t parent, device_t self, void *aux)
{
	struct fwcfg_softc *sc = device_private(self);
	struct pv_attach_args *pvaa = (struct pv_attach_args *)aux;
	bus_space_tag_t iot = pvaa->pvaa_iot;
	bus_space_handle_t ioh;

	sc->sc_dev = self;
	sc->sc_bst = iot;

	if (bus_space_map(sc->sc_bst, FWCFG_IO_BASE,
	    FWCFG_DMA_IO_SIZE, 0, &ioh) != 0) {
		aprint_error(": couldn't map I/O ports\n");
		return;
	}

	sc->sc_bsh = ioh;

	aprint_naive("\n");
	aprint_normal("\n");

	fwcfg_attach(sc);
}
