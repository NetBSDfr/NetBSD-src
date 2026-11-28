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
#include <sys/device.h>
#include <sys/timetc.h>

void	wasm_clock_tick(void);

/*
 * Clocks for the WebAssembly port: the host provides the time source.
 * There is no hardware clock and no periodic interrupt.  The host's
 * monotonic microsecond clock (the "now" import, same one used for
 * BOOT_DURATION) is the time source, but it cannot be handed to the
 * timecounter framework directly: the kernel only regains control at
 * syscall entry, so between two entries the host clock can jump by
 * seconds (a JSPI-suspended sleep).  The timecounter framework folds a
 * counter delta into the timehands as th_scale * delta, and th_scale *
 * delta == 2^64 * elapsed_seconds -- it overflows once the gap since
 * the last rewind exceeds one second, silently dropping whole seconds.
 *
 * So the port keeps its own virtual microsecond counter (wasm_vcnt) and
 * advances it to match the host clock at each syscall entry, rewinding
 * the timehands (tc_ticktock -> tc_windup) in sub-second steps so no
 * single delta ever reaches the overflow threshold.  binuptime() then
 * reports real elapsed time, which is what makes date(1)/time(1) and
 * the monotonic clock advance.  Time is frozen between syscalls (there
 * is nothing running to observe it); it is caught up before any syscall
 * that could read it.
 */
uint64_t	wasm_host_now(void)
			    __attribute__((import_module("host"),
			    import_name("now")));

static uint64_t wasm_vcnt;	/* virtual counter, microseconds */

static u_int
wasm_get_timecount(struct timecounter *tc)
{

	return (u_int)wasm_vcnt;
}

static struct timecounter wasm_timecounter = {
	.tc_get_timecount	= wasm_get_timecount,
	.tc_counter_mask	= ~0u,
	.tc_frequency		= 1000000,
	.tc_name		= "wasmhost",
	.tc_quality		= 1000,
};

void
cpu_initclocks(void)
{

	tc_init(&wasm_timecounter);
}

void
setstatclockrate(int hzrate)
{
}

/*
 * Catch the virtual counter up with the host clock and rewind the
 * timehands in <1s steps (see the comment above).  Called from the
 * syscall entry points, before the handler can read the clock.
 */
void
wasm_clock_tick(void)
{
	static uint64_t last;	/* host now() accounted for so far */
	uint64_t now = wasm_host_now();
	uint64_t gap;

	if (last == 0) {
		last = now;
		return;
	}
	gap = now - last;
	while (gap > 0) {
		uint64_t step = gap > 500000 ? 500000 : gap;	/* <1s */
		wasm_vcnt += step;
		gap -= step;
		last += step;
		tc_ticktock();
	}
}

