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
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/lock.h>
#include <sys/lwp.h>

#include <uvm/uvm.h>

#include <machine/pmap.h>

/*
 * The pmap module for the WebAssembly port: flat (VA == PA).
 *
 * wasm32 has no page tables the kernel controls: the linear memory is
 * one flat, always-mapped region.  There are therefore no PTEs to
 * build, no TLBs to shoot down, and no access/dirty hardware to track;
 * uvm's page mapping requests become accounting operations only, and
 * memory protection is enforced purely in software by uvm's map-entry
 * bookkeeping (a user write to a read-only region is, for now, simply
 * not trapped).
 *
 * What uvm still needs from us:
 *
 *	- resident/wired page counts (per pmap) for fault avoidance,
 *	  pmap_remove_all() on exec/exit, and /proc accounting;
 *	- pmap_zero_page()/pmap_copy_page() (the MD page ops);
 *	- the lifetime rules of struct pmap (create/reference/destroy/
 *	  fork), which the MI code drives blindly.
 *
 * pmap_extract() answers the identity mapping for every address: any
 * address the kernel asks about is accessible, since it was returned
 * from the same linear space.
 */

struct pmap {
	kmutex_t	pm_lock;
	int		pm_refcount;
	u_int		pm_resident_count;
	u_int		pm_wired_count;
};

static struct pmap kernel_pmap_store;

/* <uvm/uvm_pmap.h> declares this as a const pointer and pmap_kernel()
 * expands to it: hand out the static kernel pmap directly. */
struct pmap *const kernel_pmap_ptr = &kernel_pmap_store;

void
pmap_init(void)
{

	mutex_init(&kernel_pmap_store.pm_lock, MUTEX_DEFAULT, IPL_NONE);
	kernel_pmap_store.pm_refcount = 1;
}

void
pmap_pv_init(void)
{

	/* no pv tracking: see the file comment */
}

struct pmap *
pmap_create(void)
{
	struct pmap *pmap;

	pmap = kmem_alloc(sizeof(*pmap), KM_SLEEP);
	mutex_init(&pmap->pm_lock, MUTEX_DEFAULT, IPL_NONE);
	pmap->pm_refcount = 1;
	pmap->pm_resident_count = 0;
	pmap->pm_wired_count = 0;
	return pmap;
}

void
pmap_reference(pmap_t pmap)
{

	mutex_enter(&pmap->pm_lock);
	pmap->pm_refcount++;
	mutex_exit(&pmap->pm_lock);
}

void
pmap_destroy(pmap_t pmap)
{
	int refcnt;

	if (pmap == NULL || pmap == kernel_pmap_ptr)
		return;

	mutex_enter(&pmap->pm_lock);
	refcnt = --pmap->pm_refcount;
	mutex_exit(&pmap->pm_lock);

	if (refcnt == 0) {
		mutex_destroy(&pmap->pm_lock);
		kmem_free(pmap, sizeof(*pmap));
	}
}

void
pmap_fork(pmap_t parent, pmap_t child)
{

	/*
	 * The child's pmap starts empty: the pages are shared by the
	 * identity mapping and the child's uvm map entries are copied
	 * by MI code, which drives pmap_enter() for them as needed.
	 */
}

/*
 * pmap_activate/pmap_deactivate: no address space context to load.
 */
void
pmap_activate(struct lwp *l)
{
}

void
pmap_deactivate(struct lwp *l)
{
}

/*
 * pmap_enter: record a mapping.  Returns 0 on success (never fails:
 * there is nothing to allocate).
 */
int
pmap_enter(pmap_t pmap, vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{

	mutex_enter(&pmap->pm_lock);
	pmap->pm_resident_count++;
	if (flags & PMAP_WIRED)
		pmap->pm_wired_count++;
	mutex_exit(&pmap->pm_lock);
	return 0;
}

void
pmap_kenter_pa(vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{

	mutex_enter(&kernel_pmap_ptr->pm_lock);
	kernel_pmap_ptr->pm_resident_count++;
	if (flags & PMAP_WIRED)
		kernel_pmap_ptr->pm_wired_count++;
	mutex_exit(&kernel_pmap_ptr->pm_lock);
}

bool
pmap_extract(pmap_t pmap, vaddr_t va, paddr_t *pap)
{

	if (pap != NULL)
		*pap = va;
	return true;
}

/*
 * pmap_remove: forget a range of mappings.
 */
void
pmap_remove(pmap_t pmap, vaddr_t sva, vaddr_t eva)
{
	u_int npages;

	if (sva >= eva)
		return;
	npages = (u_int)((eva - sva) >> PGSHIFT);

	mutex_enter(&pmap->pm_lock);
	KASSERT(pmap->pm_resident_count >= npages);
	pmap->pm_resident_count -= npages;
	mutex_exit(&pmap->pm_lock);
}

void
pmap_kremove(vaddr_t va, vsize_t size)
{
	u_int npages;

	if (size == 0)
		return;
	npages = (u_int)(size >> PGSHIFT);

	mutex_enter(&kernel_pmap_ptr->pm_lock);
	KASSERT(kernel_pmap_ptr->pm_resident_count >= npages);
	kernel_pmap_ptr->pm_resident_count -= npages;
	mutex_exit(&kernel_pmap_ptr->pm_lock);
}

bool
pmap_remove_all(pmap_t pmap)
{

	mutex_enter(&pmap->pm_lock);
	pmap->pm_resident_count = 0;
	pmap->pm_wired_count = 0;
	mutex_exit(&pmap->pm_lock);
	return false;
}

void
pmap_unwire(pmap_t pmap, vaddr_t va)
{

	mutex_enter(&pmap->pm_lock);
	if (pmap->pm_wired_count > 0)
		pmap->pm_wired_count--;
	mutex_exit(&pmap->pm_lock);
}

/*
 * pmap_write_protect/pmap_update: protection and TLB maintenance are
 * no-ops without page tables; see the file comment.
 */
void
pmap_write_protect(pmap_t pmap, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{
}

void
pmap_update(pmap_t pmap)
{
}

long
pmap_resident_count(pmap_t pmap)
{

	return pmap->pm_resident_count;
}

long
pmap_wired_count(pmap_t pmap)
{

	return pmap->pm_wired_count;
}

/*
 * Access/dirty tracking: none in hardware, so pages report
 * never-modified/never-referenced, which biases the page daemon
 * against reclaiming (safe).
 */
bool
pmap_clear_modify(struct vm_page *pg)
{

	return true;
}

bool
pmap_clear_reference(struct vm_page *pg)
{

	return true;
}

bool
pmap_is_modified(struct vm_page *pg)
{

	return false;
}

bool
pmap_is_referenced(struct vm_page *pg)
{

	return false;
}

/*
 * Page operations: the linear memory is directly addressable, so
 * pa == va.
 */
void
pmap_zero_page(paddr_t pa)
{

	memset((void *)(vaddr_t)pa, 0, PAGE_SIZE);
}

void
pmap_copy_page(paddr_t src, paddr_t dst)
{

	memcpy((void *)(vaddr_t)dst, (const void *)(vaddr_t)src, PAGE_SIZE);
}

void
pmap_copy(pmap_t dstpmap, pmap_t srcpmap, vaddr_t dst, vsize_t len,
    vaddr_t src)
{

	/* mappings are the identity: nothing to replicate */
}

paddr_t
pmap_phys_address(paddr_t ppn)
{

	return ppn << PGSHIFT;
}

/*
 * pmap_growkernel: the address space exists in full from the start;
 * report the requested end as the new end so callers make progress.
 */
vaddr_t
pmap_growkernel(vaddr_t maxkvaddr)
{

	return round_page(maxkvaddr);
}

/*
 * pmap_virtual_space: report the user address space bounds.
 * TODO: coordinate with the host-provided memory map so that user
 * allocations cannot overlap the kernel image and static data, which
 * live at the low end of the same linear memory.
 */
void
pmap_virtual_space(vaddr_t *start, vaddr_t *end)
{

	*start = round_page((vaddr_t)VM_MIN_ADDRESS);
	*end = trunc_page((vaddr_t)VM_MAXUSER_ADDRESS);
}
