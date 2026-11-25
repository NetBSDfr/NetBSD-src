/*	$NetBSD$	*/

/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This software is derived from sys/arch/i386/include/types.h
 * (BSD-3-Clause) and adapts it for the WebAssembly wasm32 target.
 */

#ifndef	_WASM_MACHTYPES_H_
#define	_WASM_MACHTYPES_H_

#include <sys/cdefs.h>
#include <sys/featuretest.h>
#include <machine/int_types.h>

typedef int			__register_t;
typedef unsigned long		__vaddr_t;
typedef unsigned char		__cpu_simple_lock_nv_t;

#if defined(_KERNEL)
typedef struct label_t {
	int val[8];
} label_t;

typedef unsigned long		paddr_t;
typedef unsigned long		psize_t;
#define	PRIxPADDR	"lx"
#define	PRIxPSIZE	"lx"
#define	PRIuPSIZE	"lu"

#elif defined(_KMEMUSER) || defined(_KERNTYPES) || defined(_STANDALONE)
typedef __uint64_t	paddr_t;
typedef __uint64_t	psize_t;
#define	PRIxPADDR	"llx"
#define	PRIxPSIZE	"llx"
#define	PRIuPSIZE	"llu"

#endif /* _KERNEL */

#if defined(_KERNEL) || defined(_KMEMUSER) || defined(_KERNTYPES) || defined(_STANDALONE)

typedef __vaddr_t	vaddr_t;
typedef unsigned long	vsize_t;
#define	PRIxVADDR	"lx"
#define	PRIxVSIZE	"lx"
#define	PRIuVSIZE	"lu"

typedef __register_t	register_t;
#define	PRIxREGISTER	"x"

#endif /* _KERNEL || _KMEMUSER */

#define	__CPU_SIMPLE_LOCK_PAD

#define	__SIMPLELOCK_LOCKED	1
#define	__SIMPLELOCK_UNLOCKED	0

#if !__has_feature(undefined_behavior_sanitizer) && \
	!defined(__SANITIZE_UNDEFINED__)
/* wasm linear memory accesses may be unaligned (align=1). */
#define	__NO_STRICT_ALIGNMENT
#endif

#define	__HAVE_NEW_STYLE_BUS_H
#define	__HAVE_CPU_DATA_FIRST
#define	__HAVE_SYSCALL_INTERN
#define	__HAVE_MINIMAL_EMUL
#define	__HAVE_OLD_DISKLABEL
#define	__HAVE_CPU_LWP_SETPRIVATE
#define	__HAVE_ATOMIC64_OPS
#define	__HAVE_UCAS_FULL
#define	__HAVE___LWP_GETPRIVATE_FAST
#define	__HAVE_TLS_VARIANT_II
#define	__HAVE_COMMON___TLS_GET_ADDR

#endif	/* _WASM_MACHTYPES_H_ */
