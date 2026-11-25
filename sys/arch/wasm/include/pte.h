/*	$NetBSD: pte.h,v 1.36 2022/08/21 09:12:43 riastradh Exp $	*/

/*
 * Copyright (c) 2001 Wasabi Systems, Inc.
 * All rights reserved.
 *
 * Written by Frank van der Linden for Wasabi Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed for the NetBSD Project by
 *      Wasabi Systems, Inc.
 * 4. The name of Wasabi Systems, Inc. may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY WASABI SYSTEMS, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL WASABI SYSTEMS, INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1997 Charles D. Cranor and Washington University.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _I386_PTE_H_
#define _I386_PTE_H_
#ifdef _KERNEL_OPT
#include "opt_xen.h"
#endif

/*
 * The PAE extension extends the size of the PTE to 64 bits (52bits physical
 * address) and is compatible with the amd64 PTE format. The first level
 * maps 2M, the second 1G, so a third level page table is introduced to
 * map the 4GB virtual address space. This PD has only 4 entries.
 * We can't use recursive mapping at level 3 to map the PD pages, as this
 * would eat one GB of address space. In addition, Xen imposes restrictions
 * on the entries we put in the L3 page (for example, the page pointed to by
 * the last slot can't be shared among different L3 pages), which makes
 * handling this L3 page in the same way we do for L2 on i386 (or L4 on amd64)
 * difficult. For most things we'll just pretend to have only 2 levels,
 * with the 2 high bits of the L2 index being in fact the index in the
 * L3.
 */

#if !defined(_LOCORE)

/*
 * here we define the data types for PDEs and PTEs
 */
#include <sys/stdint.h>
#ifdef PAE
typedef uint64_t pd_entry_t;		/* PDE */
typedef uint64_t pt_entry_t;		/* PTE */
#else
typedef uint32_t pd_entry_t;		/* PDE */
typedef uint32_t pt_entry_t;		/* PTE */
#endif

#endif

/*
 * Mask to get rid of the sign-extended part of addresses.
 */
#define VA_SIGN_MASK		0
#define VA_SIGN_NEG(va)		((va) | VA_SIGN_MASK)
/*
 * XXXfvdl this one's not right.
 */
#define VA_SIGN_POS(va)		((va) & ~VA_SIGN_MASK)

#ifdef PAE
#define L1_SHIFT	12
#define L2_SHIFT	21
#define L3_SHIFT	30
#define NBPD_L1		(1ULL << L1_SHIFT) /* # bytes mapped by L1 ent (4K) */
#define NBPD_L2		(1ULL << L2_SHIFT) /* # bytes mapped by L2 ent (2MB) */
#define NBPD_L3		(1ULL << L3_SHIFT) /* # bytes mapped by L3 ent (1GB) */

#define L3_MASK		0xc0000000
#define L2_REALMASK	0x3fe00000
#define L2_MASK		(L2_REALMASK | L3_MASK)
#define L1_MASK		0x001ff000

#define L3_FRAME	(L3_MASK)
#define L2_FRAME	(L3_FRAME | L2_MASK)
#define L1_FRAME	(L2_FRAME|L1_MASK)

#define PTE_4KFRAME	0x000ffffffffff000ULL
#define PTE_2MFRAME	0x000fffffffe00000ULL

#define PTE_FRAME	PTE_4KFRAME
#define PTE_LGFRAME	PTE_2MFRAME

/* macros to get real L2 and L3 index, from our "extended" L2 index */
#define l2tol3(idx)	((idx) >> (L3_SHIFT - L2_SHIFT))
#define l2tol2(idx)	((idx) & (L2_REALMASK >>  L2_SHIFT))

#else /* PAE */

#define L1_SHIFT	12
#define L2_SHIFT	22
#define NBPD_L1		(1UL << L1_SHIFT) /* # bytes mapped by L1 ent (4K) */
#define NBPD_L2		(1UL << L2_SHIFT) /* # bytes mapped by L2 ent (4MB) */

#define L2_MASK		0xffc00000
#define L1_MASK		0x003ff000

#define L2_FRAME	(L2_MASK)
#define L1_FRAME	(L2_FRAME|L1_MASK)

#define PTE_4KFRAME	0xfffff000
#define PTE_4MFRAME	0xffc00000

#define PTE_FRAME	PTE_4KFRAME
#define PTE_LGFRAME	PTE_4MFRAME

#endif /* PAE */

/*
 * x86 PTE/PDE bits.
 */
#define PTE_P		0x00000001	/* Present */
#define PTE_W		0x00000002	/* Write */
#define PTE_U		0x00000004	/* User */
#define PTE_PWT		0x00000008	/* Write-Through */
#define PTE_PCD		0x00000010	/* Cache-Disable */
#define PTE_A		0x00000020	/* Accessed */
#define PTE_D		0x00000040	/* Dirty */
#define PTE_PAT		0x00000080	/* PAT on 4KB Pages */
#define PTE_PS		0x00000080	/* Large Page Size */
#define PTE_G		0x00000100	/* Global Translation */
#define PTE_AVL1	0x00000200	/* Ignored by Hardware */
#define PTE_AVL2	0x00000400	/* Ignored by Hardware */
#define PTE_AVL3	0x00000800	/* Ignored by Hardware */
#define PTE_LGPAT	0x00001000	/* PAT on Large Pages */
#ifdef PAE
#define PTE_NX	0x8000000000000000ULL	/* No Execute */
#else
#define PTE_NX		0		/* Dummy */
#endif

#define	_MACHINE_PTE_H_X86
/*	$NetBSD: pte.h,v 1.7 2022/08/20 23:19:09 riastradh Exp $	*/

/*
 * Copyright (c) 2010 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Christoph Egger.
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

#ifndef _X86_PTE_H
#define _X86_PTE_H

#ifndef _MACHINE_PTE_H_X86
#error Use machine/pte.h, not x86/pte.h directly.
#endif

/* Cacheability bits when we are using PAT */
#define PGC_WB		0			/* The default */
#define PGC_WC		PTE_PWT			/* WT and CD is WC */
#define PGC_UCMINUS	PTE_PCD			/* UC but mtrr can override */
#define PGC_UC		(PTE_PWT | PTE_PCD)	/* hard UC */

/*
 * #PF exception bits
 */
#define PGEX_P		0x0001	/* the page was present */
#define PGEX_W		0x0002	/* exception during a write cycle */
#define PGEX_U		0x0004	/* exception while in user mode */
#define PGEX_RSVD	0x0008	/* a reserved bit was set in the page tables */
#define PGEX_I		0x0010	/* exception during instruction fetch */
#define PGEX_PK		0x0020	/* access disallowed by protection key */
#define PGEX_SGX	0x8000	/* violation of sgx-specific access rights */

/*
 * pl*_pi: index in the ptp page for a pde mapping a VA.
 * (pl*_i below is the index in the virtual array of all pdes per level)
 */
#define pl1_pi(VA)	(((VA_SIGN_POS(VA)) & L1_MASK) >> L1_SHIFT)
#define pl2_pi(VA)	(((VA_SIGN_POS(VA)) & L2_MASK) >> L2_SHIFT)
#define pl3_pi(VA)	(((VA_SIGN_POS(VA)) & L3_MASK) >> L3_SHIFT)
#define pl4_pi(VA)	(((VA_SIGN_POS(VA)) & L4_MASK) >> L4_SHIFT)
#define pl_pi(va, lvl) \
        (((VA_SIGN_POS(va)) & ptp_masks[(lvl)-1]) >> ptp_shifts[(lvl)-1])

/*
 * pl*_i: generate index into pde/pte arrays in virtual space
 */
#define pl1_i(VA)	(((VA_SIGN_POS(VA)) & L1_FRAME) >> L1_SHIFT)
#define pl2_i(VA)	(((VA_SIGN_POS(VA)) & L2_FRAME) >> L2_SHIFT)
#define pl3_i(VA)	(((VA_SIGN_POS(VA)) & L3_FRAME) >> L3_SHIFT)
#define pl4_i(VA)	(((VA_SIGN_POS(VA)) & L4_FRAME) >> L4_SHIFT)

#endif /* _X86_PTE_H */

#undef	_MACHINE_PTE_H_X86

#endif /* _I386_PTE_H_ */
