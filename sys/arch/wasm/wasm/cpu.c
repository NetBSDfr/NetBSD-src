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
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/reboot.h>

#include <machine/cpu.h>
#include <machine/intr.h>

/*
 * Core machine-dependent support for the WebAssembly port.
 *
 * The model is a single, non-interruptible vCPU: the wasm engine never
 * delivers asynchronous events, so there are no hardware interrupts.
 * Kernel code runs to completion within one call from the host, and
 * everything that would be an interrupt on real hardware (softints,
 * IPIs) is driven explicitly from the host boundary or at kernel
 * entry/exit points.
 */

/*
 * The current LWP.  <machine/cpu.h> makes curlwp a plain variable:
 * every operation that could change it crosses a wasm function call,
 * which the compiler cannot move across, so no volatile or TLS is
 * needed.  cpu_switchto() maintains it.
 */
struct lwp *wasm_curlwp;

/*
 * Only one CPU, statically allocated: the primary.
 */
struct cpu_info cpu_info_primary = {
	.ci_flags	= CPUF_PRIMARY,
	.ci_cpuid	= 0,
};
struct cpu_info *cpu_info_list = &cpu_info_primary;

/*
 * Software interrupt priority level.  splraise()/spllower() move it;
 * it is the only thing separating process context from "interrupt"
 * context on this port, since there are no asynchronous interrupts.
 */
static int wasm_ipl;

bool
cpu_intr_p(void)
{

	return wasm_ipl != IPL_NONE;
}

int
splraise(int newipl)
{
	int s = wasm_ipl;

	if (newipl > s)
		wasm_ipl = newipl;
	return s;
}

void
spllower(int oldipl)
{

	wasm_ipl = oldipl;
	/*
	 * TODO: this is where a softint dispatch would be triggered
	 * when the host starts delivering virtual interrupts.
	 */
}

void
Xspllower(int oldipl)
{

	spllower(oldipl);
}

/*
 * Delay loops.  The port has no timer until the clock interface to
 * the host exists; a busy wait on the host clock is a TODO.  Callers
 * only use delay() for hardware that does not exist here.
 */
static void
wasm_delay(unsigned int usecs)
{

	/* nothing to busy wait on yet */
}
void (*delay_func)(unsigned int) = wasm_delay;

/*
 * Idle loop hook.  <machine/cpu.h> carries the x86 declaration; the
 * engine is best left to suspend by returning to the host, which the
 * idle LWP machinery will do once context switching lands.
 */
static void
wasm_cpu_idle(void)
{
}
void (*x86_cpu_idle)(void) = wasm_cpu_idle;

void
cpu_configure(void)
{

	splraise(IPL_HIGH);
}

void
cpu_rootconf(void)
{

	/* the root file system is provided by the host */
}

void
cpu_startup(void)
{

	printf("\n");
	printf("WebAssembly (wasm32) system, %d MB of memory\n",
	    (int)ptoa(physmem) / (1024 * 1024));
}

void
cpu_reboot(int howto, char *cf)
{

	if (howto & RB_HALT)
		printf("\nstopping\n");
	else
		printf("\nrebooting\n");
	for (;;) {
		/*
		 * TODO: return to / exit the host instance once the
		 * embedding interface is defined.
		 */
	}
}

/*
 * Rescheduling and cross-CPU notification.  Single CPU: all requests
 * degenerate to marking the target LWP, which then takes the AST at
 * its next kernel exit.
 */
void
cpu_need_resched(struct cpu_info *ci, struct lwp *l, int which)
{

	if (l != NULL)
		aston(l);
}

void
cpu_need_proftick(struct lwp *l)
{

	aston(l);
}

void
cpu_signotify(struct lwp *l)
{

	aston(l);
}

/*
 * Crash dump back ends: no disk crash dumps yet.
 */
void
cpu_dumpconf(void)
{
}
