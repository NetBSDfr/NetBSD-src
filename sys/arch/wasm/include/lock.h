/*	$NetBSD: lock.h,v 1.1 2026-11-25 00:00:00 imil Exp $	*/

/*-
 * Copyright (c) 2026 The smolBSD Project
 * All rights reserved.
 *
 * This code is derived from software contributed to The smolBSD Project
 * by the smolBSD project.
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
 * THIS SOFTWARE IS PROVIDED BY THE smolBSD PROJECT AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Machine-dependent spin lock operations for wasm32.
 *
 * WebAssembly is sequentially consistent: every atomic operation is a
 * coherence point for all threads, so acquire/release semantics are
 * obtained directly from the ordering arguments and no additional
 * fences or spin hints are available or needed.
 */

#ifndef	_WASM_LOCK_H_
#define	_WASM_LOCK_H_

/*
 * Drop the volatile qualifier from lock pointers.  sys/cdefs.h (hence
 * __DECONST) is not yet visible at this point in the include chain; the
 * cast through an integer type also keeps -Wcast-qual quiet.
 */
#define	__SIMPLELOCK_NV(p) \
    ((__cpu_simple_lock_nv_t *)(uintptr_t)(p))

/*
 * Unlike the asm stubs architectures (amd64 et al.), wasm provides no
 * out-of-line definitions of these functions, so the bodies are always
 * emitted in the header as static inlines, for every consumer, kernel
 * and libkern alike.
 */
#define	SPINLOCK_SPIN_HOOK	/* nothing */
#define	SPINLOCK_BACKOFF_HOOK	/* wasm has no spin hint */
#define	SPINLOCK_BODY
#define	SPINLOCK_INLINE	static __inline __unused

static __inline int
__SIMPLELOCK_LOCKED_P(const __cpu_simple_lock_t *__ptr)
{

	return __atomic_load_n(__SIMPLELOCK_NV(__ptr), __ATOMIC_RELAXED) ==
	    __SIMPLELOCK_LOCKED;
}

static __inline int
__SIMPLELOCK_UNLOCKED_P(const __cpu_simple_lock_t *__ptr)
{

	return !__SIMPLELOCK_LOCKED_P(__ptr);
}

static __inline void
__cpu_simple_lock_set(__cpu_simple_lock_t *__ptr)
{

	__atomic_store_n(__SIMPLELOCK_NV(__ptr), __SIMPLELOCK_LOCKED,
	    __ATOMIC_RELAXED);
}

static __inline void
__cpu_simple_lock_clear(__cpu_simple_lock_t *__ptr)
{

	__atomic_store_n(__SIMPLELOCK_NV(__ptr), __SIMPLELOCK_UNLOCKED,
	    __ATOMIC_RELAXED);
}

SPINLOCK_INLINE void	__cpu_simple_lock_init(__cpu_simple_lock_t *);
SPINLOCK_INLINE void	__cpu_simple_lock(__cpu_simple_lock_t *);
SPINLOCK_INLINE int	__cpu_simple_lock_try(__cpu_simple_lock_t *);
SPINLOCK_INLINE void	__cpu_simple_unlock(__cpu_simple_lock_t *);

#ifdef	SPINLOCK_BODY
SPINLOCK_INLINE void
__cpu_simple_lock_init(__cpu_simple_lock_t *lockp)
{

	__cpu_simple_lock_clear(lockp);
}

SPINLOCK_INLINE int
__cpu_simple_lock_try(__cpu_simple_lock_t *lockp)
{
	__cpu_simple_lock_nv_t expected = __SIMPLELOCK_UNLOCKED;

	return __atomic_compare_exchange_n(__SIMPLELOCK_NV(lockp), &expected,
	    __SIMPLELOCK_LOCKED, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

SPINLOCK_INLINE void
__cpu_simple_lock(__cpu_simple_lock_t *lockp)
{

	while (!__cpu_simple_lock_try(lockp))
		/* spin */;
}

SPINLOCK_INLINE void
__cpu_simple_unlock(__cpu_simple_lock_t *lockp)
{

	__atomic_store_n(__SIMPLELOCK_NV(lockp), __SIMPLELOCK_UNLOCKED,
	    __ATOMIC_RELEASE);
}
#endif	/* SPINLOCK_BODY */

#endif /* !_WASM_LOCK_H_ */
