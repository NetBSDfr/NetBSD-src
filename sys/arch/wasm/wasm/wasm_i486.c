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
 * E2: real ELF32 EM_386 executables load into a kmem-allocated guest
 * region at the standard i386 link base (0x08048000): guest VA is
 * region-relative (i486_h maps hbase + va - vbase), so it is not a
 * kernel address.  The syscall bridge therefore bounces the stack
 * args block into kernel memory and translates pointer slots to host
 * addresses (per-syscall mask table) before handing them to
 * wasm_user_svc(): the svc path and uiomove() assume identity
 * pointers (init's vmspace is the kernel's, so copyin_vmspace()
 * takes the kcopy branch), and this keeps that true for guests.
 * int $0x80 ABI (lib/libc/arch/i386/sys/__syscall.S): number in eax
 * and also pushed at [esp]; args at esp+4 in the sys_*_args layout
 * (syscallarg_t == wasm32 word, syscall numbers are arch-common).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/exec_elf.h>
#include <sys/kmem.h>
#include <lib/libkern/libkern.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/syscall.h>

#include <arch/wasm/wasm/emul_i486.h>

#define	I486_REGION_SIZE	(4u << 20)
#define	I486_GUEST_BASE		0x08048000u	/* TEXTBASE, ld(1) i386 */

/* keep in sync with wasm_loader.c */
#define	WASM_SYS_spawn		512
#define	WASM_SPAWN_ARGC_MAX	16

struct wasm_i486_task {
	struct i486_vm vm;		/* first member */
};

extern int wasm_user_svc(int, int);
extern int wasm_child_spawn(const char *, char *const *);

void	*wasm_i486_load(const void *, size_t, char *const *);
int	wasm_i486_exec(void *);
void	wasm_i486_free(void *);

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
	[SYS_wait6]		= I486_PTR(2),
};

static void
wasm_i486_ret(struct i486_vm *vm, int rv)
{

	if (rv < 0) {
		vm->s.regs[R_EAX] = (uint32_t)(-rv);
		vm->s.eflags |= I486_CF;
	} else {
		vm->s.regs[R_EAX] = (uint32_t)rv;
		vm->s.eflags &= ~I486_CF;
	}
}

static void
wasm_i486_syscall(struct i486_vm *vm)
{
	uint32_t esp = vm->s.regs[R_ESP];
	uint32_t args[8];
	char *kargv[WASM_SPAWN_ARGC_MAX + 1];
	int nr = (int)vm->s.regs[R_EAX];
	uint8_t mask, hi;
	uint32_t *ap, *av;
	int rv, i;

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

	mask = (nr >= 0 && nr <= 512) ? i486_ptrmask[nr] : 0;
	if (mask == 0) {
		uint8_t *sp = i486_h(&vm->m, esp);

		if (sp == NULL)
			goto efault;
		rv = wasm_user_svc(nr, (int)(uintptr_t)(sp + 4));
		wasm_i486_ret(vm, rv);
		return;
	}

	for (hi = 0; hi < 8 && (mask >> hi) != 0; hi++)
		;
	for (i = 0; i < hi; i++) {
		uint32_t *w = (uint32_t *)i486_h(&vm->m, esp + 4 + 4 * i);

		if (w == NULL)
			goto efault;
		args[i] = *w;
	}
	for (i = 0; i < hi; i++) {
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
 * Build the initial guest stack: argc, argv[], NULL, envp NULL,
 * auxv (AT_PAGESZ, AT_NULL), argv strings above (all SysV i386 at
 * _start).  Returns 0 or ENOMEM if the block does not fit.
 */
static int
i486_build_stack(struct wasm_i486_task *t, char *const *argv)
{
	struct i486_mem *m = &t->vm.m;
	uint32_t strp[WASM_SPAWN_ARGC_MAX];
	uint32_t *q;
	char *p;
	int argc, i, nwords;

	argc = 0;
	if (argv != NULL)
		while (argc < WASM_SPAWN_ARGC_MAX && argv[argc] != NULL)
			argc++;

	p = (char *)m->hbase + m->size - 16;
	for (i = argc - 1; i >= 0; i--) {
		size_t l = strlen(argv[i]) + 1;

		if (p - l < (char *)m->hbase)
			return ENOMEM;
		p -= l;
		memcpy(p, argv[i], l);
		strp[i] = (uint32_t)(m->vbase + (uintptr_t)(p - (char *)m->hbase));
	}

	nwords = 1 + (argc + 1) + 1 + 4;
	q = (uint32_t *)(((uintptr_t)p - nwords * 4) & ~(uintptr_t)15);
	if ((uint8_t *)q < m->hbase)
		return ENOMEM;

	q[0] = argc;
	for (i = 0; i < argc; i++)
		q[1 + i] = strp[i];
	q[1 + argc] = 0;			/* argv NULL */
	q[2 + argc] = 0;			/* envp NULL */
	q[3 + argc] = AT_PAGESZ;
	q[4 + argc] = 4096;
	q[5 + argc] = AT_NULL;
	q[6 + argc] = 0;

	t->vm.s.regs[R_ESP] = (uint32_t)(m->vbase + (uintptr_t)((uint8_t *)q - m->hbase));
	t->vm.s.regs[R_EBP] = 0;
	t->vm.s.eflags = 0;
	return 0;
}

/*
 * Load an ELF32 EM_386 executable into a fresh guest region.
 * Returns the task handle or NULL (not an i386 ELF, or does not
 * fit).  argv strings (host pointers) are copied onto the guest
 * stack.
 */
void *
wasm_i486_load(const void *image, size_t size, char *const *argv)
{
	const Elf32_Ehdr *eh = image;
	const Elf32_Phdr *ph;
	struct wasm_i486_task *t;
	uint8_t *region;
	int i;

	if (size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh->e_type != ET_EXEC || eh->e_machine != EM_386)
		return NULL;
	if (eh->e_phnum == 0 || eh->e_phnum > 16 ||
	    eh->e_phoff + (size_t)eh->e_phnum * sizeof(*ph) > size)
		return NULL;

	region = kmem_alloc(I486_REGION_SIZE, KM_SLEEP);
	memset(region, 0, I486_REGION_SIZE);

	t = kmem_zalloc(sizeof(*t), KM_SLEEP);
	t->vm.m.hbase = region;
	t->vm.m.vbase = I486_GUEST_BASE;
	t->vm.m.size = I486_REGION_SIZE;
	t->vm.syscall = wasm_i486_syscall;

	ph = (const Elf32_Phdr *)((const uint8_t *)image + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++, ph++) {
		if (ph->p_type != PT_LOAD)
			continue;
		if (ph->p_vaddr < I486_GUEST_BASE ||
		    ph->p_vaddr + ph->p_memsz > I486_GUEST_BASE + I486_REGION_SIZE ||
		    ph->p_offset + ph->p_filesz > size ||
		    ph->p_filesz > ph->p_memsz)
			goto bad;
		memcpy(region + (ph->p_vaddr - I486_GUEST_BASE),
		    (const uint8_t *)image + ph->p_offset, ph->p_filesz);
	}
	if (i486_h(&t->vm.m, eh->e_entry) == NULL)
		goto bad;
	t->vm.s.eip = eh->e_entry;

	if (i486_build_stack(t, argv) != 0)
		goto bad;

	printf("i486: loaded entry=%#x esp=%#x, %zu bytes image\n",
	    t->vm.s.eip, t->vm.s.regs[R_ESP], size);
	return t;

bad:
	kmem_free(region, I486_REGION_SIZE);
	kmem_free(t, sizeof(*t));
	return NULL;
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
		printf("i486: fault at eip=%08x (op %02x)\n", t->vm.s.eip,
		    p != NULL ? *p : 0xffu);
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
