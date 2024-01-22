/*	$NetBSD: cpu.h,v 1.55 2024/01/18 14:39:07 thorpej Exp $	*/

/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1982, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
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
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: cpu.h 1.16 91/03/25$
 *
 *	@(#)cpu.h	8.4 (Berkeley) 1/5/94
 */

#ifndef _MACHINE_CPU_H_
#define _MACHINE_CPU_H_

#if defined(_KERNEL_OPT)
#include "opt_lockdebug.h"
#endif

/*
 * Get common m68k CPU definitions.
 */
#include <m68k/cpu.h>

#if defined(_KERNEL)
/*
 * Arguments to hardclock and gatherstats encapsulate the previous
 * machine state in an opaque clockframe.  On the mvme68k, we use
 * what the locore.s glue puts on the stack before calling C-code.
 */
struct clockframe {
	u_int	cf_regs[4];	/* d0,d1,a0,a1 */
	u_short	cf_sr;		/* sr at time of interrupt */
	u_long	cf_pc;		/* pc at time of interrupt */
	u_short	cf_vo;		/* vector offset (4-word frame) */
} __attribute__((packed));

#define	CLKF_USERMODE(framep)	(((framep)->cf_sr & PSL_S) == 0)
#define	CLKF_PC(framep)		((framep)->cf_pc)

/*
 * The clock interrupt handler can determine if it's a nested
 * interrupt by checking for interrupt_depth > 1.
 * (Remember, the clock interrupt handler itself will cause the
 * depth counter to be incremented).
 */
#define	CLKF_INTR(framep)	(idepth > 1)

/*
 * Preempt the current process if in interrupt from user mode,
 * or after the current trap/syscall if in system mode.
 */
#define	cpu_need_resched(ci,l,flags)	do {	\
	__USE(flags); 				\
	aston();				\
} while (/*CONSTCOND*/0)

/*
 * Give a profiling tick to the current process when the user profiling
 * buffer pages are invalid.  On the mvme68k, request an ast to send us
 * through trap, marking the proc as needing a profiling tick.
 */
#define	cpu_need_proftick(l)	\
	do { (l)->l_pflag |= LP_OWEUPC; aston(); } while (/* CONSTCOND */0)

/*
 * Notify the current process (p) that it has a signal pending,
 * process as soon as possible.
 */
#define	cpu_signotify(l)	aston()

extern int astpending;		/* need to trap before returning to user mode */
#define aston() (astpending++)

#define	MVME68K		1	/* XXX */

#endif /* _KERNEL */

/*
 * Values for machineid; these match the Bug's values.
 */
#define	MVME_147	0x147
#define	MVME_162	0x162
#define	MVME_166	0x166
#define	MVME_167	0x167
#define	MVME_172	0x172
#define	MVME_177	0x177

#ifdef _KERNEL
extern	int machineid;
extern	int cpuspeed;
extern	char *intiobase, *intiolimit;
extern	u_int intiobase_phys, intiotop_phys;
extern	u_long ether_data_buff_size;
extern	u_char mvme_ea[6];

void	doboot(int) 
	__attribute__((__noreturn__));
int	nmihand(void *);
void	mvme68k_abort(const char *);
void	*iomap(u_long, size_t);
void	iounmap(void *, size_t);

/* physical memory addresses where mvme147's onboard devices live */
#define	INTIOBASE147	(0xfffe0000u)
#define	INTIOTOP147	(0xfffe5000u)

/* ditto for mvme1[67][27] */
#define	INTIOBASE1xx	(0xfff40000u)
#define	INTIOTOP1xx	(0xfffd0000u)

#endif /* _KERNEL */

#endif /* _MACHINE_CPU_H_ */
