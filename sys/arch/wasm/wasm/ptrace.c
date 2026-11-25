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
#include <sys/lwp.h>
#include <sys/ptrace.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/ucontext.h>

#include <machine/frame.h>
#include <machine/reg.h>

/*
 * The lwpstatus consumer (kern/sys_process_lwpstatus.c) calls the
 * debug-register set without a visible prototype in this build;
 * match its expectations here (same shape as amd64's
 * process_machdep.c).
 */
int	process_read_dbregs(struct lwp *, struct dbreg *, size_t *);
int	process_write_dbregs(struct lwp *, const struct dbreg *, size_t);
void	process_alloc_dbregs(struct lwp *);

/*
 * ptrace()/signal context support for the WebAssembly port: the
 * process register view is the saved trapframe; there is no
 * floating-point state and no single-step hardware (the emulated
 * PSL_T bit is recorded but has no effect until the host implements
 * stepping).
 */

static struct trapframe *
pt_tf(struct lwp *l)
{

	return l->l_md.md_regs;
}

int
process_read_regs(struct lwp *l, struct reg *regs)
{
	struct trapframe *tf = pt_tf(l);

	if (tf == NULL)
		memset(regs, 0, sizeof(*regs));
	else {
		regs->r_eax = tf->tf_eax;
		regs->r_ecx = tf->tf_ecx;
		regs->r_edx = tf->tf_edx;
		regs->r_ebx = tf->tf_ebx;
		regs->r_esp = tf->tf_esp;
		regs->r_ebp = tf->tf_ebp;
		regs->r_esi = tf->tf_esi;
		regs->r_edi = tf->tf_edi;
		regs->r_eip = tf->tf_eip;
		regs->r_eflags = tf->tf_eflags;
		regs->r_cs = tf->tf_cs;
		regs->r_ss = tf->tf_ss;
	}
	return 0;
}

int
process_write_regs(struct lwp *l, const struct reg *regs)
{
	struct trapframe *tf = pt_tf(l);

	if (tf == NULL)
		return EINVAL;
	tf->tf_eax = regs->r_eax;
	tf->tf_ecx = regs->r_ecx;
	tf->tf_edx = regs->r_edx;
	tf->tf_ebx = regs->r_ebx;
	tf->tf_esp = regs->r_esp;
	tf->tf_ebp = regs->r_ebp;
	tf->tf_esi = regs->r_esi;
	tf->tf_edi = regs->r_edi;
	tf->tf_eip = regs->r_eip;
	tf->tf_eflags = regs->r_eflags;
	tf->tf_cs = regs->r_cs;
	tf->tf_ss = regs->r_ss;
	return 0;
}

int
process_set_pc(struct lwp *l, void *pc)
{
	struct trapframe *tf = pt_tf(l);

	if (tf == NULL)
		return EINVAL;
	tf->tf_eip = (int)(uintptr_t)pc;
	return 0;
}

int
process_sstep(struct lwp *l, int sstep)
{
	struct trapframe *tf = pt_tf(l);

	if (tf == NULL)
		return EINVAL;
	if (sstep)
		tf->tf_eflags |= PSL_T;
	else
		tf->tf_eflags &= ~PSL_T;
	return 0;
}

int
process_read_fpregs(struct lwp *l, struct fpreg *fpregs, size_t *sz)
{

	memset(fpregs, 0, sizeof(*fpregs));
	return 0;
}

int
process_write_fpregs(struct lwp *l, const struct fpreg *fpregs, size_t sz)
{

	return EOPNOTSUPP;
}

int
process_read_dbregs(struct lwp *l, struct dbreg *regs, size_t *sz)
{

	memset(regs, 0, sizeof(*regs));
	return 0;
}

int
process_write_dbregs(struct lwp *l, const struct dbreg *regs, size_t sz)
{

	return EOPNOTSUPP;
}

void
process_alloc_dbregs(struct lwp *l)
{

	/* no debug registers: PT_GETDBREGS/PT_SETDBREGS give EINVAL */
}

int
ptrace_machdep_dorequest(struct lwp *l, struct lwp **tp, int req, void *data,
    int code)
{

	return EINVAL;
}

/*
 * mcontext: the gregs image is the struct reg layout (see mcontext.h);
 * the fp state is always zero (no FPU).
 */
void
cpu_getmcontext(struct lwp *l, mcontext_t *mcp, unsigned int *flags)
{
	struct reg r;

	(void)process_read_regs(l, &r);
	memcpy(mcp->__gregs, &r, sizeof(r));
	memset(&mcp->__fpregs, 0, sizeof(mcp->__fpregs));
}

int
cpu_setmcontext(struct lwp *l, const mcontext_t *mcp, unsigned int flags)
{
	struct reg r;

	memcpy(&r, mcp->__gregs, sizeof(r));
	return process_write_regs(l, &r);
}

int
cpu_mcontext_validate(struct lwp *l, const mcontext_t *mcp)
{

	return 0;
}

/*
 * Signal delivery to userland: building a signal frame on the user
 * stack needs the host user-thread model (TODO); signals are
 * recorded by MI code and simply not delivered.
 */
void
sendsig_siginfo(const ksiginfo_t *ksi, const sigset_t *mask)
{
}
