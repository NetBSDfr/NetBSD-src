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
#include <sys/intr.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/frame.h>
#include <machine/pcb.h>
#include <machine/userret.h>

/*
 * LWP context handling for the WebAssembly port.
 *
 * DESIGN NOTE (first bring-up model): wasm code cannot suspend and
 * resume a machine stack, so this phase runs an epoch model on the
 * single engine stack, with wasm exception handling as the only extra
 * control-flow primitive (see engine_unwind.c):
 *
 *   - The outermost cpu_switchto() call (the one made by an
 *     ordinary mi_switch()) enters a guarded region and runs an
 *     epoch: the LWP the scheduler picked, then everything
 *     sched_nextlwp() can still find afterwards.
 *   - When an LWP running inside the epoch blocks, its mi_switch()
 *     calls cpu_switchto() again; nested calls run the new LWP
 *     within the same epoch, leaving the blocked LWP's frames parked
 *     underneath.
 *   - When nothing is runnable anymore below the outermost frame,
 *     the epoch is unwound to the guarded region with
 *     wasm_engine_throw() (a wasm exception).  Parked frames are
 *     abandoned, so a blocked LWP resumes at the top of its entry
 *     function the next time it is selected.  That is safe for the
 *     kernel's daemon loops (they re-check their queues at the top
 *     of every iteration) but NOT for general partially-executed
 *     LWPs: a partially-run LWP that blocks and is later woken
 *     repeats work already done.  Accept this for bring-up.
 *   - An epoch ends when there is nothing runnable, or when the
 *     scheduler selects the epoch's own LWP again: control returns
 *     from cpu_switchto() to that LWP's mi_switch(), which is what
 *     a real context switch eventually does too.
 *
 * Consequences accepted for bring-up: an LWP exiting via lwp_exit()
 * during an epoch that unwinds never reaches lwp_free() (its lwppl
 * slot leaks until the host design below replaces this model), and
 * LP_RUNNING is advisory: one engine cannot run two LWPs at once,
 * so the races that flag guards do not exist here.
 *
 * The MI LP_RUNNING protocol is respected where it can be:
 * lwp_startup(l, newl) is called as newl takes over (it clears l's
 * LP_RUNNING -- atomically for a zombie, which is the handshake with
 * lwp_free()'s spin -- and unlocks l), and the LWP last run is
 * reported back to the caller's mi_switch(), whose post-switch
 * LP_RUNNING clear and unlock is the correct bookkeeping for an LWP
 * that has just given up the CPU.  l itself is re-marked running
 * when its epoch completes, because lwp_startup() cleared that mark
 * when the epoch started and no other LWP exists to set it back.
 *
 * This model is single-engine serial by construction; the host-worker
 * design (one Web Worker per LWP, suspending with JSPI) replaces it
 * once the embedding interface is defined.  Do not rely on true
 * concurrency until then.
 */

extern int wasm_engine_guarded(void (*)(void *), void *);
extern void wasm_engine_throw(void);

void	wasm_user_entry(void *);		/* exec_wasm32.c */

static int wasm_engine_depth;

struct wasm_epoch {
	struct lwp *l;		/* this LWP's frames own the epoch */
	struct lwp *first;	/* candidate already picked by mi_switch() */
	struct lwp *last;	/* last LWP run, for mi_switch() reporting */
};

/*
 * Select the next runnable LWP and mark it running, mirroring the
 * half of kern_synch.c:nextlwp() that mi_switch() would do for us.
 * That function cannot be called here: its empty-runqueue fallback
 * dereferences ci_data.cpu_idlelwp, and entering the idle LWP's
 * infinite loop would strand the epoch.
 */
static struct lwp *
wasm_epoch_pick(void)
{
	struct cpu_info *ci = curcpu();
	struct lwp *next;

	/*
	 * Pending soft interrupts outrank the run queues, mirroring
	 * kern_synch.c's !__HAVE_FAST_SOFTINTS branch in mi_switch().
	 */
	if (ci->ci_data.cpu_softints != 0) {
		next = softint_picklwp();
		next->l_stat = LSONPROC;
		next->l_pflag |= LP_RUNNING;
		return next;
	}

	next = sched_nextlwp();
	if (next == NULL) {
		return NULL;
	}
	sched_dequeue(next);
	next->l_stat = LSONPROC;
	next->l_pflag |= LP_RUNNING;
	lwp_setlock(next, ci->ci_schedstate.spc_lwplock);

	return next;
}

/*
 * Run the epoch body: run ep->first, then everything the scheduler
 * still has.  Returns only in the outermost frame (see the throw).
 */
static void
wasm_epoch_run(void *arg)
{
	struct wasm_epoch *ep = arg;
	struct lwp *l = ep->l;

	for (;;) {
		struct lwp *newl = ep->first;

		ep->first = NULL;
		if (newl == NULL) {
			newl = wasm_epoch_pick();
		}

		/*
		 * Nothing to run -- or the scheduler selected l
		 * itself, which means the epoch is over and l
		 * resumes.  Nested epochs have no such exit: the
		 * caller whose frames are parked below us cannot
		 * "return to" anything useful once the run queue is
		 * empty, so unwind the epoch instead.
		 */
		if (newl == NULL || newl == l ||
		    (newl->l_flag & LW_IDLE) != 0) {
			/*
			 * Restore ownership before exiting the epoch:
			 * control returns to l's own frames (normally,
			 * or past the guarded region via the throw),
			 * and a global assignment survives the unwinding.
			 */
			wasm_curlwp = l;
			if (wasm_engine_depth > 1)
				wasm_engine_throw();
			return;
		}

		ep->last = newl;

		wasm_curlwp = newl;
		lwp_startup(l, newl);

		if (newl->l_md.md_entryfunc != NULL) {
			void (*func)(void *) = newl->l_md.md_entryfunc;
			void *farg = newl->l_md.md_entryarg;

			/*
			 * The entry function is not cleared: an LWP
			 * that blocks and is later rescheduled re-
			 * enters here (see the restart caveat in the
			 * DESIGN NOTE above).
			 */
			func(farg);
		}

		wasm_curlwp = l;
	}
}

struct lwp *
cpu_switchto(struct lwp *l, struct lwp *newl, bool returning)
{
	struct wasm_epoch ep;

	(void)returning;

	if (newl == NULL || newl == l) {
		/*
		 * Nothing to run: continue under the caller's
		 * identity.
		 */
		return l;
	}

	if ((newl->l_flag & LW_IDLE) != 0) {
		/*
		 * The scheduler fell through to the idle LWP.  Inside
		 * an epoch there is nothing to gain from entering the
		 * idle loop: unwind back to the epoch owner instead,
		 * which resumes its own path (and re-picks when it
		 * next blocks).  Outside an epoch this is a plain
		 * nothing-to-do; continue under the caller's identity.
		 */
		if (wasm_engine_depth > 0) {
			wasm_engine_throw();
			/* NOTREACHED */
		}
		return l;
	}

	ep.l = l;
	ep.first = newl;
	ep.last = NULL;

	if (wasm_engine_depth != 0) {
		/*
		 * Nested call: already inside the outer epoch's
		 * guarded region.
		 */
		wasm_engine_depth++;
		wasm_epoch_run(&ep);
		wasm_engine_depth--;
		return ep.last != NULL ? ep.last : l;
	}

	wasm_engine_depth = 1;
	(void)wasm_engine_guarded(wasm_epoch_run, &ep);
	wasm_engine_depth = 0;

	/*
	 * The unwind (or the owner-resume return) put control back
	 * in l's frames: l is the running LWP again.
	 */
	wasm_curlwp = l;

	/*
	 * Back in l's frames: l resumes, so restore the running mark
	 * that lwp_startup() cleared at the epoch's start, and report
	 * the last LWP that ran so mi_switch() performs its
	 * LP_RUNNING clear and unlock.
	 */
	if (l->l_stat != LSZOMB) {
		l->l_pflag |= LP_RUNNING;
	}

	return ep.last != NULL ? ep.last : l;
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

	/*
	 * The epoch scheduler starts an LWP by calling its
	 * md_entryfunc (wasm_epoch_run()); start_init() returns right
	 * after a successful execve, so re-arm this LWP to enter the
	 * user program wrapper and put it back on the run queue for
	 * the epoch to pick up.  ep_entry carries the entry function
	 * pointer installed by exec_wasm32_makecmds().
	 */
	l->l_md.md_entryfunc = wasm_user_entry;
	l->l_md.md_entryarg = (void *)epp->ep_entry;
	sched_enqueue(l);
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
