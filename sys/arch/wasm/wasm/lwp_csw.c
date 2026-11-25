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
#include <sys/exec.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/frame.h>
#include <machine/pcb.h>
#include <machine/userret.h>

/*
 * LWP context handling for the WebAssembly port.
 *
 * DESIGN NOTE (first bring-up model): wasm32 offers no way to
 * suspend and resume a machine stack, so this phase uses the
 * run-to-completion model: cpu_switchto() changes the identity of
 * the running LWP and, if that LWP has not run yet, calls its entry
 * function synchronously, letting it run to completion on the single
 * engine stack.  Resuming a partially-depth LWP returns to the
 * caller with the new identity; frames left below an "asleep" LWP
 * belong to whatever LWP last touched them.
 *
 * This is good enough to boot a single-CPU kernel whose LWP
 * activity is effectively serial, and is what the host-worker design
 * (one Web Worker per LWP, sleeping with memory.atomic.wait32, as
 * prototyped upstream) will replace once the embedding interface is
 * defined.  Do not rely on true concurrency until then.
 */

struct lwp *
cpu_switchto(struct lwp *l, struct lwp *newl, bool returning)
{

	if (newl == NULL) {
		/*
		 * Scheduler found nothing else to run: continue with
		 * the caller's identity (the caller passes l != NULL
		 * in that path only for statistics).
		 */
		return l;
	}

	wasm_curlwp = newl;
	if (newl->l_md.md_entryfunc != NULL) {
		void (*func)(void *) = newl->l_md.md_entryfunc;
		void *arg = newl->l_md.md_entryarg;

		newl->l_md.md_entryfunc = NULL;
		newl->l_md.md_entryarg = NULL;
		func(arg);
	}
	return l;
}

/*
 * cpu_lwp_fork: give newl a copy of l's saved state and remember
 * where its first run should begin (kernel threads: startfunc;
 * userland fork: md_child_return, set by the caller chain).
 */
void
cpu_lwp_fork(struct lwp *l, struct lwp *newl, void *stacklist, size_t stacksz,
    void (*startfunc)(void *), void *startarg)
{
	struct trapframe *nqt, *qt;

	/*
	 * A userland fork has startfunc == NULL: the child returns to
	 * userland via md_child_return() on its first run.
	 */
	if (startfunc == NULL) {
		startfunc = (void (*)(void *))md_child_return;
		startarg = newl;
	}
	newl->l_md.md_entryfunc = startfunc;
	newl->l_md.md_entryarg = startarg;
	newl->l_md.md_astpending = 0;
	newl->l_md.md_flags = 0;
	newl->l_md.md_wakesig = 0;

	/*
	 * Copy the saved user registers: the child starts life as a
	 * near-clone of the parent's current frame (fork() fixes up
	 * the return value on first run, see md_child_return).
	 */
	qt = l->l_md.md_regs;
	if (qt != NULL) {
		nqt = kmem_alloc(sizeof(*nqt), KM_SLEEP);
		*nqt = *qt;
		newl->l_md.md_regs = nqt;
	}
}

/*
 * cpu_lwp_free/free2: no machine state to release: the trapframe
 * copy is owned by the MI lwp teardown (l_addr / u-area).
 */
void
cpu_lwp_free(struct lwp *l, int holdkernel)
{

	if (l->l_md.md_regs != NULL) {
		kmem_free(l->l_md.md_regs, sizeof(*l->l_md.md_regs));
		l->l_md.md_regs = NULL;
	}
}

void
cpu_lwp_free2(struct lwp *l)
{
}

/*
 * The private region for MD use: nothing to keep beyond MI's
 * l_private (called via lwp_setprivate()).
 */
int
cpu_lwp_setprivate(struct lwp *l, void *priv)
{

	return 0;
}

/*
 * setregs(): prime a freshly exec'd LWP to start at the entry point
 * with the given user stack.
 */
void
setregs(struct lwp *l, struct exec_package *epp, vaddr_t stack)
{
	struct trapframe *tf;

	if (l->l_md.md_regs == NULL) {
		tf = kmem_alloc(sizeof(*tf), KM_SLEEP);
		l->l_md.md_regs = tf;
	} else
		tf = l->l_md.md_regs;

	memset(tf, 0, sizeof(*tf));
	tf->tf_eip = epp->ep_entry;
	tf->tf_esp = stack;
	tf->tf_eax = 0;
	tf->tf_eflags = 0x00000202;	/* IF | reserved-1, informational */
	l->l_md.md_astpending = 0;
}

/*
 * startlwp(): entry for LWP creation from sys_lwp (lwp_sysinit path
 * with e_startlwp).  Context handoff from the creating process is
 * not yet implemented (no user threads until the host glue lands);
 * start in the created context.
 */
void
startlwp(void *arg)
{
	struct lwp *l = curlwp;

	(void)arg;
	userret(l);
}
