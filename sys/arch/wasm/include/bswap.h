/*	$NetBSD: bswap.h,v 1.1 2026-11-25 00:00:00 imil Exp $	*/

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

#ifndef	_WASM_BSWAP_H_
#define	_WASM_BSWAP_H_

#include <sys/stdint.h>

/*
 * WebAssembly has no byte-reversal instruction; clang lowers these
 * builtins to the usual shift/rotate sequence (and folds constants).
 * Providing the __BYTE_SWAP_U*_VARIABLE hooks keeps <sys/bswap.h>
 * from falling back to out-of-line bswap16/32/64 function calls.
 */

__BEGIN_DECLS

static __inline uint64_t
__wasm_byte_swap_u64(uint64_t v)
{
	return __builtin_bswap64(v);
}

static __inline uint32_t
__wasm_byte_swap_u32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static __inline uint16_t
__wasm_byte_swap_u16(uint16_t v)
{
	return __builtin_bswap16(v);
}

__END_DECLS

#define	__BYTE_SWAP_U64_VARIABLE __wasm_byte_swap_u64
#define	__BYTE_SWAP_U32_VARIABLE __wasm_byte_swap_u32
#define	__BYTE_SWAP_U16_VARIABLE __wasm_byte_swap_u16

#include <sys/bswap.h>

#endif /* !_WASM_BSWAP_H_ */
