/*	$NetBSD: apei_mapreg.c,v 1.1 2024/03/20 17:11:44 riastradh Exp $	*/

/*-
 * Copyright (c) 2024 The NetBSD Foundation, Inc.
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
 * Pre-mapped ACPI register access
 *
 * XXX This isn't APEI-specific -- it should be moved into the general
 * ACPI API, and unified with the AcpiRead/AcpiWrite implementation.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: apei_mapreg.c,v 1.1 2024/03/20 17:11:44 riastradh Exp $");

#include <sys/types.h>

#include <sys/atomic.h>

#include <dev/acpi/acpivar.h>
#include <dev/acpi/apei_mapreg.h>

/*
 * apei_mapreg_map(reg)
 *
 *	Return a mapping for use with apei_mapreg_read, or NULL if it
 *	can't be mapped.
 */
struct apei_mapreg *
apei_mapreg_map(const ACPI_GENERIC_ADDRESS *reg)
{

	/*
	 * Verify the result is reasonable.
	 */
	switch (reg->BitWidth) {
	case 8:
	case 16:
	case 32:
	case 64:
		break;
	default:
		return NULL;
	}

	/*
	 * Verify we know how to do the access width.
	 */
	switch (reg->AccessWidth) {
	case 1:			/* 8-bit */
	case 2:			/* 16-bit */
	case 3:			/* 32-bit */
	case 4:			/* 64-bit */
		break;
	default:
		return NULL;
	}

	/*
	 * Verify we don't need to shift anything, because I can't
	 * figure out how the shifting is supposed to work in five
	 * minutes of looking at the spec.
	 */
	switch (reg->BitOffset) {
	case 0:
		break;
	default:
		return NULL;
	}

	/*
	 * Verify the bit width is a multiple of the access width so
	 * we're not accessing more than we need.
	 */
	if (reg->BitWidth % (8*(1 << (reg->AccessWidth - 1))))
		return NULL;

	/*
	 * Dispatch on the space id.
	 *
	 * Currently this only handles memory space because I/O space
	 * is too painful to contemplate reimplementing here.
	 */
	switch (reg->SpaceId) {
	case ACPI_ADR_SPACE_SYSTEM_MEMORY:
		return AcpiOsMapMemory(reg->Address,
		    1 << (reg->AccessWidth - 1));
	default:
		return NULL;
	}
}

/*
 * apei_mapreg_unmap(reg, map)
 *
 *	Unmap a mapping previously returned by apei_mapreg_map.
 */
void
apei_mapreg_unmap(const ACPI_GENERIC_ADDRESS *reg,
    struct apei_mapreg *map)
{

	AcpiOsUnmapMemory(map, 1 << (reg->AccessWidth - 1));
}

/*
 * apei_mapreg_read(reg, map)
 *
 *	Read from reg via map previously obtained by apei_mapreg_map.
 */
uint64_t
apei_mapreg_read(const ACPI_GENERIC_ADDRESS *reg,
    const struct apei_mapreg *map)
{
	unsigned chunkbits = NBBY*(1 << (reg->AccessWidth - 1));
	unsigned i, n = reg->BitWidth % chunkbits;
	uint64_t v = 0;

	for (i = 0; i < n; i++) {
		uint64_t chunk;

		switch (reg->AccessWidth) {
		case 1:
			chunk = *(volatile const uint8_t *)map;
			break;
		case 2:
			chunk = *(volatile const uint16_t *)map;
			break;
		case 3:
			chunk = *(volatile const uint32_t *)map;
			break;
		case 4:
			chunk = *(volatile const uint64_t *)map;
			break;
		default:
			__unreachable();
		}
		v |= chunk << (i*chunkbits);
	}

	membar_acquire();	/* XXX probably not right for MMIO */
	return v;
}

/*
 * apei_mapreg_write(reg, map, v)
 *
 *	Write to reg via map previously obtained by apei_mapreg_map.
 */
void
apei_mapreg_write(const ACPI_GENERIC_ADDRESS *reg, struct apei_mapreg *map,
    uint64_t v)
{
	unsigned chunkbits = NBBY*(1 << (reg->AccessWidth - 1));
	unsigned i, n = reg->BitWidth % chunkbits;

	membar_release();	/* XXX probably not right for MMIO */
	for (i = 0; i < n; i++) {
		uint64_t chunk = v >> (i*chunkbits);

		switch (reg->AccessWidth) {
		case 1:
			*(volatile uint8_t *)map = chunk;
			break;
		case 2:
			*(volatile uint16_t *)map = chunk;
			break;
		case 3:
			*(volatile uint32_t *)map = chunk;
			break;
		case 4:
			*(volatile uint64_t *)map = chunk;
			break;
		default:
			__unreachable();
		}
	}
}
