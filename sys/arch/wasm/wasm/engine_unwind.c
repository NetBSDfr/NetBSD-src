/*	$NetBSD$ */

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
 * 2. Redistributions in the binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
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
 * Guarded execution region for the WebAssembly port's run-to-completion
 * scheduler (see the DESIGN NOTE in lwp_csw.c).
 *
 * This translation unit is compiled as C++ (-x c++ in the build
 * machinery) because C has no syntax for the wasm exception-handling
 * try/catch; everything else in the kernel stays C.  The mechanism is
 * deliberately small:
 *
 *   wasm_engine_guarded(fn, arg) runs fn(arg) and returns 0 when fn
 *   returns normally, or 1 when wasm_engine_throw() unwound the engine
 *   frames below this one.  No C++ runtime is used beyond the
 *   catch-all landing pad; the two __cxa_* shims below are all the
 *   libc++abi surface the generated code references, and a raw tag
 *   exception carries no payload that anyone here inspects.
 *
 * Requirements: -fwasm-exceptions (the wasm exception-handling
 * proposal).  Both V8 (node, browsers) support it unconditionally.
 */

extern "C" void
wasm_engine_throw(void);
extern "C" int
wasm_engine_guarded(void (*fn)(void *), void *arg);
extern "C" void *
__cxa_begin_catch(void *exc);
extern "C" void
__cxa_end_catch(void);

/*
 * Unwind the engine stack back to the innermost (most deeply nested)
 * running wasm_engine_guarded() frame.  Must be called only from
 * inside a guarded region.
 */
extern "C" void
wasm_engine_throw(void)
{
	__builtin_wasm_throw(0, (void *)0);
}

extern "C" void *
__cxa_begin_catch(void *exc)
{
	return exc;
}

extern "C" void
__cxa_end_catch(void)
{
}

/*
 * Run fn(arg) under a protected region.  Returns 0 if fn returned
 * normally, 1 if wasm_engine_throw() unwound out of fn.
 */
extern "C" int
wasm_engine_guarded(void (*fn)(void *), void *arg)
{
	try {
		fn(arg);
		return 0;
	} catch (...) {
		return 1;
	}
}
