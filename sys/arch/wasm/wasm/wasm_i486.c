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
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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
 * wasm_i486: run unmodified NetBSD/i386 binaries under the in-kernel
 * i486 interpreter (emul_i486.c), iSH-style.
 *
 * E2/E3: real ELF32 EM_386 executables (including static-pie rescue
 * binaries) load into a kmem-allocated guest region at the standard
 * i386 link base (0x08048000): guest VA is region-relative (i486_h
 * maps hbase + va - vbase), so it is not a kernel address.  The
 * syscall bridge translates pointer slots to host addresses
 * (per-syscall mask table) before handing them to wasm_user_svc():
 * the svc path and uiomove() assume identity pointers (init's
 * vmspace is the kernel's, so copyin_vmspace() takes the kcopy
 * branch), and this keeps that true for guests.
 * int $0x80 ABI (lib/libc/arch/i386/sys/__syscall.S): number in eax
 * and also pushed at [esp]; args at esp+4 in the sys_*_args layout
 * (syscallarg_t == wasm32 word, syscall numbers are arch-common).
 *
 * Guest memory management: the region carries the image at the bottom,
 * a brk heap (break_cur) and a bump anonymous-mmap arena above it;
 * the initial stack and ps_strings sit at the top.  fork snapshots
 * the task (wasm_i486_snapshot) and execve reloads it in place.
 *
 * Register ABI on svc return (lib/libc/arch/i386/sys asm): EAX =
 * rval[0] or errno with CF, and EDX = rval[1] on every success
 * (sys/arch/x86/x86/syscall.c sets RDX = rval[1] unconditionally).
 * fork.S computes pid = EAX & (EDX - 1): parent must see EDX=0,
 * child EDX=1; pipe returns fd0/fd1 in EAX/EDX.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/exec_elf.h>
#include <sys/kmem.h>
#include <uvm/uvm_extern.h>
#include <lib/libkern/libkern.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/syscall.h>
#include <sys/syscallargs.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/timevar.h>

#include <arch/wasm/wasm/emul_i486.h>

#define	I486_REGION_SIZE	(28u << 20)
#define	I486_GUEST_BASE		0x08048000u	/* TEXTBASE, ld(1) i386 */
#define	I486_BRK_RESERVE	(4u << 20)
#define	I486_TLS_MAX		4096
#define	I486_ARGC_MAX		16
#define	I486_ENVC_MAX		16

/* keep in sync with wasm_loader.c */
#define	WASM_SYS_spawn		512
#define	WASM_SPAWN_ARGC_MAX	16

struct wasm_i486_task {
	struct i486_vm vm;		/* first member */
	uint32_t break_cur;		/* brk heap top (gva) */
	uint32_t break_max;
	uint32_t lwpctl_va;		/* guest lwpctl block */
	uint8_t	 tls_tmpl[I486_TLS_MAX];
	uint32_t tls_tmpl_sz;		/* filesz */
	uint32_t tls_static_sz;		/* round_up(memsz, 8) */
	int	 tls_copied;
};

extern int wasm_user_svc(int, int);
extern int wasm_child_spawn(const char *, char *const *);
extern int wasm_i486_fork(struct i486_vm *);
extern int wasm_read_file(const char *, void **, size_t *);
extern void wasm_clock_tick(void);

void	*wasm_i486_load(const void *, size_t, char *const *, char *const *);
int	wasm_i486_exec(void *);
void	wasm_i486_free(void *);
int	wasm_i486_reload(struct wasm_i486_task *, const void *, size_t,
	    char *const *, char *const *);
void	wasm_i486_snapshot(struct i486_vm *, struct i486_vm *);
void	*wasm_i486_task_alloc(void);

/* bounds-checked guest memory helpers */
static int
i486_rd32(struct i486_vm *vm, uint32_t va, uint32_t *out)
{
	uint8_t *p = i486_h(&vm->m, va);

	if (p == NULL)
		return EFAULT;
	memcpy(out, p, 4);
	return 0;
}

static int
i486_wr32(struct i486_vm *vm, uint32_t va, uint32_t v)
{
	uint8_t *p = i486_h(&vm->m, va);

	if (p == NULL)
		return EFAULT;
	memcpy(p, &v, 4);
	return 0;
}

static bool
i486_str(struct i486_vm *vm, uint32_t va, char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len - 1; i++) {
		uint8_t *p = i486_h(&vm->m, va + i);

		if (p == NULL)
			return false;
		buf[i] = (char)*p;
		if (buf[i] == 0)
			return true;
	}
	buf[0] = 0;
	return false;
}

/*
 * Pointer slots per syscall (bit i = arg slot i is a guest pointer),
 * from sys/sys/syscallargs.h.  Syscalls absent here take no pointers;
 * WASM_SYS_spawn is special-cased (argv is a pointer array).
 */
#define	I486_PTR(n)	(1u << (n))
static const uint8_t i486_ptrmask[513] = {
	[SYS_read]		= I486_PTR(1),
	[SYS_write]		= I486_PTR(1),
	[SYS_open]		= I486_PTR(0),
	[SYS_unlink]		= I486_PTR(0),
	[SYS_chdir]		= I486_PTR(0),
	[SYS_mkdir]		= I486_PTR(0),
	[SYS_rmdir]		= I486_PTR(0),
	[SYS_access]		= I486_PTR(0),
	[SYS_readlink]		= I486_PTR(0) | I486_PTR(1),
	[SYS___getcwd]		= I486_PTR(0),
	[SYS___getdents30]	= I486_PTR(1),
	[SYS___stat50]		= I486_PTR(0) | I486_PTR(1),
	[SYS___lstat50]		= I486_PTR(0) | I486_PTR(1),
	[SYS___fstat50]		= I486_PTR(1),
	[SYS___statvfs190]	= I486_PTR(0) | I486_PTR(1),
	[SYS___fstatvfs190]	= I486_PTR(1),
	/* wait6(idtype, id, status, options, rusage, rusage_info) */
	[SYS_wait6]		= I486_PTR(2) | I486_PTR(4) | I486_PTR(5),
	[SYS___wait450]		= I486_PTR(1) | I486_PTR(3),
	[SYS___sysctl]		= I486_PTR(0) | I486_PTR(2) |
				  I486_PTR(3) | I486_PTR(4),
	[SYS_getrlimit]		= I486_PTR(1),
	[SYS_setrlimit]		= I486_PTR(1),
	[SYS___sigprocmask14]	= I486_PTR(1) | I486_PTR(2),
	[SYS___clock_gettime50]	= I486_PTR(1),
	[SYS___gettimeofday50]	= I486_PTR(0) | I486_PTR(1),
	[SYS___nanosleep50]	= I486_PTR(0) | I486_PTR(1),
	[SYS_lpathconf]		= I486_PTR(0),
	[SYS_pread]		= I486_PTR(1),
	[SYS___acl_get_link]	= I486_PTR(0) | I486_PTR(2),
	/* __mount50(name, path, flags, data): the fstype name, mount point and
	 * fs-specific data are guest pointers; without translation the kernel
	 * reads them as host addresses and returns EINVAL (mount_tmpfs et al.
	 * are i486 binaries, unlike the wasm32 svc path where guest==host). */
	[SYS___mount50]		= I486_PTR(0) | I486_PTR(1) | I486_PTR(3),
	[SYS_unmount]		= I486_PTR(0),
};

static int i486_trace_nr;
static int i486_trace_budget;	/* >0: trace next N syscalls (debug aid) */
static int i486_trace_on;

static void
wasm_i486_ret2(struct i486_vm *vm, int rv, uint32_t rv1)
{

	if (i486_trace_on)
		printf("[svc %d pid %d] -> %d ebx=%08x\n", i486_trace_nr,
		    curlwp->l_proc->p_pid, rv,
		    vm->s.regs[R_EBX]);
	if (rv < 0) {
		vm->s.regs[R_EAX] = (uint32_t)(-rv);
		vm->s.eflags |= I486_CF;
		return;
	}
	vm->s.regs[R_EAX] = (uint32_t)rv;
	vm->s.regs[R_EDX] = rv1;	/* kernel sets RDX = rval[1] */
	vm->s.eflags &= ~I486_CF;
}

static void
wasm_i486_ret(struct i486_vm *vm, int rv)
{

	wasm_i486_ret2(vm, rv, 0);
}

/*
 * Guest brk heap: image end .. image end + reserve.
 */
static int
i486_brk(struct wasm_i486_task *t, uint32_t nb)
{

	nb = (nb + 0xfff) & ~0xfff;
	if (nb == 0)
		return (int)t->break_cur;
	if (nb < t->vm.img_end || nb > t->break_max)
		return -ENOMEM;
	if (nb > t->break_cur)
		memset(i486_h(&t->vm.m, t->break_cur), 0, nb - t->break_cur);
	t->break_cur = nb;
	return (int)nb;
}

/*
 * Guest anonymous mmap: bump allocator between the brk reserve and
 * the stack guard.  Hints and MAP_FIXED are ignored (the region is
 * private to this guest and the arena is fresh zeroed memory).
 * MAP_ALIGNED is honored: libc's startup stack mmap asks for a 4 MB
 * region at 2 MB alignment and rejects anything else.
 */
static int
i486_mmap(struct wasm_i486_task *t, uint32_t len, uint32_t flags)
{
	uint32_t va, al;
	unsigned lg = (flags >> 24) & 0xff;

	al = lg > 12 ? 1u << lg : 0x1000u;
	len = (len + 0xfff) & ~0xfff;
	va = (t->vm.mmap_cur + al - 1) & ~(al - 1);
	if (len == 0 || va + len > t->vm.mmap_end)
		return -ENOMEM;
	t->vm.mmap_cur = va + len;
	memset(i486_h(&t->vm.m, va), 0, len);
	return (int)va;
}

/*
 * execve: reload this task in place from the named binary.  argv and
 * envp are guest pointer arrays; strings are copied out before the
 * old image is replaced.  Returns 0 (guest continues as the new
 * program) or a negative errno (guest continues unchanged).
 */
static int
i486_execve(struct wasm_i486_task *t, const uint32_t *a)
{
	struct i486_vm *vm = &t->vm;
	char *kargv[I486_ARGC_MAX + 1], *kenvp[I486_ENVC_MAX + 1];
	char path[256];
	void *buf;
	size_t size;
	uint32_t p;
	int i, n, error;

	if (!i486_str(vm, a[0], path, sizeof(path)))
		return -ENAMETOOLONG;
	n = 0;
	if (a[1] != 0) {
		for (; n < I486_ARGC_MAX; n++) {
			if (i486_rd32(vm, a[1] + 4 * n, &p) != 0 || p == 0)
				break;
			kargv[n] = kmem_alloc(256, KM_SLEEP);
			if (!i486_str(vm, p, kargv[n], 256)) {
				kargv[n][0] = 0;
			}
		}
	}
	kargv[n] = NULL;
	n = 0;
	if (a[2] != 0) {
		for (; n < I486_ENVC_MAX; n++) {
			if (i486_rd32(vm, a[2] + 4 * n, &p) != 0 || p == 0)
				break;
			kenvp[n] = kmem_alloc(256, KM_SLEEP);
			if (!i486_str(vm, p, kenvp[n], 256))
				kenvp[n][0] = 0;
		}
	}
	kenvp[n] = NULL;

	error = wasm_read_file(path, &buf, &size);
	if (error != 0)
		return -error;
	error = wasm_i486_reload(t, buf, size, kargv, kenvp);
	kmem_free(buf, size);
	for (i = 0; kargv[i] != NULL; i++)
		kmem_free(kargv[i], 256);
	for (i = 0; kenvp[i] != NULL; i++)
		kmem_free(kenvp[i], 256);
	return error;		/* 0 or negative errno from reload */
}

/* Host-driven timed sleep: suspends the epoch for usec microseconds
 * (JSPI Suspending import; see wasmrun/run.mjs, webboot kernel_worker.js). */
void	wasm_host_sleep(uint32_t)
    __attribute__((import_module("host"), import_name("sleep")));

static void
wasm_i486_syscall(struct i486_vm *vm)
{
	uint32_t esp = vm->s.regs[R_ESP];
	struct wasm_i486_task *t = (struct wasm_i486_task *)vm;
	uint32_t args[8];
	char *kargv[WASM_SPAWN_ARGC_MAX + 1];
	int nr = (int)vm->s.regs[R_EAX];
	uint8_t mask;
	uint32_t *ap, *av;
	int rv, i;

	/* Keep the host-backed clock's timehands fresh (see clock.c). */
	wasm_clock_tick();

	i486_trace_nr = nr;
	i486_trace_on = (i486_trace_budget-- > 200);
	if (i486_trace_on) {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);

		printf("[svc %d pid %d PRE] %08x %08x %08x %08x\n", nr,
		    curlwp->l_proc->p_pid,
		    w != NULL ? w[0] : 0, w != NULL ? w[1] : 0,
		    w != NULL ? w[2] : 0, w != NULL ? w[3] : 0);
	}

	/*
	 * WASM_SYS_spawn { path, argv }: both are guest pointers and
	 * argv is an array of guest string pointers.  The parent is
	 * frozen while the child is spawned and runs, so the strings
	 * can be read in place from the guest region; only the
	 * pointers need translating.
	 */
	if (nr == WASM_SYS_spawn) {
		char *path;
		int n = 0;

		ap = (uint32_t *)i486_h(&vm->m, esp + 4);
		if (ap == NULL)
			goto efault;
		path = (char *)i486_h(&vm->m, ap[0]);
		if (path == NULL)
			goto efault;
		if (ap[1] != 0) {
			av = (uint32_t *)i486_h(&vm->m, ap[1]);
			if (av == NULL)
				goto efault;
			for (; n < WASM_SPAWN_ARGC_MAX && av[n] != 0; n++) {
				kargv[n] = (char *)i486_h(&vm->m, av[n]);
				if (kargv[n] == NULL)
					goto efault;
			}
		}
		kargv[n] = NULL;
		wasm_i486_ret(vm, wasm_child_spawn(path, n > 0 ? kargv : NULL));
		return;
	}

	/*
	 * Guest-memory-management and process-model syscalls served
	 * here rather than by the kernel's sys_* (the guest address
	 * space is this region, not a vmspace).
	 */
	switch (nr) {
	case SYS_break: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		if (w == NULL)
			goto efault;
		wasm_i486_ret(vm, i486_brk(t, w[0]));
		return;
	}
	case SYS_mmap: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		if (w == NULL)
			goto efault;
		rv = i486_mmap(t, w[1], w[3]);
		wasm_i486_ret(vm, rv);
		return;
	}
	case SYS_munmap: case SYS_madvise: case SYS_mprotect:
	case SYS___msync13: case SYS___posix_fadvise50:
		wasm_i486_ret(vm, 0);
		return;
	case SYS__lwp_setprivate: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		if (w == NULL)
			goto efault;
		vm->tls_base = w[0];
		if (!t->tls_copied && t->tls_static_sz != 0 &&
		    t->tls_static_sz <= I486_TLS_MAX &&
		    i486_h(&vm->m, w[0] - t->tls_static_sz) != NULL) {
			memset(i486_h(&vm->m, w[0] - t->tls_static_sz), 0,
			    t->tls_static_sz);
			memcpy(i486_h(&vm->m, w[0] - t->tls_static_sz),
			    t->tls_tmpl, t->tls_tmpl_sz);
			t->tls_copied = 1;
		}
		wasm_i486_ret(vm, 0);
		return;
	}
	case SYS__lwp_ctl: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		if (w == NULL)
			goto efault;
		if ((w[0] & 1) == 0) {
			wasm_i486_ret(vm, -EOPNOTSUPP);
			return;
		}
		if (t->lwpctl_va == 0) {
			rv = i486_mmap(t, 8, 0);
			if (rv < 0) {
				wasm_i486_ret(vm, rv);
				return;
			}
			t->lwpctl_va = (uint32_t)rv;
		}
		if (i486_wr32(vm, w[1], t->lwpctl_va) != 0)
			goto efault;
		wasm_i486_ret(vm, 0);
		return;
	}
	case SYS__lwp_self:
		wasm_i486_ret(vm, (int)curlwp->l_proc->p_pid);
		return;
	case SYS___sigprocmask14: case SYS_compat_16___sigaction14:
	case SYS___sigaltstack14: case SYS_getcontext:
	case SYS_setcontext: case SYS___sigsuspend14:
	case SYS____lwp_park60: case SYS_compat_50__lwp_park:
	case SYS__lwp_kill: case SYS_sysarch:
	case SYS___sigaction_sigtramp:
	case SYS__lwp_unpark_all:
		wasm_i486_ret(vm, 0);
		return;
	case SYS__lwp_getprivate:
		wasm_i486_ret(vm, (int)vm->tls_base);
		return;
	case SYS___sigpending14: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		uint8_t *set;

		if (w == NULL)
			goto efault;
		set = w[0] != 0 ? i486_h(&vm->m, w[0]) : NULL;
		if (set != NULL)
			memset(set, 0, 128);
		wasm_i486_ret(vm, 0);
		return;
	}
	case SYS_ioctl: {
		/* nullcons has no termios: fake a sane tty like the
		 * native harness does (guest termios is 44 bytes) */
		static const uint32_t tios[9] = {
		    0x00000700u, 0x00000002u, 0x00000117u, 0x00008887u,
		    0, 0, 0, 0, 17 /* ispeed */ };
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		uint8_t *p;

		if (w == NULL)
			goto efault;
		p = w[2] != 0 ? i486_h(&vm->m, w[2]) : NULL;
		switch (w[1]) {
		case 0x402c7413u:		/* TIOCGETA */
			if (p == NULL) {
				wasm_i486_ret(vm, EINVAL);
				return;
			}
			memset(p, 0, 44);
			memcpy(p, tios, 16);
			p[16 + 5] = 4;	/* VEOF ^D */
			p[16 + 2] = 0x7f;	/* VERASE */
			p[16 + 3] = 0x15;	/* VKILL */
			p[16 + 1] = 3;	/* VINTR ^C */
			memcpy(p + 36, &tios[8], 4);	/* ispeed */
			memcpy(p + 40, &tios[8], 4);	/* ospeed */
			break;
		case 0x40047477u:		/* TIOCGPGRP */
			if (p != NULL) {
				uint32_t pg = 2;
				memcpy(p, &pg, 4);
			}
			break;
		default:
			break;	/* generous: TIOCSETA, GWINSZ, SPGRP ok */
		}
		wasm_i486_ret(vm, 0);
		return;
	}
	case SYS_issetugid:
		wasm_i486_ret(vm, 0);
		return;
	case SYS_pipe: {
		register_t rvs[2] = { 0, 0 };

		rv = -sys_pipe(curlwp, NULL, rvs);
		if (rv != 0) {
			wasm_i486_ret(vm, rv);
			return;
		}
		/* pipe.S stores both fds from EAX/EDX */
		wasm_i486_ret2(vm, (int)rvs[0], (uint32_t)rvs[1]);
		return;
	}
	case SYS_fork: case SYS___vfork14: case SYS___clone:
		wasm_i486_ret(vm, wasm_i486_fork(vm));
		return;
	case SYS_execve: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		if (w == NULL)
			goto efault;
		for (i = 0; i < 3; i++)
			args[i] = w[i];
		wasm_i486_ret(vm, i486_execve(t, args));
		return;
	}
	/*
	 * i386 packs 64-bit fields at 4-byte alignment: timespec50/
	 * timeval50 are 12 bytes here but 16 on wasm32, and struct
	 * stat50 differs too.  Serve these through local buffers and
	 * pack the i386 layout by hand (like the native harness).
	 */
	case SYS___clock_gettime50: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		uint8_t buf50[16];
		uint8_t *p;

		if (w == NULL || w[1] == 0)
			goto efault;
		p = i486_h(&vm->m, w[1]);
		if (p == NULL)
			goto efault;
		args[0] = w[0];
		args[1] = (uint32_t)(uintptr_t)buf50;
		rv = wasm_user_svc(nr, (int)(uintptr_t)args);
		if (rv == 0) {
			memcpy(p, buf50, 8);		/* tv_sec i64 */
			memcpy(p + 8, buf50 + 8, 4);	/* tv_nsec u32 */
		}
		wasm_i486_ret(vm, rv);
		return;
	}
	case SYS___gettimeofday50: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		uint8_t buf50[16];
		uint8_t *p;

		if (w == NULL || w[0] == 0)
			goto efault;
		p = i486_h(&vm->m, w[0]);
		if (p == NULL)
			goto efault;
		args[0] = (uint32_t)(uintptr_t)buf50;
		args[1] = 0;
		rv = wasm_user_svc(nr, (int)(uintptr_t)args);
		if (rv == 0) {
			memcpy(p, buf50, 8);		/* tv_sec i64 */
			memcpy(p + 8, buf50 + 8, 4);	/* tv_usec u32 */
		}
		wasm_i486_ret(vm, rv);
		return;
	}
	case SYS___nanosleep50: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		uint8_t req[16], rem[16];
		uint8_t *g;

		if (w == NULL)
			goto efault;
		if (w[0] == 0) {
			wasm_i486_ret(vm, EFAULT);
			return;
		}
		g = i486_h(&vm->m, w[0]);
		if (g == NULL)
			goto efault;
		memset(req, 0, sizeof(req));
		memcpy(req, g, 12);
		args[0] = (uint32_t)(uintptr_t)req;
		if (w[1] != 0) {
			g = i486_h(&vm->m, w[1]);
			if (g == NULL)
				goto efault;
			memset(rem, 0, sizeof(rem));
			args[1] = (uint32_t)(uintptr_t)rem;
		} else
			args[1] = 0;
		rv = wasm_user_svc(nr, (int)(uintptr_t)args);
		if (rv == 0 && w[1] != 0)
			memcpy(g, rem, 12);
		wasm_i486_ret(vm, rv);
		return;
	}
	case SYS_clock_nanosleep: {
		/* clock_nanosleep(clock_id, flags, rqtp, rmtp): the wasm
		 * kernel has no timer interrupt, so the sleep is delegated
		 * to the host (JSPI Suspending import).  flags &
		 * TIMER_ABSTIME means rqtp is an absolute CLOCK_MONOTONIC
		 * deadline; convert it to a relative duration against the
		 * monotonic clock before handing it to the host. */
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		uint8_t *rq;
		struct timespec now, ts;
		uint64_t sec, nsec, usec;

		if (w == NULL || w[2] == 0)
			goto efault;
		rq = i486_h(&vm->m, w[2]);
		if (rq == NULL)
			goto efault;
		sec = (uint64_t)rq[0] | (uint64_t)rq[1] << 8 |
		    (uint64_t)rq[2] << 16 | (uint64_t)rq[3] << 24;
		nsec = (uint64_t)rq[4] | (uint64_t)rq[5] << 8 |
		    (uint64_t)rq[6] << 16 | (uint64_t)rq[7] << 24;
		ts.tv_sec = (time_t)sec;
		ts.tv_nsec = (long)nsec;
		if (w[1] & TIMER_ABSTIME) {
			nanouptime(&now);
			if (timespeccmp(&ts, &now, >))
				timespecsub(&ts, &now, &ts);
			else
				timespecclear(&ts);
		}
		usec = (uint64_t)ts.tv_sec * 1000000u +
		    (uint64_t)ts.tv_nsec / 1000u;
		wasm_host_sleep((uint32_t)usec);
		wasm_i486_ret(vm, 0);
		return;
	}
	case SYS___stat50: case SYS___lstat50: case SYS___fstat50: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		struct stat st;
		uint8_t b[124];
		uint8_t *p;

		if (w == NULL)
			goto efault;
		args[0] = w[0];
		if (nr != SYS___fstat50 && w[0] != 0) {
			args[0] = (uint32_t)(uintptr_t)
			    i486_h(&vm->m, w[0]);
			if (args[0] == 0)
				goto efault;
		}
		p = w[1] != 0 ? i486_h(&vm->m, w[1]) : NULL;
		if (p == NULL)
			goto efault;
		args[1] = (uint32_t)(uintptr_t)&st;
		rv = wasm_user_svc(nr, (int)(uintptr_t)args);
		if (rv == 0) {
#define	PUT(off, val, sz) do { \
	    uint64_t v_ = (uint64_t)(val); int j_; \
	    for (j_ = 0; j_ < (sz); j_++) \
		    b[(off) + j_] = v_ >> (8 * j_); \
	} while (0)
			memset(b, 0, sizeof(b));
			PUT(0, st.st_dev, 8);
			PUT(8, st.st_mode, 4);
			PUT(12, st.st_ino, 8);
			PUT(20, st.st_nlink, 4);
			PUT(24, st.st_uid, 4);
			PUT(28, st.st_gid, 4);
			PUT(32, st.st_rdev, 8);
			PUT(40, st.st_atim.tv_sec, 8);
			PUT(48, st.st_atim.tv_nsec, 4);
			PUT(52, st.st_mtim.tv_sec, 8);
			PUT(60, st.st_mtim.tv_nsec, 4);
			PUT(64, st.st_ctim.tv_sec, 8);
			PUT(72, st.st_ctim.tv_nsec, 4);
			PUT(88, st.st_size, 8);
			PUT(96, st.st_blocks, 8);
			PUT(104, st.st_blksize, 4);
#undef	PUT
			memcpy(p, b, sizeof(b));
		}
		wasm_i486_ret(vm, rv);
		return;
	}
	case SYS___fstatvfs190: {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4);
		struct statvfs ksv;
		size_t head = offsetof(struct statvfs, f_spare);
		uint8_t *dst;

		if (w == NULL || w[1] == 0 ||
		    (dst = i486_h(&vm->m, w[1])) == NULL)
			goto efault;
		/*
		 * i386 SysV aligns 64-bit fields to 4 bytes; the wasm32
		 * kernel aligns them to 8.  The layouts are identical up
		 * to the 4-byte padding hole before f_spare; every field
		 * after it sits 4 bytes lower in the guest struct.  Run
		 * the syscall into a kernel-local struct, then pack it
		 * into the guest buffer (3268 bytes, not 3272).
		 */
		args[0] = w[0];
		args[1] = (uint32_t)(uintptr_t)&ksv;
		rv = wasm_user_svc(nr, (int)(uintptr_t)args);
		if (rv == 0) {
			memcpy(dst, &ksv, head - 4);
			memcpy(dst + head - 4, (uint8_t *)&ksv + head,
			    sizeof(ksv) - head);
		}
		wasm_i486_ret(vm, rv);
		return;
	}
	default:
		break;
	}

	mask = (nr >= 0 && nr <= 512) ? i486_ptrmask[nr] : 0;
	if (mask == 0) {
		uint8_t *sp = i486_h(&vm->m, esp);

		if (sp == NULL)
			goto efault;
		rv = wasm_user_svc(nr, (int)(uintptr_t)(sp + 4));
		wasm_i486_ret(vm, rv);
		return;
	}

	/*
	 * Copy all 8 slots, not just up to the highest pointer arg:
	 * scalars above it (write's len, getcwd's buflen) are part of
	 * the kernel's args struct and must not be left uninitialized.
	 */
	for (i = 0; i < 8; i++) {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4 + 4 * i);

		args[i] = w != NULL ? *w : 0;
	}
	for (i = 0; i < 8; i++) {
		if ((mask & I486_PTR(i)) == 0 || args[i] == 0)
			continue;
		args[i] = (uint32_t)(uintptr_t)i486_h(&vm->m, args[i]);
		if (args[i] == 0)
			goto efault;
	}
	rv = wasm_user_svc(nr, (int)(uintptr_t)args);
	wasm_i486_ret(vm, rv);
	return;

efault:
	vm->s.regs[R_EAX] = EFAULT;
	vm->s.eflags |= I486_CF;
}

/*
 * Build the initial guest stack: argc, argv[], NULL, envp[], NULL,
 * auxv; argv/envp strings above; struct psstrings at the very top
 * (passed in %ebx, %edx = cleanup = NULL).  Returns 0 or ENOMEM.
 */
static int
i486_build_stack(struct wasm_i486_task *t, char *const *argv,
    char *const *envp, uint32_t phdr_va, uint32_t entry, uint16_t phnum)
{
	struct i486_mem *m = &t->vm.m;
	uint32_t strp[I486_ARGC_MAX], estrp[I486_ENVC_MAX];
	uint32_t q[64];
	uint32_t *qp;
	char *p;
	int argc, ne, n, i;

	argc = 0;
	if (argv != NULL)
		while (argc < I486_ARGC_MAX && argv[argc] != NULL)
			argc++;
	ne = 0;
	if (envp != NULL)
		while (ne < I486_ENVC_MAX && envp[ne] != NULL)
			ne++;

	p = (char *)m->hbase + m->size - 64;	/* top 64B: ps_strings */
	for (i = argc - 1; i >= 0; i--) {
		size_t l = strlen(argv[i]) + 1;

		if (p - l < (char *)m->hbase)
			return ENOMEM;
		p -= l;
		memcpy(p, argv[i], l);
		strp[i] = m->vbase + (uint32_t)(p - (char *)m->hbase);
	}
	for (i = ne - 1; i >= 0; i--) {
		size_t l = strlen(envp[i]) + 1;

		if (p - l < (char *)m->hbase)
			return ENOMEM;
		p -= l;
		memcpy(p, envp[i], l);
		estrp[i] = m->vbase + (uint32_t)(p - (char *)m->hbase);
	}

	n = 0;
	q[n++] = argc;
	for (i = 0; i < argc; i++)
		q[n++] = strp[i];
	q[n++] = 0;				/* argv NULL */
	for (i = 0; i < ne; i++)
		q[n++] = estrp[i];
	q[n++] = 0;				/* envp NULL */
	q[n++] = 3;			q[n++] = phdr_va;	/* AT_PHDR */
	q[n++] = 4;			q[n++] = 32;	/* AT_PHENT */
	q[n++] = 5;			q[n++] = phnum;	/* AT_PHNUM */
	q[n++] = 7;			q[n++] = 0;	/* AT_BASE */
	q[n++] = 9;			q[n++] = entry;	/* AT_ENTRY */
	q[n++] = 6;			q[n++] = 4096;	/* AT_PAGESZ */
	q[n++] = 11;			q[n++] = m->vbase + m->size;
							/* AT_STACKBASE */
	q[n++] = 0;			q[n++] = 0;	/* AT_NULL */

	qp = (uint32_t *)(((uintptr_t)p - n * 4) & ~(uintptr_t)15);
	if ((uint8_t *)qp < m->hbase)
		return ENOMEM;
	memcpy(qp, q, n * 4);

	{
		uint32_t qva = m->vbase + (uint32_t)((uint8_t *)qp - m->hbase);
		uint32_t psstr = (m->vbase + m->size - 32) & ~(uint32_t)15;
		uint32_t *ps = (uint32_t *)((char *)m->hbase +
		    (psstr - m->vbase));

		ps[0] = qva + 4;		/* ps_argvstr */
		ps[1] = argc;			/* ps_nargvstr */
		ps[2] = qva + 4 + 4 * (argc + 1); /* ps_envstr */
		ps[3] = ne;			/* ps_nenvstr */

		t->vm.s.regs[R_ESP] = qva;
		t->vm.s.regs[R_EBX] = psstr;	/* crt0: ps_strings */
		t->vm.s.regs[R_EDX] = 0;	/* crt0: cleanup */
	}
	t->vm.s.regs[R_EAX] = 0;
	t->vm.s.regs[R_EBP] = 0;
	t->vm.s.eip = entry;
	t->vm.s.eflags = 0;
	return 0;
}

/*
 * Map the phdr table VA: the PT_LOAD segment covering e_phoff.
 */
static uint32_t
i486_phdr_va(const Elf32_Ehdr *eh, const Elf32_Phdr *ph)
{
	int i;

	for (i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD &&
		    eh->e_phoff >= ph[i].p_offset &&
		    eh->e_phoff < ph[i].p_offset + ph[i].p_filesz)
			return ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
	return 0;
}

/*
 * Load image segments + TLS template into the task's region and set
 * up the initial stack.  Shared by load and execve-reload.
 */
static int
i486_setup(struct wasm_i486_task *t, const void *image, size_t size,
    char *const *argv, char *const *envp)
{
	const Elf32_Ehdr *eh = image;
	const Elf32_Phdr *ph;
	uint8_t *region = t->vm.m.hbase;
	uint32_t maxend = 0;
	uint32_t phdr_va, entry;
	/* static-pie (ET_DYN) images link at 0: bias them into the region */
	const uint32_t load_bias =
	    (eh->e_type == ET_DYN) ? I486_GUEST_BASE : 0;
	int i;

	ph = (const Elf32_Phdr *)((const uint8_t *)image + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++, ph++) {
		uint32_t va;

		if (ph->p_type != PT_LOAD)
			continue;
		va = ph->p_vaddr + load_bias;
		if (va < I486_GUEST_BASE ||
		    va + ph->p_memsz >
		    I486_GUEST_BASE + I486_REGION_SIZE ||
		    ph->p_offset + ph->p_filesz > size ||
		    ph->p_filesz > ph->p_memsz)
			return EFTYPE;
		memcpy(region + (va - I486_GUEST_BASE),
		    (const uint8_t *)image + ph->p_offset, ph->p_filesz);
		if (va + ph->p_memsz > maxend)
			maxend = va + ph->p_memsz;
	}
	ph = (const Elf32_Phdr *)((const uint8_t *)image + eh->e_phoff);
	t->tls_tmpl_sz = 0;
	t->tls_static_sz = 0;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_TLS)
			continue;
		t->tls_static_sz = (ph[i].p_memsz + 7) & ~(uint32_t)7;
		t->tls_tmpl_sz = ph[i].p_filesz > I486_TLS_MAX ?
		    I486_TLS_MAX : ph[i].p_filesz;
		memcpy(t->tls_tmpl, (const uint8_t *)image + ph[i].p_offset,
		    t->tls_tmpl_sz);
	}
	t->tls_copied = 0;

	phdr_va = i486_phdr_va(eh, ph);
	if (phdr_va != 0)
		phdr_va += load_bias;
	entry = eh->e_entry + load_bias;

	t->vm.img_end = (maxend + 0xfff) & ~(uint32_t)0xfff;
	t->break_cur = t->vm.img_end;
	t->break_max = t->vm.img_end + I486_BRK_RESERVE;
	if (t->break_max > t->vm.m.vbase + t->vm.m.size - (2u << 20))
		t->break_max = t->vm.m.vbase + t->vm.m.size - (2u << 20);
	t->vm.mmap_cur = t->break_max;
	t->vm.mmap_end = t->vm.m.vbase + t->vm.m.size - (2u << 20);
	t->lwpctl_va = 0;

	if (i486_build_stack(t, argv, envp, phdr_va, entry, eh->e_phnum) != 0)
		return ENOMEM;
	if (i486_h(&t->vm.m, entry) == NULL)
		return EFTYPE;
	/* verify the copied segments survived the module read */
	ph = (const Elf32_Phdr *)((const uint8_t *)image + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		uint32_t va = ph[i].p_vaddr + load_bias;

		if (ph[i].p_type == PT_LOAD && ph[i].p_filesz != 0 &&
		    memcmp(region + (va - I486_GUEST_BASE),
		    (const uint8_t *)image + ph[i].p_offset,
		    ph[i].p_filesz) != 0) {
			printf("i486: SEG %d MISMATCH after load\n", i);
			return EFTYPE;
		}
	}
	return 0;
}

static int
i486_check_elf(const void *image, size_t size)
{
	const Elf32_Ehdr *eh = image;

	if (size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB ||
	    (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
	    eh->e_machine != EM_386)
		return EINVAL;
	if (eh->e_phnum == 0 || eh->e_phnum > 16 ||
	    eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > size)
		return EFTYPE;
	return 0;
}

/*
 * Load an ELF32 EM_386 executable into a fresh guest region.
 * Returns the task handle or NULL (not an i386 ELF, or does not fit).
 */
void *
wasm_i486_load(const void *image, size_t size, char *const *argv,
    char *const *envp)
{
	struct wasm_i486_task *t;
	uint8_t *region;

	if (i486_check_elf(image, size) != 0)
		return NULL;

	region = kmem_alloc(I486_REGION_SIZE, KM_SLEEP);
	if (region == NULL)
		return NULL;
	printf("i486: load region %u MB at %p\n", I486_REGION_SIZE >> 20,
	    (void *)region);
	memset(region, 0, I486_REGION_SIZE);

	t = kmem_zalloc(sizeof(*t), KM_SLEEP);
	t->vm.m.hbase = region;
	t->vm.m.vbase = I486_GUEST_BASE;
	t->vm.m.size = I486_REGION_SIZE;
	t->vm.syscall = wasm_i486_syscall;

	if (i486_setup(t, image, size, argv, envp) != 0) {
		kmem_free(region, I486_REGION_SIZE);
		kmem_free(t, sizeof(*t));
		return NULL;
	}
	printf("i486: loaded entry=%#x esp=%#x, %zu bytes image\n",
	    t->vm.s.eip, t->vm.s.regs[R_ESP], size);
	return t;
}

/*
 * execve support: reload an existing task in place.
 */
int
wasm_i486_reload(struct wasm_i486_task *t, const void *image, size_t size,
    char *const *argv, char *const *envp)
{
	int error;

	error = i486_check_elf(image, size);
	if (error != 0)
		return -error;
	memset(t->vm.m.hbase, 0, I486_REGION_SIZE);
	/* fresh process state: the harness resets these too; stale
	 * eflags/TLS/segment bases from the old image derail the
	 * static-pie startup (it branches on flags and relocates
	 * through %gs before crt0 sets anything up) */
	t->vm.s.eflags = 0;
	t->vm.s.regs[R_EAX] = 0;
	t->vm.s.regs[R_EBP] = 0;
	t->vm.tls_base = 0;
	t->vm.seg = 0;
	t->vm.dbg_steps = 0;
	error = i486_setup(t, image, size, argv, envp);
	if (error != 0)
		return -error;
	printf("i486: execve reload entry=%#x esp=%#x\n",
	    t->vm.s.eip, t->vm.s.regs[R_ESP]);
	return 0;
}

/*
 * fork support: snapshot the parent task into an already-prepared
 * child task (caller allocated region + task struct); the child
 * resumes just after the svc with EAX=0, EDX=1 (fork.S convention).
 */
void
wasm_i486_snapshot(struct i486_vm *parent, struct i486_vm *child)
{
	uint8_t *chbase = child->m.hbase;

	memcpy(child, parent, sizeof(struct wasm_i486_task));
	child->m.hbase = chbase;	/* struct copy clobbered it */
	memcpy(chbase, parent->m.hbase, I486_REGION_SIZE);
	child->syscall = wasm_i486_syscall;
	child->s.regs[R_EAX] = 0;
	child->s.regs[R_EDX] = 1;	/* fork.S: pid = EAX & (EDX-1) */
	child->steps = 0;
}

/*
 * Allocate an empty child task (region + struct) for a snapshot.
 */
void *
wasm_i486_task_alloc(void)
{
	struct wasm_i486_task *t;
	uint8_t *region;

	region = kmem_alloc(I486_REGION_SIZE, KM_NOSLEEP);
	if (region == NULL)
		return NULL;
	memset(region, 0, I486_REGION_SIZE);
	t = kmem_zalloc(sizeof(*t), KM_SLEEP);
	t->vm.m.hbase = region;
	t->vm.m.vbase = I486_GUEST_BASE;
	t->vm.m.size = I486_REGION_SIZE;
	return t;
}

/*
 * Run the guest to completion.  SYS_exit unwinds out through this
 * frame via wasm_engine_throw (the svc child path), caught by the
 * guarded slice in wasm_run_child; returning here means hlt.
 */
int
wasm_i486_exec(void *arg)
{
	struct wasm_i486_task *t = arg;
	int rv = i486_run(&t->vm, 0);

	if (rv != I486_OK) {
		uint8_t *p = i486_h(&t->vm.m, t->vm.s.eip);
		int bj;

		printf("i486: fault rv=%d eip=%08x (op %02x)\n", rv,
		    t->vm.s.eip, p != NULL ? *p : 0xffu);
		{
			uint8_t *q = i486_h(&t->vm.m, t->vm.s.eip - 8);

			if (q != NULL) {
				printf("i486: bytes@eip-8:");
				for (bj = 0; bj < 24; bj++)
					printf(" %02x", q[bj]);
				printf("\n");
			}
		}
		printf("i486: eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
		    t->vm.s.regs[R_EAX], t->vm.s.regs[R_EBX],
		    t->vm.s.regs[R_ECX], t->vm.s.regs[R_EDX]);
		printf("i486: esi=%08x edi=%08x esp=%08x ebp=%08x efl=%08x\n",
		    t->vm.s.regs[R_ESI], t->vm.s.regs[R_EDI],
		    t->vm.s.regs[R_ESP], t->vm.s.regs[R_EBP],
		    t->vm.s.eflags);
		{
			uint8_t *sp = i486_h(&t->vm.m,
			    t->vm.s.regs[R_ESP] - 0x20);
			int j;

			if (sp != NULL) {
				printf("i486: stk@esp-20:");
				for (j = 0; j < 0x30; j++)
					printf(" %02x", sp[j]);
				printf("\n");
			}
		}
		return 139;
	}
	return 0;
}

void
wasm_i486_free(void *arg)
{
	struct wasm_i486_task *t = arg;

	if (t == NULL)
		return;
	kmem_free(t->vm.m.hbase, I486_REGION_SIZE);
	kmem_free(t, sizeof(*t));
}
