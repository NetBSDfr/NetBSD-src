/* $NetBSD$ */

/*
 * Copyright (c) 2024 Emile 'iMil' Heitor.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/param.h>
#include <xen/hypervisor.h>

volatile shared_info_t *HYPERVISOR_shared_info __read_mostly;
paddr_t HYPERVISOR_shared_info_pa;
union start_info_union start_info_union __aligned(PAGE_SIZE);
struct hvm_start_info *hvm_start_info;

uint32_t hvm_start_paddr;

void init_start_info(void);
void
init_start_info(void)
{
	const char *cmd_line;
	if (vm_guest != VM_GUEST_XENPVH && vm_guest != VM_GUEST_GENPVH)
		return;

	hvm_start_info = (void *)((uintptr_t)hvm_start_paddr + KERNBASE);

	if (hvm_start_info->cmdline_paddr != 0) {
	    cmd_line =
		(void *)((uintptr_t)hvm_start_info->cmdline_paddr + KERNBASE);
	    strlcpy(xen_start_info.cmd_line, cmd_line,
		sizeof(xen_start_info.cmd_line));
	} else {
		xen_start_info.cmd_line[0] = '\0';
	}
	xen_start_info.flags = hvm_start_info->flags;
}
