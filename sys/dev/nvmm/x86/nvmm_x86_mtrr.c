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
 *
 * Intel 64 and IA-32 Architectures Software Developer's Manual
 * Combined Volumes 3A, 3B, 3C, and 3D: System Programming Guide, ch. 13.11
 * https://cdrdv2.intel.com/v1/dl/getContent/671447
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
nvmm_x86_mtrr_valid_memtype(uint8_t type)
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

static int
nvmm_x86_mtrr_getset(struct nvmm_x86_mtrr *mtrr, uint8_t physbits,
    uint32_t msr, uint64_t *val, bool write)
{
	uint64_t *mtrraddr = NULL;
	int i;

#ifdef NVMM_DEBUG
	printf("MTRR RECV MSR: %x (%u)\n", msr, write);
#endif

	switch(msr) {
	/* 13.11.1 MTRR Feature Identification */
	case MSR_MTRRcap:
		/*
		 * [7:0]: Number of variable range registers (8)
		 * 8: Fixed range registers supported
		 * 10: Write-combining memory type supported
		 */
		*val = 8 | __BIT(8) | __BIT(10);
		return 0;
	/* 13.11.2.1 IA32_MTRR_DEF_TYPE MSR */
	case MSR_MTRRdefType:
		mtrraddr = &mtrr->deftype;
		if (!write)
			break;
		/* We're writing
		 *
		 * [7:0]: Memory type
		 * [9:8]: Reserved
		 * 10: Fixed-range MTRRs enable/disable
		 * 11: MTRR enable/disable
		 * [63:12]: Reserved
		 */
		if (*val & (__BITS(8,9) | __BITS(12,63)))
			return EINVAL;
		/* validate memory type */
		if (!nvmm_x86_mtrr_valid_memtype((uint8_t)(*val & 0xff)))
			return EINVAL;
		break;
	/* 13.11.2.2 Fixed Range MTRRs */
	case MSR_MTRRfix64K_00000:
		mtrraddr = &mtrr->fixed_64k;
	/* FALLTHROUGH */
	case MSR_MTRRfix16K_80000 ... MSR_MTRRfix16K_A0000:
		if (mtrraddr == NULL) /* mtrraddr not set by previous case */
			mtrraddr = &mtrr->fixed_16k[msr - MSR_MTRRfix16K_80000];
	/* FALLTHROUGH */
	case MSR_MTRRfix4K_C0000 ... MSR_MTRRfix4K_F8000:
		if (mtrraddr == NULL)
			mtrraddr = &mtrr->fixed_4k[msr - MSR_MTRRfix4K_C0000];
		if (!write)
			break;
		/*
		 * The fixed memory ranges are mapped with 11 fixed-range
		 * registers of 64 bits each.
		 * Each of these registers is divided into 8-bit fields that are
		 * used to specify the memory type for each of the sub-ranges the
		 * register controls.
		 */
		for (i = 0; i < 8; i++) {
			uint8_t type = (uint8_t)((*val >> (i * 8)) & 0xff);
			if (!nvmm_x86_mtrr_valid_memtype(type))
				return EINVAL;
		}
		break;
	/*
	 * 13.11.2.3 Variable Range MTRRs
	 * 8 PhysBase / PhysMask pairs
	 */
	case MSR_MTRRphysBase0 ... MSR_MTRRphysMask7:
		mtrraddr = &mtrr->var_ranges[msr - MSR_MTRRphysBase0];
		if (!write)
			break;
		/* 63:MAXPHYADDR reserved */
		if (*val & (~__BITS(0, physbits - 1)))
			return EINVAL;
		if (msr & 1) { /* odd: phyMask */
			/* [10:0]: Reserved */
			if (*val & (__BITS(0,10)))
				return EINVAL;
		} else { /* even: phyBase */
			/* [11:8]: Reserved */
			if (*val & (__BITS(8,11)))
				return EINVAL;
		}
		break;
	default:
		return ENOENT;
	}

	if (write) { /* write mtrr */
#ifdef NVMM_DEBUG
		printf("MTRR: writing 0x%016lx at MSR 0x%x\n", *val, msr);
#endif
		*mtrraddr = *val;
	} else { /* read mtrr */
#ifdef NVMM_DEBUG
		printf("MTRR: reading 0x%016lx at MSR %x\n", *mtrraddr, msr);
#endif
		*val = *mtrraddr;
	}

	return 0;
}

int
nvmm_x86_mtrr_rdmsr(struct nvmm_x86_mtrr *mtrr, uint32_t msr, uint64_t *val)
{
	return nvmm_x86_mtrr_getset(mtrr, 0, msr, val, false);

}

int
nvmm_x86_mtrr_wrmsr(struct nvmm_x86_mtrr *mtrr, uint8_t physbits,
    uint32_t msr, uint64_t val)
{
	return nvmm_x86_mtrr_getset(mtrr, physbits, msr, &val, true);

}
