/*	$NetBSD$	*/

#ifndef	_WASM_CDEFS_H_
#define	_WASM_CDEFS_H_

/*
 * The wasm toolchain does not predefine __ELF__, but wasm-ld emits
 * ELF objects honouring .section attributes and NetBSD's MI headers
 * need the ELF branch (cdefs_elf.h: __read_mostly, link sets, ...)
 * to compile.  This file is included by sys/cdefs.h immediately
 * before it tests __ELF__, so it is the right place to correct for
 * that.
 */
#ifndef __ELF__
#define	__ELF__
#endif

/*
 * clang has no __builtin_return_address on bare wasm (no frame
 * unwinding; only Emscripten emulates it).  Every kernel use is
 * caller-identity for diagnostics (mutex init/abort, ktr, wchan
 * names), so substitute 0.
 */
#undef __builtin_return_address
#define	__builtin_return_address(x)	(0)

#define __ALIGNBYTES	(sizeof(int) - 1)

#endif /* !_WASM_CDEFS_H_ */
