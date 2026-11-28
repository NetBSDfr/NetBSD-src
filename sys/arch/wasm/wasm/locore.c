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

#include "opt_md.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lwp.h>
#include <sys/proc.h>

#include <uvm/uvm.h>
#include <uvm/uvm_physseg.h>

#ifdef BOOT_DURATION
#include <sys/boot_duration.h>
#endif
#ifdef MEMORY_DISK_DYNAMIC
#include <dev/md.h>
#endif

#include <machine/cpu.h>
#include <machine/pmap.h>
#include <machine/rootimg.h>

/*
 * Module entry and physical memory bootstrap for the WebAssembly port.
 *
 * There is no crt0 and no _start: the host instantiates the module
 * (supplying the imported linear memory) and calls the exported
 * kernel_boot(), which describes memory to uvm and then enters the MI
 * main().  main() is declared here as init_main.c does; like every
 * other port, the entry path is:
 *
 *	host -> kernel_boot -> pmap_bootstrap -> main (init_main.c)
 *
 * Physical memory is the wasm linear memory, with VA == PA.  The
 * usable region begins above everything the linker laid out
 * statically: __heap_base is the first address after the data
 * segments and the reserved engine stack, so no allocation handed to
 * uvm can alias the running stack or the image.
 */

void	kernel_boot(void);	/* exported host entry point */

#ifdef BOOT_DURATION
/*
 * Elapsed wall time since module entry, from the host clock.  Same
 * contract as the other ports' boot_duration_timer() (cf.
 * sys/arch/aarch64/aarch64/aarch64_machdep.c): init_main.c prints
 * "kernel boot time" just before entering uvm_scheduler().
 * The import mirrors sys/arch/wasm/wasm/cpu.c's host.exit; the host
 * (wasmrun/run.mjs) returns microseconds as an i64.
 */
uint64_t	wasm_host_now(void)
				__attribute__((import_module("host"),
				import_name("now")));

static uint64_t boot_start_us;

uint64_t
boot_duration_timer(void)
{

	return (wasm_host_now() - boot_start_us) / 1000;
}
#endif /* BOOT_DURATION */

void
pmap_bootstrap(vaddr_t first_avail)
{
	paddr_t avail_start, avail_end, pool_end;
	size_t wasm_pages;
	vsize_t kmemsize;

	(void)first_avail;

	wasm_pages = __builtin_wasm_memory_size(0);
	avail_end = (paddr_t)wasm_pages * WASM_MEM_PAGE_BYTES;
	avail_start = round_page((paddr_t)(uintptr_t)__heap_base);

	/*
	 * Flat address map: uvm (and through it kmem, kva and every
	 * kernel arena) must stay inside its documented region, below
	 * the user address space and the root image staged by the host
	 * (see rootimg.h).  Clamping here means no kernel allocator can
	 * ever hand out an address outside the arena.
	 */
	if (avail_end > WASM_KERNEL_ARENA_END)
		avail_end = WASM_KERNEL_ARENA_END;

	if (avail_end <= avail_start)
		panic("pmap_bootstrap: no usable memory: linear memory"
		    " too small (avail_start %lx avail_end %lx)",
		    (u_long)avail_start, (u_long)avail_end);

	uvm_md_init();

	/*
	 * Bound the kmem arena by the memory we actually manage: with
	 * VA == PA the arena is physical memory, so the default
	 * fraction of physmem must not be allowed to push its fixed
	 * reservation past the end of the linear memory.
	 * kmeminit_nkmempages() leaves a preset value alone.  Two
	 * thirds covers the boot-time pool and malloc footprint plus
	 * several concurrent 28 MB i486 guest regions (a shell plus a
	 * pipeline); the remainder leaves the page allocator room for
	 * object pages (vnode pager, tmpfs, guest COW).
	 */
	nkmempages = atop(avail_end - avail_start) * 2 / 3;
	kmemsize = ptoa(nkmempages);

	/*
	 * Carve the kmem arena out of the page allocator: the arena's
	 * kva range is physical memory under the identity pmap, so
	 * those pages must never sit in the free pool or they would be
	 * handed out a second time as object pages (and then freed a
	 * second time by uvm_km_pgremove_intrsafe()).  The page array
	 * still covers the whole range (segment end is avail_end) so
	 * PHYS_TO_VM_PAGE() answers for every managed address; only
	 * [avail_start, pool_end) is ever free.  pmap_virtual_space()
	 * reports [pool_end, avail_end) so the boot-time page array
	 * and the kmem arena live above the pool, inside the reserve.
	 */
	pool_end = avail_end - kmemsize - WASM_BOOTALLOC_RESERVE;
	if (pool_end <= avail_start)
		panic("pmap_bootstrap: linear memory too small for kmem"
		    " carve-out (avail_start %lx avail_end %lx)",
		    (u_long)avail_start, (u_long)avail_end);
	wasm_pool_end = pool_end;

	physmem = atop(avail_end);
	uvm_page_physload(atop(avail_start), atop(avail_end),
	    atop(avail_start), atop(pool_end), VM_FREELIST_DEFAULT);
}

/*
 * kernel_boot(): the exported entry point.
 */
void main(void);		/* init_main.c */

/*
 * Bootstrap handover to the MI code.  There is no boot assembly and
 * lwp0's static initializer cannot know about cpu_info, so point it
 * at the (statically initialised) primary CPU and make it the
 * running LWP before entering main(); without this, curlwp is NULL
 * and every curcpu()-derived scheduler structure resolves to near-
 * zero linear memory.  The u-area is a fixed reservation: this port
 * has no boot stack to adopt, and only lwp_getpcb()/kstack sanity
 * reads it during boot.
 */
static char lwp0_uarea[USPACE] __aligned(NBPG);

static void
lwp0_bootstrap(void)
{

	lwp0.l_addr = (vaddr_t)lwp0_uarea;
	lwp0.l_cpu = &cpu_info_primary;
	lwp0.l_mutex = cpu_info_primary.ci_schedstate.spc_lwplock;
	lwp0.l_pflag |= LP_RUNNING;
	cpu_info_primary.ci_onproc = &lwp0;
	/*
	 * There is no idle LWP in this bring-up model (nothing calls
	 * cpu_up()): kern_synch.c:nextlwp() falls back to
	 * ci_data.cpu_idlelwp when the run queue is empty, and
	 * cpu_switchto() treats the epoch's own LWP being selected
	 * there as "epoch over", which is the correct answer for a
	 * request to schedule nothing.
	 */
	cpu_info_primary.ci_data.cpu_idlelwp = &lwp0;
	wasm_curlwp = &lwp0;
}

void
kernel_boot(void)
{

#ifdef BOOT_DURATION
	boot_start_us = wasm_host_now();
#endif
	pmap_bootstrap(0);
#ifdef MEMORY_DISK_DYNAMIC
	/*
	 * Hand the host-staged FFS image to md(4): md0's attach hook
	 * (dev/md_root.c) turns it into the root ramdisk.  This runs
	 * before main() configures any devices, which is the ordering
	 * md_root_setconf() requires.
	 */
	md_root_setconf((char *)(uintptr_t)WASM_ROOTIMG_ADDR,
	    (size_t)WASM_ROOTIMG_SIZE);
#endif
	lwp0_bootstrap();
	main();
}
