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
 * E1b: a fixed test blob (write + exit via int $0x80) runs in a
 * kmem-allocated guest region.  The guest address space is identity:
 * guest VA is the kernel address (flat wasm32 space, copyin is
 * memcpy), bounded by the interpreter's region check.  int $0x80
 * reuses wasm_user_svc() directly: the guest stack args at esp+4
 * have exactly the sys_*_args layout the svc path already consumes
 * (NetBSD/i386 passes syscall args on the stack, 32-bit words ==
 * syscallarg_t on wasm32).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <arch/wasm/wasm/emul_i486.h>

#define	I486_REGION_SIZE	(4u << 20)

struct wasm_i486_task {
	struct i486_vm vm;		/* first member */
};

extern int wasm_user_svc(int, int);

void	*wasm_i486_create(void);
int	wasm_i486_exec(void *);
void	wasm_i486_free(void *);

/*
 * int $0x80 bridge.  NetBSD/i386 ABI (lib/libc/arch/i386/sys/
 * __syscall.S): number in eax and also pushed at [esp]; args at
 * esp+4 in the sys_*_args layout; return eax/edx, CF = error.
 * wasm_user_svc returns rv or -errno, matching what we need here.
 */
static void
wasm_i486_syscall(struct i486_vm *vm)
{
	uint8_t *sp;
	int rv;

	sp = i486_h(&vm->m, vm->s.regs[R_ESP]);
	if (sp == NULL) {
		vm->s.regs[R_EAX] = EFAULT;
		vm->s.eflags |= I486_CF;
		return;
	}
	rv = wasm_user_svc((int)vm->s.regs[R_EAX],
	    (int)(uintptr_t)(sp + 4));
	if (rv < 0) {
		vm->s.regs[R_EAX] = (uint32_t)(-rv);
		vm->s.eflags |= I486_CF;
	} else {
		vm->s.regs[R_EAX] = (uint32_t)rv;
		vm->s.eflags &= ~I486_CF;
	}
}

/*
 * write(1, msg, 16); exit(0) -- addresses patched to the region.
 * The number slot push (%eax before int) matches the libc stub.
 */
static const uint8_t wasm_i486_blob[] = {
	0xb8, 0x04, 0x00, 0x00, 0x00,		/* mov  $SYS_write,%eax */
	0x68, 0x10, 0x00, 0x00, 0x00,		/* push $16		*/
	0x68, 0x00, 0x00, 0x00, 0x00,		/* push $msg  [patched]	*/
	0x68, 0x01, 0x00, 0x00, 0x00,		/* push $1		*/
	0x50,					/* push %eax (slot)	*/
	0xcd, 0x80,				/* int  $0x80		*/
	0x83, 0xc4, 0x10,			/* add  $16,%esp	*/
	0xb8, 0x01, 0x00, 0x00, 0x00,		/* mov  $SYS_exit,%eax	*/
	0x6a, 0x00,				/* push $0		*/
	0x50,					/* push %eax (slot)	*/
	0xcd, 0x80,				/* int  $0x80		*/
	0xf4					/* hlt (exit throws)	*/
};

void *
wasm_i486_create(void)
{
	struct wasm_i486_task *t;
	uint8_t *region;
	uintptr_t base, entry, msg;

	region = kmem_alloc(I486_REGION_SIZE, KM_SLEEP);
	memset(region, 0, I486_REGION_SIZE);

	t = kmem_zalloc(sizeof(*t), KM_SLEEP);
	t->vm.m.hbase = region;
	t->vm.m.vbase = (uintptr_t)region;	/* identity */
	t->vm.m.size = I486_REGION_SIZE;
	t->vm.syscall = wasm_i486_syscall;

	entry = base = (uintptr_t)region;
	entry += 0x1000;
	msg = base + 0x2000;

	memcpy(region + 0x2000, "hello from i486\n", 16);
	memcpy(region + 0x1000, wasm_i486_blob, sizeof(wasm_i486_blob));
	/* patch push $msg imm32 (blob bytes 11..14; byte 10 is the 0x68) */
	region[0x1000 + 11] = msg;
	region[0x1000 + 12] = msg >> 8;
	region[0x1000 + 13] = msg >> 16;
	region[0x1000 + 14] = msg >> 24;

	t->vm.s.eip = entry;
	t->vm.s.regs[R_ESP] = base + I486_REGION_SIZE - 64;

	printf("i486: guest region %p..%p\n", region,
	    region + I486_REGION_SIZE);
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
