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

#ifndef _WASM_ROOTIMG_H_
#define _WASM_ROOTIMG_H_

/*
 * Root filesystem staging ABI between the kernel and the host loader.
 *
 * The host writes a raw FFS image into the linear memory at
 * WASM_ROOTIMG_ADDR before calling kernel_boot(); the kernel keeps
 * that span out of uvm's reach (pmap_bootstrap) and points md0 at it
 * via md_root_setconf() (dev/md.c with MEMORY_DISK_DYNAMIC +
 * MEMORY_DISK_HOOKS).  The host loader parses the two constants below
 * out of this header, so this file is the single source of truth.
 *
 * The current image is the 'rescueroot' GPT partition of
 * ~/src/smolBSD/images/rescue-amd64.img (a plain FFSv1 filesystem
 * whose superblock sits at SBLOCK_UFS1 of the partition itself).
 */

#define	WASM_ROOTIMG_ADDR	0x18000000u	/* 384MB into linear memory */
#define	WASM_ROOTIMG_SIZE	0x01300000u	/* 19922944 bytes */

/*
 * The rest of the wasm32 flat address map.  There is no MMU: every
 * address the kernel allocates is the memory it touches, so every
 * region must lie inside the linear memory and below the root image:
 *
 *   [0, __heap_base)                 kernel image, data, bss
 *   [__heap_base, WASM_KERNEL_ARENA_END)   kernel dynamic: kmem, kva,
 *                                          pools, uvm pages (pmap_
 *                                          bootstrap and
 *                                          pmap_virtual_space cap here)
 *   [WASM_KERNEL_ARENA_END, USRSTACK]      user address space (init's
 *                                          stack grows down from
 *                                          USRSTACK = VM_MAXUSER_ADDRESS,
 *                                          see include/vmparam.h)
 *   [WASM_ROOTIMG_ADDR, +SIZE)       host-staged root image (never uvm)
 */
#define	WASM_KERNEL_ARENA_END	0x14000000u	/* 320MB */
#define	WASM_USER_STACK_MAX	0x17800000u	/* 376MB: USRSTACK top */

#endif /* _WASM_ROOTIMG_H_ */
