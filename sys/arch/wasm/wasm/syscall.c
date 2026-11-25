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
#include <sys/cpu.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/syscall.h>
#include <sys/syscallargs.h>
#include <sys/syscallvar.h>
#include <sys/systm.h>

#include <uvm/uvm_extern.h>

#include <machine/frame.h>
#include <machine/userret.h>

/*
 * System call entry for the WebAssembly port.
 *
 * The port defines its own user/kernel interface (there is no
 * hardware trap; the host enters through an exported wrapper that
 * fills a trapframe): the syscall number is in tf_eax, up to four
 * arguments in tf_edi/tf_esi/tf_edx/tf_ecx, any remainder at the
 * user stack pointer tf_esp.  Return values land in tf_eax/tf_edx;
 * errors are returned in tf_eax with the carry flag emulated in
 * tf_eflags bit 0 (the host reads it back).
 */

void	syscall(struct trapframe *);

#ifndef NARGREG
#define	NARGREG		4
#endif
#define	MOREARGS(sp)	((const void *)(uintptr_t)(sp))



void
cpu_spawn_return(struct lwp *l)
{

	userret(l);
}

void
md_child_return(struct lwp *l)
{
	struct trapframe * const tf = lwp_trapframe(l);

	tf->tf_eax = 0;
	tf->tf_edx = 1;
	tf->tf_eflags &= ~1;		/* clear emulated carry */

	userret(l);
}

void
syscall(struct trapframe *tf)
{
	struct lwp * const l = curlwp;
	struct proc * const p = l->l_proc;
	register_t rval[2];
	register_t args[10];
	register_t kargs[NARGREG];
	register_t *params = kargs;
	int error;

	curcpu()->ci_data.cpu_nsyscall++;

	params[0] = tf->tf_edi;
	params[1] = tf->tf_esi;
	params[2] = tf->tf_edx;
	params[3] = tf->tf_ecx;
	size_t nargs = NARGREG;
	size_t code = tf->tf_eax;

	switch (code) {
	case SYS_syscall:
	case SYS___syscall:
		/*
		 * code is first argument, followed by actual args.
		 */
		code = params[0];
		if (nargs > 1) {
			memmove(params, params + 1,
			    (nargs - 1) * sizeof(params[0]));
			nargs -= 1;
		}
		break;
	default:
		break;
	}

	code &= SYS_NSYSENT - 1;
	const struct sysent * const callp = p->p_emul->e_sysent + code;

	if (__predict_false(callp->sy_narg > nargs)) {
		const size_t diff = callp->sy_narg - nargs;
		memcpy(args, params, nargs * sizeof(params[0]));
		error = copyin(MOREARGS(tf->tf_esp), &args[nargs],
		    diff * sizeof(register_t));
		if (error)
			goto bad;
		params = args;
	}

	rval[0] = 0;
	rval[1] = tf->tf_edx;
	error = sy_invoke(callp, l, params, rval, code);

	if (__predict_true(error == 0)) {
		tf->tf_eax = rval[0];
		tf->tf_edx = rval[1];
		tf->tf_eflags &= ~1;		/* clear emulated carry */
	} else {
		switch (error) {
		case ERESTART:
			/*
			 * Set user's pc back to redo the system call.
			 * The host defines the size of the call site
			 * instruction sequence; 4 is the wasm encoding
			 * we standardise on (see TODO in host glue).
			 */
			tf->tf_eip -= 4;
			break;
		case EJUSTRETURN:
			/* nothing to do */
			break;
		default:
		bad:
#ifndef __HAVE_MINIMAL_EMUL
			if (p->p_emul->e_errno)
				error = p->p_emul->e_errno[error];
#endif
			tf->tf_eax = error;
			tf->tf_eflags |= 1;	/* emulated carry */
			break;
		}
	}
}

void
syscall_intern(struct proc *p)
{

	p->p_md.md_syscall = syscall;
}

/*
 * sysarch(): the port reserves none.
 */
int
sys_sysarch(struct lwp *l, const struct sys_sysarch_args *uap,
    register_t *retval)
{

	return ENOSYS;
}
