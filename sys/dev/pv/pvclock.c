/* $NetBSD$ */

/*-
 * Copyright (c) 2024 The NetBSD Foundation, Inc.
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

/*	$OpenBSD: pvclock.c,v 1.9 2023/02/04 19:19:37 cheloha Exp $	*/

/*
 * Copyright (c) 2018 Reyk Floeter <reyk@openbsd.org>
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

#if !defined(__i386__) && !defined(__amd64__)
#error pvclock(4) is only supported on i386 and amd64
#endif

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/timetc.h>
#include <sys/atomic.h>

#include <machine/cpu.h>
#include <uvm/uvm_extern.h>

#include <dev/pv/pvreg.h>

#define virtio_membar_consumer()	membar_consumer()

uint pvclock_lastcount;

struct pvclock_softc {
	device_t		 sc_dev;
	void			*sc_time;
	paddr_t			 sc_paddr;
	struct timecounter	*sc_tc;
};

static int	 pvclock_match(device_t, cfdata_t, void *);
static void	 pvclock_attach(device_t, device_t, void *);

/*
void	 pvclock_read_time_info(struct pvclock_softc *,
	    struct pvclock_time_info *);
*/
static inline uint32_t
	pvclock_read_begin(const struct pvclock_time_info *);
static inline int
	pvclock_read_done(const struct pvclock_time_info *, uint32_t);
static uint
	pvclock_get_timecount(struct timecounter *);

struct timecounter pvclock_timecounter = {
	.tc_get_timecount = pvclock_get_timecount,
	.tc_counter_mask = ~0u,
	.tc_frequency = 0,
	.tc_name = NULL,
	.tc_quality = -2000,
	.tc_priv = NULL,
};

CFATTACH_DECL_NEW(pvclock, sizeof(struct pvclock_softc),
	pvclock_match, pvclock_attach, NULL, NULL);


static int
pvclock_match(device_t parent, cfdata_t cf, void *aux)
{
	u_int regs[6];
	/*
	 * pvclock is provided by different hypervisors, we currently
	 * only support the "kvmclock".
	 */
	x86_cpuid(0x40000000 + CPUID_OFFSET_KVM_FEATURES, regs);
	/*
	 * We only implement support for the 2nd version of pvclock.
	 * The first version is basically the same but with different
	 * non-standard MSRs and it is deprecated.
	 */
	if ((regs[0] & (1 << KVM_FEATURE_CLOCKSOURCE2)) == 0)
		return (0);

	/*
	 * Only the "stable" clock with a sync'ed TSC is supported.
	 * In this case the host guarantees that the TSC is constant
	 * and invariant, either by the underlying TSC or by passing
	 * on a synchronized value.
	 */
	if ((regs[0] &
	    (1 << KVM_FEATURE_CLOCKSOURCE_STABLE_BIT)) == 0)
		return (0);
	return (1);
}

static inline uint32_t
pvclock_read_begin(const struct pvclock_time_info *ti)
{
	uint32_t ti_version = ti->ti_version & ~0x1;
	virtio_membar_consumer();
	return (ti_version);
}

static inline int
pvclock_read_done(const struct pvclock_time_info *ti,
    uint32_t ti_version)
{
	virtio_membar_consumer();
	return (ti->ti_version == ti_version);
}

static uint
pvclock_get_timecount(struct timecounter *tc)
{
	struct pvclock_softc		*sc = tc->tc_priv;
	struct pvclock_time_info	*ti;
	uint64_t			 tsc_timestamp, system_time, delta, ctr;
	uint32_t			 ti_version, mul_frac;
	int8_t				 shift;
	uint8_t				 flags;

	ti = sc->sc_time;
	do {
		ti_version = pvclock_read_begin(ti);
		system_time = ti->ti_system_time;
		tsc_timestamp = ti->ti_tsc_timestamp;
		mul_frac = ti->ti_tsc_to_system_mul;
		shift = ti->ti_tsc_shift;
		flags = ti->ti_flags;
	} while (!pvclock_read_done(ti, ti_version));

	/*
	 * The algorithm is described in
	 * linux/Documentation/virtual/kvm/msr.txt
	 */
	delta = rdtsc() - tsc_timestamp;
	if (shift < 0)
		delta >>= -shift;
	else
		delta <<= shift;
	ctr = ((delta * mul_frac) >> 32) + system_time;

	if ((flags & PVCLOCK_FLAG_TSC_STABLE) != 0)
		return (ctr);

	if (ctr < pvclock_lastcount)
		return (pvclock_lastcount);

	atomic_swap_uint(&pvclock_lastcount, ctr);

	return (ctr);
}

static void
pvclock_attach(device_t parent, device_t self, void *aux)
{
	struct pvclock_softc		*sc = device_private(self);
	struct pvclock_time_info	*ti;
	paddr_t			 	 pa;
	uint32_t			 ti_version;
	uint8_t				 flags;

	aprint_naive("\n");
	aprint_normal("\n");

	if ((sc->sc_time = (void *)uvm_km_alloc(kernel_map,
		PAGE_SIZE, PAGE_SIZE,
		UVM_KMF_WIRED | UVM_KMF_ZERO)) == NULL) {
		aprint_error("time page allocation failed\n");
		return;
	}

	if (!pmap_extract(pmap_kernel(), (vaddr_t)sc->sc_time, &pa)) {
		aprint_error("time page PA extraction failed\n");
		uvm_km_free(kernel_map, (vaddr_t)sc->sc_time,
			PAGE_SIZE, UVM_KMF_WIRED);
		sc->sc_time = NULL;
		return;
	}

	wrmsr(KVM_MSR_SYSTEM_TIME, pa | PVCLOCK_SYSTEM_TIME_ENABLE);
	sc->sc_paddr = pa;

	sc->sc_dev = self;

	ti = sc->sc_time;
	do {
		ti_version = pvclock_read_begin(ti);
		flags = ti->ti_flags;
	} while (!pvclock_read_done(ti, ti_version));

	sc->sc_tc = &pvclock_timecounter;
	sc->sc_tc->tc_name = device_xname(sc->sc_dev);

	sc->sc_tc->tc_frequency = 1000000000ULL;
	sc->sc_tc->tc_priv = sc;

	pvclock_lastcount = 0;

	/* Better than HPET but below TSC */
	sc->sc_tc->tc_quality = 1500;

	if ((flags & PVCLOCK_FLAG_TSC_STABLE) == 0) {
		/* if tsc is not stable, set a lower priority */
		/* Better than i8254 but below HPET */
		sc->sc_tc->tc_quality = 500;
	}

	tc_init(sc->sc_tc);
}
