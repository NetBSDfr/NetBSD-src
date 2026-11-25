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

#define __UCAS_PRIVATE
#define __UFETCHSTORE_PRIVATE

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/uio.h>
#include <uvm/uvm.h>

/*
 * User/kernel address space crossers for the WebAssembly port.
 *
 * wasm32 runs with one flat linear address space and the port uses an
 * identity pmap, so every address is directly accessible from kernel
 * code; the copy functions are plain copies.  Access validation (a
 * user address must point at guest memory, not past the end of it)
 * is a TODO: wasm itself traps out-of-bounds accesses, and the host
 * will see the trap.
 */

int
copyin(const void *uaddr, void *kaddr, size_t len)
{

	memcpy(kaddr, uaddr, len);
	return 0;
}

int
copyout(const void *kaddr, void *uaddr, size_t len)
{

	memcpy(uaddr, kaddr, len);
	return 0;
}

int
copyinstr(const void *uaddr, void *kaddr, size_t maxlen, size_t *len)
{
	const char *u = uaddr;
	char *k = kaddr;
	size_t n = 0;
	int error = 0;

	for (;;) {
		if (n >= maxlen) {
			error = ENAMETOOLONG;
			break;
		}
		if ((k[n] = u[n]) == '\0')
			break;
		n++;
	}
	if (len != NULL)
		*len = n + 1;
	return error;
}

int
copyoutstr(const void *kaddr, void *uaddr, size_t maxlen, size_t *len)
{
	const char *k = kaddr;
	char *u = uaddr;
	size_t n = 0;
	int error = 0;

	for (;;) {
		if (n >= maxlen) {
			error = ENAMETOOLONG;
			break;
		}
		if ((u[n] = k[n]) == '\0')
			break;
		n++;
	}
	if (len != NULL)
		*len = n + 1;
	return error;
}

int
kcopy(const void *src, void *dst, size_t len)
{

	memcpy(dst, src, len);
	return 0;
}

/*
 * The identity pmap makes all kernel addresses, including those in
 * struct buf, directly usable; nothing to map in or out.
 */
int
vmapbuf(struct buf *bp, vsize_t size)
{

	return 1;
}

void
vunmapbuf(struct buf *bp, vsize_t size)
{
}

/*
 * Unaligned-safe single accessors for user atomics (see systm.h).
 * Single-threaded, non-preemptible within one wasm call: the
 * read/modify/write of _ucas_* needs no locking.
 */
int
_ufetch_8(const uint8_t *uaddr, uint8_t *valp)
{

	*valp = *uaddr;
	return 0;
}

int
_ufetch_16(const uint16_t *uaddr, uint16_t *valp)
{

	*valp = *uaddr;
	return 0;
}

int
_ufetch_32(const uint32_t *uaddr, uint32_t *valp)
{

	*valp = *uaddr;
	return 0;
}

int
_ustore_8(uint8_t *uaddr, uint8_t val)
{

	*uaddr = val;
	return 0;
}

int
_ustore_16(uint16_t *uaddr, uint16_t val)
{

	*uaddr = val;
	return 0;
}

int
_ustore_32(uint32_t *uaddr, uint32_t val)
{

	*uaddr = val;
	return 0;
}

int
_ucas_32(volatile uint32_t *uaddr, uint32_t old, uint32_t new, uint32_t *ret)
{

	*ret = *uaddr;
	if (*ret == old)
		*uaddr = new;
	return 0;
}
