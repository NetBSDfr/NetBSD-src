/*	$NetBSD$	*/
/*
 * E5b: x86 block -> wasm translator.  Kernel-portable C (no libc
 * beyond stdint).  Translates one straight-line guest block starting
 * at vm->s.eip into a wasm module exporting f(vm_ptr) -> i32:
 *   0 = block executed, state committed to vm, s.eip = next eip
 *  -1 = fault (nothing committed; interpreter must run the block)
 * Registers live in wasm locals during the block and commit at the
 * end, so a mid-block fault leaves vm untouched.
 *
 * Subset (32-bit only, no 0x66): mov/alu reg-reg + reg-imm, inc/dec,
 * push/pop, lea, jmp/jcc/call/ret, nop.  Anything else ends the block
 * (not included).  Block ends at first branch/syscall/unsupported.
 */
#ifndef _JIT_H_
#define _JIT_H_

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/cdefs.h>
#include <sys/null.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#include "emul_i486.h"

/* i486_vm field offsets (must match struct layout; asserted in jit.c) */
#define	JIT_O_EIP	32
#define	JIT_O_EFLAGS	36
#define	JIT_O_HBASE	40
#define	JIT_O_VBASE	44
#define	JIT_O_SIZE	48

#define	JIT_MAX_BLOCK	64		/* guest insns per block */

struct jit_out {
	uint8_t *buf;
	size_t cap;
	size_t len;
	int error;
	int stores;		/* block writes guest memory (verify-unsafe) */
};

/*
 * Translate the block at vm->s.eip.  Returns number of guest
 * instructions consumed (>0 on success, 0 if the first instruction is
 * unsupported), writes the module into out.  *ninsns also returned.
 */
int jit_translate(struct i486_vm *vm, struct jit_out *out, int *ninsns);

#endif /* _JIT_H_ */
