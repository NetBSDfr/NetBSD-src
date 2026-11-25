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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/exec.h>
#include <sys/ptrace.h>

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>

#include <dev/mm.h>

/*
 * Assorted machine-dependent entry points that do not belong to the
 * pmap / context-switch / syscall groups.
 */

char machine[] = "wasm";		/* system type */
char machine_arch[] = "wasm32";		/* architecture */

/*
 * /dev/mmap-style direct physical access: all of linear memory is
 * accessible.
 */
int
mm_md_physacc(paddr_t pa, vm_prot_t prot)
{

	return 0;
}

/*
 * fork(): no machine state lives in struct proc for this port.
 */
void
cpu_proc_fork(struct proc *p1, struct proc *p2)
{
}

/*
 * coredump(): no machine-dependent section yet.
 */
int
cpu_coredump(struct lwp *l, struct coredump_iostate *iocookie,
    struct core *chdr)
{

	return 0;
}

/*
 * Soft interrupts: with no virtual interrupt controller yet,
 * softint_trigger() records nothing; softint lwp work runs when the
 * scheduler dispatches them (see the lwp_csw.c design note).
 */
void
softint_init_md(struct lwp *l, u_int level, uintptr_t *machdep)
{

	*machdep = level;
}

void
softint_trigger(uintptr_t machdep)
{
}
