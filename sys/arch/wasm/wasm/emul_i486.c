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
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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

#ifdef _KERNEL
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");
#endif

/*
 * i486-class interpreter core.  Flat 32-bit mode, no paging/segmentation:
 * guest VA -> host via hbase + (va - vbase).  Instruction subset
 * sufficient for NetBSD/i386 libc startup + syscall path; unknown insns
 * -> I486_FAULT.
 */

#include "emul_i486.h"

uint8_t *
i486_h(struct i486_mem *m, uint32_t va)
{

	return (va >= m->vbase && (uint32_t)(va - m->vbase) < m->size)
	    ? m->hbase + (va - m->vbase) : NULL;
}

static int
rd32(struct i486_vm *vm, uint32_t va, uint32_t *out)
{
	uint8_t *p = i486_h(&vm->m, va);

	if (p == NULL || (uint32_t)(va - vm->m.vbase) + 4 > vm->m.size)
		return -1;
	*out = (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
	return 0;
}

static int
wr32(struct i486_vm *vm, uint32_t va, uint32_t v)
{
	uint8_t *p = i486_h(&vm->m, va);

	if (p == NULL || (uint32_t)(va - vm->m.vbase) + 4 > vm->m.size)
		return -1;
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
	return 0;
}

static int
fetch8(struct i486_vm *vm, uint32_t *out)
{
	uint8_t *p = i486_h(&vm->m, vm->s.eip);

	if (p == NULL)
		return -1;
	*out = *p;
	vm->s.eip++;
	return 0;
}

static int
fetch32(struct i486_vm *vm, uint32_t *out)
{
	uint32_t v;
	int rv = rd32(vm, vm->s.eip, &v);

	if (rv == 0) {
		*out = v;
		vm->s.eip += 4;
	}
	return rv;
}

static void
push32(struct i486_vm *vm, uint32_t v)
{

	vm->s.regs[R_ESP] -= 4;
	wr32(vm, vm->s.regs[R_ESP], v);
}

static uint32_t
pop32(struct i486_vm *vm)
{
	uint32_t v = 0;

	rd32(vm, vm->s.regs[R_ESP], &v);
	vm->s.regs[R_ESP] += 4;
	return v;
}

static void
set_logic_flags(struct i486_vm *vm, uint32_t r)
{

	vm->s.eflags &= ~(I486_CF | I486_OF | I486_SF | I486_ZF);
	if (r == 0)
		vm->s.eflags |= I486_ZF;
	if (r & 0x80000000u)
		vm->s.eflags |= I486_SF;
}

static void
set_sub_flags(struct i486_vm *vm, uint32_t a, uint32_t b, uint32_t r)
{

	vm->s.eflags &= ~(I486_CF | I486_OF | I486_SF | I486_ZF);
	if (r == 0)
		vm->s.eflags |= I486_ZF;
	if (r & 0x80000000u)
		vm->s.eflags |= I486_SF;
	if (a < b)
		vm->s.eflags |= I486_CF;
	if (((a ^ b) & (a ^ r) & 0x80000000u) != 0)
		vm->s.eflags |= I486_OF;
}

static void
set_add_flags(struct i486_vm *vm, uint32_t a, uint32_t b, uint32_t r);

static int
cond_taken(struct i486_vm *vm, int cc)
{
	uint32_t f = vm->s.eflags;
	int cf = !!(f & I486_CF), zf = !!(f & I486_ZF);
	int sf = !!(f & I486_SF), of = !!(f & I486_OF);

	switch (cc) {
	case 0x0: return of;		/* O */
	case 0x1: return !of;		/* NO */
	case 0x2: return cf;		/* B */
	case 0x3: return !cf;		/* AE */
	case 0x4: return zf;		/* E */
	case 0x5: return !zf;		/* NE */
	case 0x6: return cf || zf;	/* BE */
	case 0x7: return !cf && !zf;	/* A */
	case 0x8: return sf;		/* S */
	case 0x9: return !sf;		/* NS */
	case 0xA: return 0;		/* P (untracked) */
	case 0xB: return 1;		/* NP */
	case 0xC: return sf != of;	/* L */
	case 0xD: return sf == of;	/* GE */
	case 0xE: return zf || sf != of;	/* LE */
	default:  return zf || sf == of;	/* G */
	}
}

/*
 * modrm decode.  Fills reg (reg field), rmreg (rm reg index when mod==3),
 * va (effective address when mod!=3).  Returns 0 / -1.
 */
struct rm {
	int reg;
	int rmreg;
	int isreg;
	uint32_t va;
};

static int
decode_modrm(struct i486_vm *vm, struct rm *rm)
{
	uint32_t m, sib = 0, disp = 0;
	int mod, scale = 0, idx = -1;

	if (fetch8(vm, &m))
		return -1;
	mod = m >> 6;
	rm->reg = (m >> 3) & 7;
	rm->isreg = (mod == 3);
	rm->rmreg = m & 7;
	rm->va = 0;

	if (mod == 3)
		return 0;

	if ((m & 7) == 4) {		/* SIB */
		if (fetch8(vm, &sib))
			return -1;
		scale = (sib >> 6) & 3;
		idx = (sib >> 3) & 7;
		if (idx == R_ESP)
			idx = -1;	/* no index encoded as ESP */
		rm->va = (sib & 7) == R_ESP ? 0 : vm->s.regs[sib & 7];
		if ((sib & 7) == R_ESP && mod == 0) {
			if (fetch32(vm, &disp))
				return -1;
			rm->va = disp;
		}
	} else if ((m & 7) == 5 && mod == 0) {
		if (fetch32(vm, &disp))
			return -1;
		rm->va = disp;
		return 0;
	} else
		rm->va = vm->s.regs[m & 7];

	if (idx >= 0)
		rm->va += vm->s.regs[idx] << scale;

	if (mod == 1) {
		uint32_t d8;
		if (fetch8(vm, &d8))
			return -1;
		rm->va += (uint32_t)(int32_t)(int8_t)d8;
	} else if (mod == 2) {
		if (fetch32(vm, &disp))
			return -1;
		rm->va += disp;
	}
	return 0;
}

/* read operand: register file or memory */
static int
rm_read(struct i486_vm *vm, struct rm *rm, uint32_t *out)
{

	if (rm->isreg) {
		*out = vm->s.regs[rm->rmreg];
		return 0;
	}
	return rd32(vm, rm->va, out);
}

static int
rm_write(struct i486_vm *vm, struct rm *rm, uint32_t v)
{

	if (rm->isreg) {
		vm->s.regs[rm->rmreg] = v;
		return 0;
	}
	return wr32(vm, rm->va, v);
}

int
i486_run(struct i486_vm *vm, uint64_t max_steps)
{
	struct i486_state *s = &vm->s;
	uint64_t n = 0;

	for (;;) {
		uint32_t op, op2, v, imm, addr;
		struct rm rm;

		if (vm->stop)
			return I486_OK;
		if (max_steps && ++n > max_steps)
			return I486_STEP;

		if (fetch8(vm, &op))
			return I486_FAULT;

		switch (op) {
		case 0x90:					/* nop */
			break;

		/* --- two-operand ALU r/m32,r32 / r32,r/m32 --- */
		case 0x01: case 0x03: case 0x09: case 0x0b:
		case 0x21: case 0x23: case 0x29: case 0x2b:
		case 0x31: case 0x33: case 0x39: case 0x3b:
		case 0x85: case 0x89: case 0x8b: case 0x8d: {
			uint32_t rmv, rv;
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (op == 0x8d) {			/* lea */
				s->regs[rm.reg] = rm.va;
				break;
			}
			if (rm_read(vm, &rm, &rmv))
				return I486_FAULT;
			rv = s->regs[rm.reg];
			switch (op) {
			case 0x89: rv = rm_write(vm, &rm, rv); break;
			case 0x8b: s->regs[rm.reg] = rmv; rv = 0; break;
			case 0x01: v = rmv + rv; set_add_flags(vm, rmv, rv, v);
				   rv = rm_write(vm, &rm, v); break;
			case 0x03: v = rv + rmv; set_add_flags(vm, rv, rmv, v);
				   s->regs[rm.reg] = v; rv = 0; break;
			case 0x29: v = rmv - rv; set_sub_flags(vm, rmv, rv, v);
				   rv = rm_write(vm, &rm, v); break;
			case 0x2b: v = rv - rmv; set_sub_flags(vm, rv, rmv, v);
				   s->regs[rm.reg] = v; rv = 0; break;
			case 0x31: v = rmv ^ rv; set_logic_flags(vm, v);
				   rv = rm_write(vm, &rm, v); break;
			case 0x33: v = rv ^ rmv; set_logic_flags(vm, v);
				   s->regs[rm.reg] = v; rv = 0; break;
			case 0x09: v = rmv | rv; set_logic_flags(vm, v);
				   rv = rm_write(vm, &rm, v); break;
			case 0x0b: v = rv | rmv; set_logic_flags(vm, v);
				   s->regs[rm.reg] = v; rv = 0; break;
			case 0x21: v = rmv & rv; set_logic_flags(vm, v);
				   rv = rm_write(vm, &rm, v); break;
			case 0x23: v = rv & rmv; set_logic_flags(vm, v);
				   s->regs[rm.reg] = v; rv = 0; break;
			case 0x39: set_sub_flags(vm, rmv, rv, rmv - rv); rv = 0; break;
			case 0x3b: set_sub_flags(vm, rv, rmv, rv - rmv); rv = 0; break;
			case 0x85: set_logic_flags(vm, rmv & rv); rv = 0; break;
			}
			if (rv == (uint32_t)-1 && op == 0x89)
				return I486_FAULT;
			break;
		}

		/* --- ALU r/m32, imm --- */
		case 0x83: case 0x81: {
			uint32_t rmv, res;
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (op == 0x83) {
				if (fetch8(vm, &imm))
					return I486_FAULT;
				imm = (uint32_t)(int32_t)(int8_t)imm;
			} else if (fetch32(vm, &imm))
				return I486_FAULT;
			if (rm_read(vm, &rm, &rmv))
				return I486_FAULT;
			res = rmv;
			switch (rm.reg) {	/* /digit */
			case 0: res = rmv + imm; set_add_flags(vm, rmv, imm, res); break;
			case 1: res = rmv | imm; set_logic_flags(vm, res); break;
			case 4: res = rmv & imm; set_logic_flags(vm, res); break;
			case 5: res = rmv - imm; set_sub_flags(vm, rmv, imm, res); break;
			case 6: res = rmv ^ imm; set_logic_flags(vm, res); break;
			case 7: set_sub_flags(vm, rmv, imm, rmv - imm);
				continue;
			default: return I486_FAULT;
			}
			if (rm_write(vm, &rm, res))
				return I486_FAULT;
			break;
		}

		/* --- ALU eax, imm32 --- */
		case 0x05: case 0x0d: case 0x25: case 0x2d:
		case 0x35: case 0x3d:
			if (fetch32(vm, &imm))
				return I486_FAULT;
			v = s->regs[R_EAX];
			switch (op) {
			case 0x05: addr = v + imm; set_add_flags(vm, v, imm, addr);
				   s->regs[R_EAX] = addr; break;
			case 0x0d: addr = v | imm; set_logic_flags(vm, addr);
				   s->regs[R_EAX] = addr; break;
			case 0x25: addr = v & imm; set_logic_flags(vm, addr);
				   s->regs[R_EAX] = addr; break;
			case 0x2d: addr = v - imm; set_sub_flags(vm, v, imm, addr);
				   s->regs[R_EAX] = addr; break;
			case 0x35: addr = v ^ imm; set_logic_flags(vm, addr);
				   s->regs[R_EAX] = addr; break;
			case 0x3d: set_sub_flags(vm, v, imm, v - imm); break;
			}
			break;

		/* --- push/pop reg --- */
		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57:
			push32(vm, s->regs[op - 0x50]);
			break;
		case 0x58: case 0x59: case 0x5a: case 0x5b:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f:
			s->regs[op - 0x58] = pop32(vm);
			break;

		case 0x68:					/* push imm32 */
			if (fetch32(vm, &v))
				return I486_FAULT;
			push32(vm, v);
			break;
		case 0x6a:					/* push imm8 */
			if (fetch8(vm, &v))
				return I486_FAULT;
			push32(vm, (uint32_t)(int32_t)(int8_t)v);
			break;

		/* --- mov r32, imm32 --- */
		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			if (fetch32(vm, &v))
				return I486_FAULT;
			s->regs[op - 0xb8] = v;
			break;

		case 0xe8:					/* call rel32 */
			if (fetch32(vm, &v))
				return I486_FAULT;
			push32(vm, s->eip);
			s->eip += (int32_t)v;
			break;
		case 0xe9:
			if (fetch32(vm, &v))
				return I486_FAULT;
			s->eip += (int32_t)v;
			break;
		case 0xeb:
			if (fetch8(vm, &v))
				return I486_FAULT;
			s->eip += (int32_t)(int8_t)v;
			break;

		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b:
		case 0x7c: case 0x7d: case 0x7e: case 0x7f:
			if (fetch8(vm, &v))
				return I486_FAULT;
			if (cond_taken(vm, op & 0xf))
				s->eip += (int32_t)(int8_t)v;
			break;

		case 0xc3:					/* ret */
			s->eip = pop32(vm);
			break;
		case 0xc2:					/* ret imm16 */
			s->eip = pop32(vm);
			if (fetch8(vm, &v) || fetch8(vm, &op2))
				return I486_FAULT;
			s->regs[R_ESP] += v | (op2 << 8);
			break;
		case 0xc9:					/* leave */
			s->regs[R_ESP] = s->regs[R_EBP];
			s->regs[R_EBP] = pop32(vm);
			break;

		case 0xff: {					/* /4 jmp /6 push */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (rm.reg != 4 && rm.reg != 6)
				return I486_FAULT;
			if (rm_read(vm, &rm, &v))
				return I486_FAULT;
			if (rm.reg == 4)
				s->eip = v;
			else
				push32(vm, v);
			break;
		}

		case 0xcd:					/* int imm8 */
			if (fetch8(vm, &v))
				return I486_FAULT;
			if (v != 0x80)
				return I486_FAULT;
			vm->syscall(vm);
			break;

		case 0xf4:					/* hlt */
			vm->stop = 1;
			return I486_OK;

		case 0x0f:
			if (fetch8(vm, &op2))
				return I486_FAULT;
			if (op2 >= 0x80 && op2 <= 0x8f) {	/* jcc rel32 */
				if (fetch32(vm, &v))
					return I486_FAULT;
				if (cond_taken(vm, op2 & 0xf))
					s->eip += (int32_t)v;
				break;
			}
			return I486_FAULT;

		default:
			return I486_FAULT;
		}
	}
}

static void
set_add_flags(struct i486_vm *vm, uint32_t a, uint32_t b, uint32_t r)
{

	vm->s.eflags &= ~(I486_CF | I486_OF | I486_SF | I486_ZF);
	if (r == 0)
		vm->s.eflags |= I486_ZF;
	if (r & 0x80000000u)
		vm->s.eflags |= I486_SF;
	if (r < a)
		vm->s.eflags |= I486_CF;
	if (((a ^ r) & (b ^ r) & 0x80000000u) != 0)
		vm->s.eflags |= I486_OF;
}
