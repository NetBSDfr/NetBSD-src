/*	$NetBSD$	*/

/*-
 * Copyright (c) 2025 The NetBSD Foundation, Inc.
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
 * MTRR (Memory Type Range Register) virtualization support for NVMM.
 * Intel 64 and IA-32 Architectures Software Developer’s Manual, p. 473 10.11
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>

#include <x86/specialreg.h>

#include <dev/nvmm/nvmm.h>
#include <dev/nvmm/nvmm_internal.h>
#include <dev/nvmm/x86/nvmm_x86.h>

#define NVMM_DEBUG 1

static bool
mtrr_valid_memtype(uint8_t type)
{
	/*
	 * Type is only one of those
	 *
	 * 0 - Uncacheable (UC)
	 * 1 - Write Combining (WC)
	 * 2 - Reserved
	 * 3 - Reserved
	 * 4 - Write Through (WT)
	 * 5 - Write-protected (WP)
	 * 6 - Writeback (WB)
	 */
	uint8_t valid = __BITS(0,1) | __BITS(4,6);

	if (type > 7 || !((1 << type) & valid))
		return false;

	return true;
}

int
mtrr_getset(struct nvmm_machine *mach, struct nvmm_x86_mtrr *mtrr, 
    uint32_t msr, uint64_t *data)
{
	uint64_t *mtrraddr = NULL;
	int i;

	switch(msr) {
	case MSR_MTRRdefType:
		mtrraddr = &mtrr->deftype;
		/* no payload passed, read register */
		if (!*data)
			break;
		/* data was passed, we're writing
		 *
		 * [7:0]: Memory type
		 * [9:8]: Reserved
		 * 10: Fixed-range MTRRs enable/disable
		 * 11: MTRR enable/disable
		 * [63:12]: Reserved
		 */
		if (*data & (__BITS(8,9) | __BITS(12,63)))
			return EINVAL;
		/* validate memory type */
		if (!mtrr_valid_memtype((uint8_t)(*data & 0xff)))
			return EINVAL;
		break;
	case MSR_MTRRfix64K_00000:
		mtrraddr = &mtrr->fixed_64k;
	/* FALLTHROUGH */
	case MSR_MTRRfix16K_80000 ... MSR_MTRRfix16K_A0000:
		if (mtrraddr == NULL)
			mtrraddr = &mtrr->fixed_16k[msr - MSR_MTRRfix16K_80000];
	/* FALLTHROUGH */
	case MSR_MTRRfix4K_C0000 ... MSR_MTRRfix4K_F8000:
		if (mtrraddr == NULL)
			mtrraddr = &mtrr->fixed_4k[msr - MSR_MTRRfix4K_C0000];
		if (!*data)
			break;
		/*
		 * The fixed memory ranges are mapped with 11 fixed-range
		 * registers of 64 bits each.
		 * Each of these registers is divided into 8-bit fields that are
		 * used to specify the memory type for each of the sub-ranges the
		 * register controls.
		 */
		for (i = 0; i < 8; i++) {
			uint8_t type = (uint8_t)((*data >> (i * 8)) & 0xff);
			if (!mtrr_valid_memtype(type))
				return EINVAL;
		}
		break;
	case MSR_MTRRphysBase0 ... MSR_MTRRphysMask15:
		mtrraddr = &mtrr->var_ranges[msr - MSR_MTRRphysBase0];
		if (!*data)
			break;
		/* 63:MAXPHYSADD reserved, i.e. is data > MAX_RAM */
		if ((*data & __BITS(12, 63)) > mach->gpa_end)
			return EINVAL;
		if (msr | 1) { /* even: phyBase */
			/* [11:8]: Reserved */
			if (*data & (__BITS(8,11)))
				return EINVAL;
		} else { /* odd: phyMask */
			/* [10:0]: Reserved */
			if (*data & (__BITS(0,10)))
				return EINVAL;
		}
		break;
	default:
		return ENOENT;
	}

	if (*data) { /* write mtrr */
#ifdef NVMM_DEBUG
		printf("MTRR: writing 0x%016lx at MSR 0x%x\n", *data, msr);
#endif
		*mtrraddr = *data;
	} else { /* read mtrr */
#ifdef NVMM_DEBUG
		printf("MTRR: reading 0x%016lx at MSR %x\n", *mtrraddr, msr);
#endif
		*data = *mtrraddr;
	}

	return 0;
}
