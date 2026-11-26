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

/*
 * exec_wasm32: binary format handler for WebAssembly userland.
 *
 * wasm32 cannot execute code loaded into linear memory: calls go
 * through the module's function table, so user programs are linked
 * into the kernel image itself.  The file in the root filesystem is a
 * 32-byte stub (magic + program name) that names a registered entry
 * function; everything above the loader (namei, vnode, execve,
 * vmspace, argv copyout) is the real MI machinery.
 *
 * The syscall ABI (v0) is direct calls from user programs to the
 * wasm_user_* entry points below: same module, same linear memory,
 * no protection boundary until the pmap work lands.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/cpu.h>
#include <sys/exec.h>
#include <sys/lwp.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/reboot.h>
#include <sys/systm.h>

#include <uvm/uvm_extern.h>

#include <machine/proc.h>

/*
 * Stub file format: the file in the filesystem is not the program,
 * it is a ticket naming a program linked into this module.
 */
#define	WASMUSER_MAGIC	"WASMUSER"
#define	WASMUSER_NAMESZ	24

struct wasmuser_hdr {
	char	magic[8];
	char	name[WASMUSER_NAMESZ];
};

CTASSERT(sizeof(struct wasmuser_hdr) == 32);

struct wasm_userprog {
	const char *name;
	void	(*entry)(void);
};

/* arch/wasm/user/init.c */
void	wasm_user_init_main(void);

/* syscall ABI v0, the kernel side */
void	wasm_user_puts(const char *);
void	wasm_user_reboot(void);
void	wasm_user_entry(void *);

static const struct wasm_userprog wasm_userprogs[] = {
	{ "init",	wasm_user_init_main },
};

/*
 * Syscall ABI v0, the kernel side: user programs call these directly.
 */
void
wasm_user_puts(const char *s)
{

	printf("%s", s);
}

void
wasm_user_reboot(void)
{

	cpu_reboot(RB_AUTOBOOT, NULL);
	for (;;)
		;
}

/*
 * Epoch entry wrapper: wasm_epoch_run() starts an LWP by calling
 * md_entryfunc(md_entryarg); setregs() in lwp_csw.c arms this pair
 * with the program's entry.  A program that returns has ended the
 * process: with init gone there is nothing left to run, so reboot
 * into the host.
 */
void
wasm_user_entry(void *arg)
{
	void (*main)(void) = (void (*)(void))arg;

	main();
	printf("user: init exited\n");
	wasm_user_reboot();
}

static const struct wasm_userprog *
wasm_userprog_lookup(const char *name)
{

	for (size_t i = 0; i < __arraycount(wasm_userprogs); i++) {
		/*
		 * Compare through the NUL: the stub's name field is
		 * zero-padded, and a plain 24-byte strncmp would also
		 * compare the rodata that follows the short literal.
		 */
		if (strncmp(wasm_userprogs[i].name, name,
		    strlen(wasm_userprogs[i].name) + 1) == 0)
			return &wasm_userprogs[i];
	}
	return NULL;
}

static int
exec_wasm32_makecmds(struct lwp *l, struct exec_package *epp)
{
	const struct wasmuser_hdr *hdr = epp->ep_hdr;
	const struct wasm_userprog *prog;
	int error;

	if (memcmp(hdr->magic, WASMUSER_MAGIC, sizeof(hdr->magic)) != 0)
		return ENOEXEC;
	prog = wasm_userprog_lookup(hdr->name);
	if (prog == NULL)
		return ENOEXEC;

	/*
	 * There is no image to map: ep_entry carries the entry
	 * function pointer (opaque vaddr_t round-trip; setregs()
	 * hands it to the epoch as md_entryarg).
	 */
	epp->ep_entry = (vaddr_t)(uintptr_t)prog->entry;
	epp->ep_vm_minaddr = VM_MIN_ADDRESS;
	epp->ep_vm_maxaddr = VM_MAX_ADDRESS;

	/*
	 * No text or data segments: the program is linked into the
	 * kernel image, so there is nothing to map for it.
	 */
	epp->ep_taddr = 0;
	epp->ep_tsize = 0;
	epp->ep_daddr = 0;
	epp->ep_dsize = 0;

	/*
	 * Set up ep_minsaddr/ep_ssize/ep_maxsaddr and the stack
	 * vmcmds.  The generic exec path leaves these to the format
	 * handler (the ELF loader calls this itself at the same
	 * point); without it they stay uninitialized.
	 */
	error = exec_setup_stack(l, epp);
	if (error != 0)
		return error;

	return 0;
}

static struct execsw exec_wasm32_execsw[] = {
	{
		.es_hdrsz = sizeof(struct wasmuser_hdr),
		.es_makecmds = exec_wasm32_makecmds,
		.es_emul = &emul_netbsd,
		.es_prio = EXECSW_PRIO_FIRST,
		.es_arglen = 0,
		.es_copyargs = copyargs,
		.es_setregs = NULL,
		.es_coredump = NULL,
		.es_setup_stack = exec_setup_stack,
	},
};

MODULE(MODULE_CLASS_EXEC, exec_wasm32, NULL);

static int
exec_wasm32_modcmd(modcmd_t cmd, void *arg)
{

	switch (cmd) {
	case MODULE_CMD_INIT:
		return exec_add(exec_wasm32_execsw,
		    __arraycount(exec_wasm32_execsw));

	case MODULE_CMD_FINI:
		return exec_remove(exec_wasm32_execsw,
		    __arraycount(exec_wasm32_execsw));

	default:
		return ENOTTY;
	}
}
