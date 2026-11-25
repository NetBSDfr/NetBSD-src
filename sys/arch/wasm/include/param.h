/*	$NetBSD$	*/

/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This software is derived from sys/arch/i386/include/param.h
 * (BSD-3-Clause) and adapts it for the WebAssembly wasm32 target.
 */

#ifndef _WASM_PARAM_H_
#define _WASM_PARAM_H_

#ifdef _KERNEL_OPT
#include "opt_param.h"
#endif

/*
 * Machine dependent constants for WebAssembly.
 */

#define	MAXCPUS		1

#ifdef _KERNEL
#include <machine/cpu.h>
#endif

#define	_MACHINE	wasm32
#define	MACHINE		"wasm32"
#define	_MACHINE_ARCH	wasm32
#define	MACHINE_ARCH	"wasm32"
#define	MID_MACHINE	0

#define ALIGNED_POINTER(p,t)		1
#define ALIGNED_POINTER_LOAD(q,p,t)	memcpy((q), (p), sizeof(t))

#define	STACK_ALIGNBYTES	(8 - 1)

/*
 * NBPG is the wasm page size: 64 KiB. uvm software pages are 4 KiB
 * (PAGE_SIZE in vmparam.h); hardware protection granularity is
 * "whole linear memory" (none).
 */
#define	PGSHIFT		16		/* LOG2(NBPG) */
#define	NBPG		(1 << PGSHIFT)	/* bytes/page */
#define	PGOFSET		(NBPG-1)	/* byte offset into page */

/*
 * Maximum physical memory: wasm32 linear memory limit.
 */
#define MAXPHYSMEM	0x100000000ULL	/* 4GB */

#ifndef	KERNBASE
#define	KERNBASE	0x0		/* VA == PA, flat linear memory */
#endif

#define	SSIZE		1		/* initial stack size/NBPG */
#define	SINCR		1		/* increment of stack/NBPG */

#ifndef UPAGES
# ifdef DIAGNOSTIC
#  define	UPAGES		3
# else
#  define	UPAGES		2
# endif /* DIAGNOSTIC */
#endif /* !defined(UPAGES) */
#define	USPACE		(UPAGES * NBPG)	/* total size of u-area */
#define	INTRSTACKSIZE	8192

#ifndef MSGBUFSIZE
#define MSGBUFSIZE	(16*NBPG)	/* default message buffer size */
#endif

/*
 * Constants related to network buffer management.
 */
#define	MSIZE		256		/* size of an mbuf */

#ifndef MCLSHIFT
#define	MCLSHIFT	11
#endif	/* MCLSHIFT */

#define	MCLBYTES	(1 << MCLSHIFT)	/* size of a m_buf cluster */

#ifndef NMBCLUSTERS_MAX
#define	NMBCLUSTERS_MAX	(0x4000000 / MCLBYTES)
#endif

#ifndef NFS_RSIZE
#define NFS_RSIZE	32768
#endif
#ifndef NFS_WSIZE
#define NFS_WSIZE	32768
#endif

/*
 * Minimum and maximum sizes of the kernel malloc arena in PAGE_SIZE-sized
 * logical pages.
 */
#define	NKMEMPAGES_MIN_DEFAULT	((16 * 1024 * 1024) >> PAGE_SHIFT)
#define	NKMEMPAGES_MAX_DEFAULT	((360 * 1024 * 1024) >> PAGE_SHIFT)

#endif /* _WASM_PARAM_H_ */
