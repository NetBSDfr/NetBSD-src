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
 * Adapted from Linux KVM implementation (arch/x86/kvm/mtrr.c).
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


static uint64_t *
nvmm_x86_mtrr_find(struct nvmm_x86_mtrr *mtrr, uint32_t msr)
{
	u_int idx;

	/* Variable-range MTRRs (PhysBase/PhysMask pairs) */
	if (msr >= MSR_MTRRphysBase0 && msr <= MSR_MTRRphysMask15) {
		idx = msr - MSR_MTRRphysBase0;
		return &mtrr->var_ranges[idx];
	}

	/* Fixed-range MTRRs and default-type MSR */
	switch (msr) {
	case MSR_MTRRfix64K_00000:
		return &mtrr->fixed_64k;
	case MSR_MTRRfix16K_80000:
	case MSR_MTRRfix16K_A0000:
		idx = msr - MSR_MTRRfix16K_80000;
		return &mtrr->fixed_16k[idx];
	case MSR_MTRRfix4K_C0000:
	case MSR_MTRRfix4K_C8000:
	case MSR_MTRRfix4K_D0000:
	case MSR_MTRRfix4K_D8000:
	case MSR_MTRRfix4K_E0000:
	case MSR_MTRRfix4K_E8000:
	case MSR_MTRRfix4K_F0000:
	case MSR_MTRRfix4K_F8000:
		idx = msr - MSR_MTRRfix4K_C0000;
		return &mtrr->fixed_4k[idx];
	case MSR_MTRRdefType:
		return &mtrr->deftype;
	default:
		return NULL;
	}
}

static bool
nvmm_x86_mtrr_valid_type(uint8_t type)
{

	if (type >= 8)
		return false;
	/* Use a bitmask to test allowed values: bits 0,1,4,5,6 against 0x73 */
	return ((1 << type) & 0x73) != 0;
}

/*
 * Validate an MTRR MSR value before accepting a write from the guest.
 */
int
nvmm_x86_mtrr_valid(struct nvmm_machine *mach, uint32_t msr, uint64_t data)
{
	uint8_t type;
	int i;

	if (msr == MSR_MTRRdefType) {
		/* Reserved bits above bit 11 and bits [9:8] must be zero */
		if ((data & ~__BITS(0, 11)) != 0)
			return EINVAL;
		if ((data & __BITS(8, 9)) != 0)
			return EINVAL;
		type = (uint8_t)(data & __BITS(0, 7));
		if (!nvmm_x86_mtrr_valid_type(type))
			return EINVAL;
		return 0;
	}

	if (msr >= MSR_MTRRfix64K_00000 && msr <= MSR_MTRRfix4K_F8000) {
		for (i = 0; i < 8; i++) {
			type = (uint8_t)((data >> (i * 8)) & 0xff);
			if (!nvmm_x86_mtrr_valid_type(type))
				return EINVAL;
		}
		return 0;
	}

	/* Lastly, variable-range registers */
	if (msr < MSR_MTRRphysBase0 || msr > MSR_MTRRphysBase15)
		return EINVAL;

	/* Even / physBase registers */
	if (!(msr & 1)) {
		/* clear out bits < 12 */
		uint64_t base = data & __BITS(12, 63);

		/* Reserved bits [11:8] must be zero. */
		if ((data & __BITS(8, 11)) != 0)
			return EINVAL;
		type = (uint8_t)(data & 0xff);
		if (!nvmm_x86_mtrr_valid_type(type))
			return EINVAL;
		if (base >= mach->gpa_end)
			return EINVAL;

		return 0;
	} else {
		/*
		 * Odd physMask registers.
		 *
		 * Bits [10:0] must be zero
		 */
		if ((data & __BITS(0, 10)) != 0)
			return EINVAL;

		return 0;
	}

	return EINVAL;
}

int
nvmm_x86_mtrr_set_msr(struct nvmm_machine *mach, struct nvmm_x86_mtrr *mtrr,
    uint32_t msr, uint64_t data)
{
	uint64_t *ptr;

	ptr = nvmm_x86_mtrr_find(mtrr, msr);
	if (ptr == NULL)
		return ENOENT;

	if (nvmm_x86_mtrr_valid(mach, msr, data) != 0)
		return EINVAL;

	*ptr = data;
	return 0;
}

int
nvmm_x86_mtrr_get_msr(struct nvmm_x86_mtrr *mtrr, uint32_t msr,
    uint64_t *valp)
{
	const uint64_t *ptr;

	if (valp == NULL)
		return EFAULT;

	if (msr == MSR_MTRRcap) {
		/*
		 * Bits [7:0]: count of variable-size MTRRs supported
		 * Bit 8: all fixed-size MTRRs are available
		 * Bit 10: WC type is available
		 */
		*valp = (uint64_t)NVMM_X86_NR_VAR_MTRR |
		    __BIT(8) | __BIT(10);
		return 0;
	}

	ptr = nvmm_x86_mtrr_find(mtrr, msr);
	if (ptr == NULL)
		return ENOENT;

	*valp = *ptr;
	return 0;
}
