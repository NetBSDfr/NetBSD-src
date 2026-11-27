/*	$NetBSD$	*/

/* i486 interpreter core; kernel glue in wasm_i486.c. */

/*
 * i486-class userland interpreter for the wasm port: runs unmodified
 * NetBSD/i386 binaries inside the wasm32 kernel (iSH model).
 * Portable C: no kernel or libc dependencies, host harness for tests.
 */

#ifndef	_EMUL_I486_H_
#define	_EMUL_I486_H_

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/cdefs.h>
#include <sys/null.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

#define	I486_CF		0x0001u
#define	I486_DF		0x0400u
#define	I486_IF		0x0200u
#define	I486_SF		0x0080u
#define	I486_ZF		0x0040u
#define	I486_OF		0x0800u

struct i486_mem {
	uint8_t *hbase;		/* host pointer of guest VA vbase */
	uintptr_t vbase;	/* guest VA of region start */
	uint32_t size;
};

/*
 * Translation model: guest VA g maps to hbase + (g - vbase) when in
 * bounds; i486_h() returns NULL otherwise.  The kernel glue allocates
 * one region per guest task and sets vbase to the ELF load base.
 */

struct i486_state {
	uint32_t regs[8];
	uint32_t eip;
	uint32_t eflags;
};

/* run results */
#define	I486_OK		0	/* stopped: callback asked (halt/exit) */
#define	I486_FAULT	1	/* bad guest address or unimplemented insn */
#define	I486_STEP	2	/* max_steps exhausted */

/*
 * Syscall bridge: guest executed int $0x80.  Handler reads the syscall
 * number from regs[R_EAX] and args from the guest stack at esp+4
 * (NetBSD/i386 stack-passing ABI), sets regs[R_EAX]/[R_EDX] and the
 * carry flag (CF = error).  Set *stop = 1 to end i486_run with I486_OK.
 */
struct i486_vm {
	struct i486_state s;
	struct i486_mem m;
	void (*syscall)(struct i486_vm *);
	int stop;
	uint64_t steps;
	uint32_t fault_va;	/* last failed access (debug aid) */
#ifdef I486_WATCH
	uint32_t watch_lo, watch_hi;
#endif

	/* segment bases: %gs is the TLS block (0 = uninitialised),
	 * %fs unused; seg holds the active prefix base. */
	uint32_t tls_base;
	uint32_t seg;

	/* guest-side bookkeeping used by the syscall bridge glue */
	uint32_t img_end;	/* page-aligned end of loaded image (gva) */
	uint32_t mmap_cur;	/* bump pointer for anonymous mmap (gva) */
	uint32_t mmap_end;	/* stack guard for the bump allocator */
};

uint8_t *i486_h(struct i486_mem *m, uint32_t va);	/* NULL if OOB */
int  i486_run(struct i486_vm *vm, uint64_t max_steps);

#endif	/* _EMUL_I486_H_ */
