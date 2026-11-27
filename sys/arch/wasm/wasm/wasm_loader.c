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
 *           wasm_user_svc() directly (a wasm function import, no JS
 *           trampoline), wiring env.memory to the shared linear
 *           memory, and storing the module's _start in the next slot
 *           of our exported function table; returns that slot.
 *   kernel: wasm_user_start(slot, argp) (wasm_callin.s) -> call_indirect
 *           through the table straight into _start(argp); the return
 *           value is the program's exit code.  Keeping this chain in
 *           wasm frames is what lets a blocking svc suspend the epoch
 *           via JSPI (a JS frame in between would trap).
 *   user:   svc(nr, argp) -> wasm_user_svc() here, directly.
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
 * The syscall ABI (v1) is the svc import below: NetBSD SYS_* numbers,
 * args as the matching sys_*_args struct in shared memory, straight
 * into the kernel's real sys_* implementations.  _start receives a
 * wasm_start_info block (argc/argv/envc/envp).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/syscall.h>
#include <sys/syscallargs.h>
#include <sys/vnode.h>

#include <lib/libkern/libkern.h>

/*
 * Host loader service (imports).  The host keeps instance handles
 * until unload; see wasmrun/run.mjs.
 */
int32_t	wasm_host_load_module(uint32_t, uint32_t)
	    __attribute__((import_module("host"), import_name("load_module")));
void	wasm_host_unload_module(int32_t)
	    __attribute__((import_module("host"), import_name("unload_module")));

/*
 * JSPI blocking support.  The host calls wasm_set_jspi(1) before
 * boot when it can suspend wasm stacks (WebAssembly.promising +
 * Suspending).  A syscall that would block then calls wasm_host_wait
 * (a Suspending import: it returns a Promise the host resolves when
 * input arrives, suspending this epoch) and retries.  Without the
 * flag, EWOULDBLOCK is returned to the guest as before.
 */
void	wasm_host_wait(void)
	    __attribute__((import_module("host"), import_name("wait")));

static int wasm_jspi;

int	wasm_set_jspi(int);
int
wasm_set_jspi(int v)
{
	wasm_jspi = v;
	return 0;
}

/* wasm_callin.s: call_indirect into the user module's _start */
int32_t	wasm_user_start(int32_t, uint32_t);

int	wasm_user_svc(int, int);
int	wasm_loader_load(struct vnode *, size_t, const char *);
void	wasm_loader_run(void);

/*
 * Startup info handed to _start as its single argument (wasm exports
 * take one i32): the classic argc/argv/envc/envp, with pointers as
 * u32 words into the shared linear memory.
 */
struct wasm_start_info {
	int32_t		argc;
	uint32_t	argv;	/* char *[argc + 1], NULL-terminated */
	int32_t		envc;
	uint32_t	envp;	/* char *[envc + 1], NULL-terminated */
};

static struct wasm_start_info wasm_si;
static char *wasm_argv[2];
static char *wasm_envp[1];
static char wasm_arg0[128];

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

static int32_t wasm_pending_slot = -1;

int
wasm_loader_load(struct vnode *vp, size_t size, const char *kname)
{
	uint8_t *buf;
	int32_t slot;
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

	slot = wasm_host_load_module((uint32_t)(uintptr_t)buf,
	    (uint32_t)size);
	kmem_free(buf, size);
	if (slot < 0)
		return EFTYPE;

	strlcpy(wasm_arg0, kname, sizeof(wasm_arg0));
	wasm_argv[0] = wasm_arg0;
	wasm_argv[1] = NULL;
	wasm_envp[0] = NULL;
	wasm_si.argc = 1;
	wasm_si.argv = (uint32_t)(uintptr_t)wasm_argv;
	wasm_si.envc = 0;
	wasm_si.envp = (uint32_t)(uintptr_t)wasm_envp;

	wasm_pending_slot = slot;
	return 0;
}

/*
 * Epoch entry for a real module (ep_entry round-trip; see
 * wasm_user_entry).  _start receives the startup info block.
 */
void
wasm_loader_run(void)
{
	int32_t slot, rv;

	if (wasm_pending_slot < 0) {
		printf("wasm: no module pending\n");
		return;
	}
	slot = wasm_pending_slot;
	wasm_pending_slot = -1;

	rv = wasm_user_start(slot, (uint32_t)(uintptr_t)&wasm_si);
	printf("wasm: module exited with %d\n", rv);
	wasm_host_unload_module(slot);
}

/*
 * svc: syscall entry for user modules (ABI v1).  The host wires this
 * as every user module's env.svc import.  Numbers are the real NetBSD
 * SYS_* numbers; argp points to the matching sys_*_args struct in the
 * shared linear memory (wasm32 words match syscallarg_t exactly).
 * The call goes straight into the kernel's sys_* implementation --
 * the same function a trapframe dispatch would reach -- so user
 * programs share the real VFS, fd table, and credentials.
 *
 * Deviation: SYS_exit on pid 1 would panic ("init died") in real
 * NetBSD; here it reboots into the host, matching what returning
 * from _start already does.
 *
 * Exported for the host (--export-all).
 */
void	wasm_user_reboot(void);		/* exec_wasm32.c */

int
wasm_user_svc(int nr, int argp)
{
	const void *uap = (const void *)(uintptr_t)argp;
	register_t rv = 0;
	struct lwp *l = curlwp;
	int error;

	/*
	 * Each case reinterprets argp as the specific args struct;
	 * the user side declares the identical layout.
	 */
	switch (nr) {
	case SYS_read:
		/*
		 * Blocking read: the console driver returns EWOULDBLOCK
		 * when its input ring is empty.  With JSPI, suspend the
		 * epoch in the host (which resolves when input arrives)
		 * and retry; the whole chain from the host's promising
		 * entry to wasm_host_wait is wasm frames, as JSPI
		 * requires.
		 */
		for (;;) {
			error = sys_read(l,
			    (const struct sys_read_args *)uap, &rv);
			if (error != EWOULDBLOCK || wasm_jspi == 0)
				break;
			wasm_host_wait();
		}
		break;
	case SYS_write:
		error = sys_write(l, (const struct sys_write_args *)uap, &rv);
		break;
	case SYS_open:
		error = sys_open(l, (const struct sys_open_args *)uap, &rv);
		break;
	case SYS_close:
		error = sys_close(l, (const struct sys_close_args *)uap, &rv);
		break;
	case SYS_exit:
		printf("wasm: init exited via SYS_exit\n");
		wasm_user_reboot();	/* not reached */
		/* FALLTHROUGH */
	default:
		return EINVAL;
	}

	return error != 0 ? -error : (int)rv;
}
