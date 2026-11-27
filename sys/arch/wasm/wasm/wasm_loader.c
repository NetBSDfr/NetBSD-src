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
 * wasm_loader: host-mediated loader for wasm user modules.
 *
 * The kernel cannot compile or instantiate wasm by itself: that is a
 * host (embedder) privilege.  The loop is:
 *
 *   kernel: wasm_host_load_module(bytes) -> host compiles + instantiates
 *           the module, wiring its env.svc import to our exported
 *           wasm_user_svc(), and its env.memory to the shared linear
 *           memory; returns an opaque handle.
 *   kernel: wasm_host_call_start(handle, argp) -> host calls the
 *           module's _start(argp) export; the return value is the
 *           program's exit code.
 *   user:   svc(nr, argp) -> host trampoline -> wasm_user_svc() here.
 *
 * Shared memory (B1): user modules import the kernel's own linear
 * memory, so pointers pass unmodified across every boundary.  Data
 * isolation (per-instance memory) is a later phase.  Modules are
 * linked with --global-base/--stack-first inside the user window
 * (see rootimg.h) so their data and stack avoid the kernel's own
 * image, stack, and the exec'd argv area; uvm page allocation does
 * not reach that deep in practice yet, but the real carve-out comes
 * with per-instance memory.
 *
 * The syscall ABI (v1) is the svc import below: raw pointers, no
 * marshalling.  Only puts(1) exists so far; mapping nr onto real
 * kernel syscalls (write, exit, read) is the next step.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <lib/libkern/libkern.h>

/*
 * Host loader service (imports).  The host keeps instance handles
 * until unload; see wasmrun/run.mjs.
 */
int32_t	wasm_host_load_module(uint32_t, uint32_t)
	    __attribute__((import_module("host"), import_name("load_module")));
int32_t	wasm_host_call_start(int32_t, uint32_t)
	    __attribute__((import_module("host"), import_name("call_start")));
void	wasm_host_unload_module(int32_t)
	    __attribute__((import_module("host"), import_name("unload_module")));

int	wasm_user_svc(int, int);
int	wasm_loader_load(struct vnode *, size_t);
void	wasm_loader_run(void);

/*
 * Real-module support (used by exec_wasm32 for files that carry the
 * wasm magic rather than a WASMUSER stub).
 *
 * Loading happens at makecmds time while the vnode is still open;
 * the resulting handle is parked until the epoch enters the program.
 * One pending module at a time: this system execs init once and
 * never again, so a global is honest v1 scope.
 */
#define	WASM_MODULE_MAXSIZE	(8 * 1024 * 1024)

static int32_t wasm_pending_handle = -1;

int
wasm_loader_load(struct vnode *vp, size_t size)
{
	uint8_t *buf;
	int32_t handle;
	int error;

	if (size == 0 || size > WASM_MODULE_MAXSIZE)
		return EFTYPE;

	buf = kmem_alloc(size, KM_SLEEP);
	error = vn_rdwr(UIO_READ, vp, buf, size, (off_t)0, UIO_SYSSPACE,
	    IO_UNIT, kauth_cred_get(), NULL, NULL);
	if (error != 0) {
		kmem_free(buf, size);
		return error;
	}

	handle = wasm_host_load_module((uint32_t)(uintptr_t)buf,
	    (uint32_t)size);
	kmem_free(buf, size);
	if (handle < 0)
		return EFTYPE;

	wasm_pending_handle = handle;
	return 0;
}

/*
 * Epoch entry for a real module (ep_entry round-trip; see
 * wasm_user_entry).  argp is 0 for now: argv/envp delivery is the
 * next ABI step.
 */
void
wasm_loader_run(void)
{
	int32_t handle, rv;

	if (wasm_pending_handle < 0) {
		printf("wasm: no module pending\n");
		return;
	}
	handle = wasm_pending_handle;
	wasm_pending_handle = -1;

	rv = wasm_host_call_start(handle, 0);
	printf("wasm: module exited with %d\n", rv);
	wasm_host_unload_module(handle);
}

/*
 * svc: syscall entry for user modules (ABI v1).  The host wires this
 * as every user module's env.svc import; arguments are raw pointers
 * into the shared linear memory.  Exported for the host (--export-all).
 */
int
wasm_user_svc(int nr, int argp)
{

	switch (nr) {
	case 1:		/* puts(argp) */
		printf("%s", (const char *)(uintptr_t)argp);
		return 0;
	default:
		return EINVAL;
	}
}
