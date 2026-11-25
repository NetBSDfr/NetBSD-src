/*	$NetBSD: atomic.c,v 1.1 2026/11/25 00:00:00 imil Exp $	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by the WebAssembly port maintainers.
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
#include <sys/atomic.h>

/*
 * Machine atomic operations for WebAssembly.
 *
 * The whole API declared by <sys/atomic.h> is implemented here on top
 * of the compiler's __atomic builtins, which lower to wasm i32/i64
 * atomic instructions (memory.atomic.* / i64.atomic.*).  64-bit
 * atomics are native even in the 32-bit address model.
 *
 * The WebAssembly (and JS shared-memory) memory model is sequentially
 * consistent: every memory access participates in one global total
 * order, so all memory barriers are no-ops and SEQ_CST is the natural
 * ordering for every operation.  The underscored spelling of each
 * operation is the real implementation; the public names forward to
 * them so that both spellings are linkable, mirroring the layout of
 * the per-architecture atomic implementations on other ports.
 *
 * Even the 8- and 16-bit compare-exchange lower natively, to the
 * sign-extending i32.atomic.rmw8/rmw16.cmpxchg instructions.
 */

#define	_MEMSEQ	__ATOMIC_SEQ_CST

/*
 * Memory barriers: all no-ops in a sequentially consistent model.
 * They remain external function calls, so they still keep the
 * compiler from moving accesses across them.
 */
void
_membar_acquire(void)
{
}

void
_membar_release(void)
{
}

void
_membar_sync(void)
{
	__atomic_thread_fence(_MEMSEQ);
}

void
_membar_producer(void)
{
}

void
_membar_consumer(void)
{
}

void
_membar_enter(void)
{
}

void
_membar_exit(void)
{
}

/*
 * Operation definition farm.
 */
#define	_IMPL_CAS(name, type)						\
type									\
_atomic_cas_##name(volatile type *p, type o, type n)			\
{									\
	type expected = o;						\
	(void)__atomic_compare_exchange_n(p, &expected, n, false,	\
	    _MEMSEQ, _MEMSEQ);						\
	return expected;						\
}									\
type									\
_atomic_cas_##name##_ni(volatile type *p, type o, type n)		\
{									\
	type old = _atomic_cas_##name(p, o, n);				\
	return old == o ? n : old;					\
}

#define	_IMPL_SWAP(name, type)						\
type									\
_atomic_swap_##name(volatile type *p, type v)				\
{									\
	return __atomic_exchange_n(p, v, _MEMSEQ);			\
}

#define	_IMPL_BITOP(name, type)						\
void									\
_atomic_and_##name(volatile type *p, type v)				\
{									\
	(void)__atomic_fetch_and(p, v, _MEMSEQ);			\
}									\
void									\
_atomic_or_##name(volatile type *p, type v)				\
{									\
	(void)__atomic_fetch_or(p, v, _MEMSEQ);				\
}									\
type									\
_atomic_and_##name##_nv(volatile type *p, type v)			\
{									\
	return __atomic_fetch_and(p, v, _MEMSEQ) & v;			\
}									\
type									\
_atomic_or_##name##_nv(volatile type *p, type v)			\
{									\
	return __atomic_fetch_or(p, v, _MEMSEQ) | v;			\
}

#define	_IMPL_ADDSUB(name, vtype, atype)				\
void									\
_atomic_add_##name(volatile vtype *p, atype v)				\
{									\
	(void)__atomic_fetch_add(p, (vtype)v, _MEMSEQ);			\
}									\
vtype									\
_atomic_add_##name##_nv(volatile vtype *p, atype v)			\
{									\
	return __atomic_fetch_add(p, (vtype)v, _MEMSEQ) + (vtype)v;	\
}									\
void									\
_atomic_dec_##name(volatile vtype *p)					\
{									\
	(void)__atomic_fetch_sub(p, (vtype)1, _MEMSEQ);			\
}									\
vtype									\
_atomic_dec_##name##_nv(volatile vtype *p)				\
{									\
	return __atomic_fetch_sub(p, (vtype)1, _MEMSEQ) - (vtype)1;	\
}									\
void									\
_atomic_inc_##name(volatile vtype *p)					\
{									\
	(void)__atomic_fetch_add(p, (vtype)1, _MEMSEQ);			\
}									\
vtype									\
_atomic_inc_##name##_nv(volatile vtype *p)				\
{									\
	return __atomic_fetch_add(p, (vtype)1, _MEMSEQ) + (vtype)1;	\
}

/*
 * The 32- and 64-bit fixed-width families.
 */
_IMPL_CAS(32, uint32_t)
_IMPL_CAS(64, uint64_t)
_IMPL_SWAP(32, uint32_t)
_IMPL_SWAP(64, uint64_t)
_IMPL_BITOP(32, uint32_t)
_IMPL_BITOP(64, uint64_t)
_IMPL_ADDSUB(32, uint32_t, int32_t)
_IMPL_ADDSUB(64, uint64_t, int64_t)

/*
 * Natural-width families.  On wasm32 these are aliases of the
 * fixed-width operations in spirit; implemented separately to match
 * the declared prototypes exactly (int/long are both 32-bit ILP32).
 */
_IMPL_CAS(uint, unsigned int)
_IMPL_CAS(ulong, unsigned long)
_IMPL_SWAP(uint, unsigned int)
_IMPL_SWAP(ulong, unsigned long)
_IMPL_BITOP(uint, unsigned int)
_IMPL_BITOP(ulong, unsigned long)
_IMPL_ADDSUB(int, unsigned int, int)
_IMPL_ADDSUB(long, unsigned long, long)

/*
 * Pointer family.  <sys/atomic.h> text-expands the pointer prototypes
 * to take "volatile void *"; re-cast internally to the true atomic
 * operand type, a pointer to a volatile void pointer.
 */
void *
_atomic_cas_ptr(volatile void *p, void *o, void *n)
{
	void *volatile *pp = (void *volatile *)p;
	void *expected = o;
	(void)__atomic_compare_exchange_n(pp, &expected, n, false,
	    _MEMSEQ, _MEMSEQ);
	return expected;
}

void *
_atomic_cas_ptr_ni(volatile void *p, void *o, void *n)
{
	void *old = _atomic_cas_ptr(p, o, n);
	return old == o ? n : old;
}

void *
_atomic_swap_ptr(volatile void *p, void *v)
{
	void *volatile *pp = (void *volatile *)p;
	return __atomic_exchange_n(pp, v, _MEMSEQ);
}

void
_atomic_add_ptr(volatile void *p, ssize_t v)
{
	void *volatile *pp = (void *volatile *)p;
	(void)__atomic_fetch_add(pp, v, _MEMSEQ);
}

void *
_atomic_add_ptr_nv(volatile void *p, ssize_t v)
{
	void *volatile *pp = (void *volatile *)p;
	return __atomic_fetch_add(pp, v, _MEMSEQ) + v;
}

void
_atomic_dec_ptr(volatile void *p)
{
	void *volatile *pp = (void *volatile *)p;
	(void)__atomic_fetch_sub(pp, 1, _MEMSEQ);
}

void *
_atomic_dec_ptr_nv(volatile void *p)
{
	void *volatile *pp = (void *volatile *)p;
	return __atomic_fetch_sub(pp, 1, _MEMSEQ) - 1;
}

void
_atomic_inc_ptr(volatile void *p)
{
	void *volatile *pp = (void *volatile *)p;
	(void)__atomic_fetch_add(pp, 1, _MEMSEQ);
}

void *
_atomic_inc_ptr_nv(volatile void *p)
{
	void *volatile *pp = (void *volatile *)p;
	return __atomic_fetch_add(pp, 1, _MEMSEQ) + 1;
}

/*
 * Narrow compare-exchange; these lower to the wasm sign-extending
 * rmw8/rmw16 cmpxchg instructions.
 */
uint16_t
_atomic_cas_16(volatile uint16_t *p, uint16_t o, uint16_t n)
{
	uint16_t expected = o;
	(void)__atomic_compare_exchange_n(p, &expected, n, false,
	    _MEMSEQ, _MEMSEQ);
	return expected;
}

uint8_t
_atomic_cas_8(volatile uint8_t *p, uint8_t o, uint8_t n)
{
	uint8_t expected = o;
	(void)__atomic_compare_exchange_n(p, &expected, n, false,
	    _MEMSEQ, _MEMSEQ);
	return expected;
}

/*
 * Public API: forwarding functions to the implementations above, so
 * that both the public spelling (used by MI code via <sys/atomic.h>)
 * and the underscored spelling (used by machine headers, cf.
 * x86/include/mutex.h) resolve at link time.
 */
#define	_PUB_CAS(name, type)						\
type									\
atomic_cas_##name(volatile type *p, type o, type n)			\
{									\
	return _atomic_cas_##name(p, o, n);				\
}									\
type									\
atomic_cas_##name##_ni(volatile type *p, type o, type n)		\
{									\
	return _atomic_cas_##name##_ni(p, o, n);			\
}

#define	_PUB_SWAP(name, type)						\
type									\
atomic_swap_##name(volatile type *p, type v)				\
{									\
	return _atomic_swap_##name(p, v);					\
}

#define	_PUB_BITOP(name, type)						\
void									\
atomic_and_##name(volatile type *p, type v)				\
{									\
	_atomic_and_##name(p, v);						\
}									\
void									\
atomic_or_##name(volatile type *p, type v)				\
{									\
	_atomic_or_##name(p, v);						\
}									\
type									\
atomic_and_##name##_nv(volatile type *p, type v)			\
{									\
	return _atomic_and_##name##_nv(p, v);				\
}									\
type									\
atomic_or_##name##_nv(volatile type *p, type v)				\
{									\
	return _atomic_or_##name##_nv(p, v);				\
}

#define	_PUB_ADDSUB(name, vtype, atype)				\
void									\
atomic_add_##name(volatile vtype *p, atype v)				\
{									\
	_atomic_add_##name(p, v);						\
}									\
vtype									\
atomic_add_##name##_nv(volatile vtype *p, atype v)			\
{									\
	return _atomic_add_##name##_nv(p, v);				\
}									\
void									\
atomic_dec_##name(volatile vtype *p)					\
{									\
	_atomic_dec_##name(p);						\
}									\
vtype									\
atomic_dec_##name##_nv(volatile vtype *p)				\
{									\
	return _atomic_dec_##name##_nv(p);				\
}									\
void									\
atomic_inc_##name(volatile vtype *p)					\
{									\
	_atomic_inc_##name(p);						\
}									\
vtype									\
atomic_inc_##name##_nv(volatile vtype *p)				\
{									\
	return _atomic_inc_##name##_nv(p);				\
}

_PUB_CAS(32, uint32_t)
_PUB_CAS(64, uint64_t)
_PUB_CAS(uint, unsigned int)
_PUB_CAS(ulong, unsigned long)
_PUB_SWAP(32, uint32_t)
_PUB_SWAP(64, uint64_t)
_PUB_SWAP(uint, unsigned int)
_PUB_SWAP(ulong, unsigned long)
_PUB_BITOP(32, uint32_t)
_PUB_BITOP(64, uint64_t)
_PUB_BITOP(uint, unsigned int)
_PUB_BITOP(ulong, unsigned long)
_PUB_ADDSUB(32, uint32_t, int32_t)
_PUB_ADDSUB(64, uint64_t, int64_t)
_PUB_ADDSUB(int, unsigned int, int)
_PUB_ADDSUB(long, unsigned long, long)

void
atomic_add_ptr(volatile void *p, ssize_t v)
{
	_atomic_add_ptr(p, v);
}

void *
atomic_add_ptr_nv(volatile void *p, ssize_t v)
{
	return _atomic_add_ptr_nv(p, v);
}

void
atomic_dec_ptr(volatile void *p)
{
	_atomic_dec_ptr(p);
}

void *
atomic_dec_ptr_nv(volatile void *p)
{
	return _atomic_dec_ptr_nv(p);
}

void
atomic_inc_ptr(volatile void *p)
{
	_atomic_inc_ptr(p);
}

void *
atomic_inc_ptr_nv(volatile void *p)
{
	return _atomic_inc_ptr_nv(p);
}

void *
atomic_cas_ptr(volatile void *p, void *o, void *n)
{
	return _atomic_cas_ptr(p, o, n);
}

void *
atomic_cas_ptr_ni(volatile void *p, void *o, void *n)
{
	return _atomic_cas_ptr_ni(p, o, n);
}

void *
atomic_swap_ptr(volatile void *p, void *v)
{
	return _atomic_swap_ptr(p, v);
}

uint16_t
atomic_cas_16(volatile uint16_t *p, uint16_t o, uint16_t n)
{
	return _atomic_cas_16(p, o, n);
}

uint8_t
atomic_cas_8(volatile uint8_t *p, uint8_t o, uint8_t n)
{
	return _atomic_cas_8(p, o, n);
}

void
membar_acquire(void)
{
	_membar_acquire();
}

void
membar_release(void)
{
	_membar_release();
}

void
membar_producer(void)
{
	_membar_producer();
}

void
membar_consumer(void)
{
	_membar_consumer();
}

void
membar_sync(void)
{
	_membar_sync();
}

void
membar_enter(void)
{
	_membar_enter();
}

void
membar_exit(void)
{
	_membar_exit();
}
