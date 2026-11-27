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
 * i486-class interpreter core.  Flat 32-bit mode, no paging: guest VA
 * -> host via hbase + (va - vbase).  %gs is emulated as the TLS block
 * (vm->tls_base); lock prefixes execute non-atomically (single guest);
 * rep/repne string ops and 16-bit operand size are supported.
 * Unknown insns -> I486_FAULT.
 */

#include "emul_i486.h"

#define	FL_W	(I486_CF | I486_SF | I486_ZF | I486_OF)

uint8_t *
i486_h(struct i486_mem *m, uint32_t va)
{

	return (va >= m->vbase && (uint32_t)(va - m->vbase) < m->size)
	    ? m->hbase + (va - m->vbase) : NULL;
}

static uint32_t
sz_mask(int sz)
{

	return sz == 1 ? 0xffu : sz == 2 ? 0xffffu : 0xffffffffu;
}

static uint32_t
sz_sign(int sz)
{

	return 1u << (sz * 8 - 1);
}

static uint32_t
sext(uint32_t v, int sz)
{
	uint32_t bit = sz_sign(sz);

	return (v & bit) ? v | ~sz_mask(sz) : v & sz_mask(sz);
}

static int
rd_mem(struct i486_vm *vm, uint32_t va, int sz, uint32_t *out)
{
	uint8_t *p = i486_h(&vm->m, va);
	int i;

	if (p == NULL || (uint32_t)(va - vm->m.vbase) + sz > vm->m.size) {
		vm->fault_va = va;
		return -1;
	}
	*out = 0;
	for (i = 0; i < sz; i++)
		*out |= (uint32_t)p[i] << (8 * i);
#ifdef I486_WATCH
	if (vm->watch_lo && va >= vm->watch_lo && va < vm->watch_hi) {
		extern void i486_watch(struct i486_vm *, uint32_t, int,
		    uint32_t, int);
		i486_watch(vm, va, sz, *out, 0);
	}
#endif
	return 0;
}

static int
wr_mem(struct i486_vm *vm, uint32_t va, int sz, uint32_t v)
{
	uint8_t *p = i486_h(&vm->m, va);
	int i;

	if (p == NULL || (uint32_t)(va - vm->m.vbase) + sz > vm->m.size) {
		vm->fault_va = va;
		return -1;
	}
	for (i = 0; i < sz; i++)
		p[i] = v >> (8 * i);
#ifdef I486_WATCH
	if (vm->watch_lo && va >= vm->watch_lo && va < vm->watch_hi) {
		extern void i486_watch(struct i486_vm *, uint32_t, int,
		    uint32_t, int);
		i486_watch(vm, va, sz, v, 1);
	}
#endif
	return 0;
}

static int
fetch8(struct i486_vm *vm, uint32_t *out)
{
	uint8_t *p = i486_h(&vm->m, vm->s.eip);

	if (p == NULL) {
		vm->fault_va = vm->s.eip;
		return -1;
	}
	*out = *p;
	vm->s.eip++;
	return 0;
}

static int
fetch16(struct i486_vm *vm, uint32_t *out)
{
	uint32_t lo, hi;

	if (fetch8(vm, &lo))
		return -1;
	if (fetch8(vm, &hi))
		return -1;
	*out = lo | (hi << 8);
	return 0;
}

static int
fetch32(struct i486_vm *vm, uint32_t *out)
{

	if (rd_mem(vm, vm->s.eip, 4, out))
		return -1;
	vm->s.eip += 4;
	return 0;
}

static int
fetch_imm(struct i486_vm *vm, int sz, uint32_t *out)
{

	if (sz == 1)
		return fetch8(vm, out);
	if (sz == 2)
		return fetch16(vm, out);
	return fetch32(vm, out);
}

static void
push32(struct i486_vm *vm, uint32_t v)
{

	vm->s.regs[R_ESP] -= 4;
	wr_mem(vm, vm->s.regs[R_ESP], 4, v);
}

static uint32_t
pop32(struct i486_vm *vm)
{
	uint32_t v = 0;

	rd_mem(vm, vm->s.regs[R_ESP], 4, &v);
	vm->s.regs[R_ESP] += 4;
	return v;
}

/* register file access with operand size (byte regs include ah..bh) */
static uint32_t
reg_r(struct i486_vm *vm, int idx, int sz)
{
	uint32_t r;

	if (sz == 4)
		return vm->s.regs[idx];
	if (sz == 2)
		return vm->s.regs[idx] & 0xffff;
	r = idx < 4 ? vm->s.regs[idx] : vm->s.regs[idx - 4] >> 8;
	return r & 0xff;
}

static void
reg_w(struct i486_vm *vm, int idx, int sz, uint32_t v)
{

	if (sz == 4) {
		vm->s.regs[idx] = v;
	} else if (sz == 2) {
		vm->s.regs[idx] = (vm->s.regs[idx] & ~0xffffu) | (v & 0xffff);
	} else if (idx < 4) {
		vm->s.regs[idx] = (vm->s.regs[idx] & ~0xffu) | (v & 0xff);
	} else {
		idx -= 4;
		vm->s.regs[idx] = (vm->s.regs[idx] & ~0xff00u) |
		    ((v & 0xff) << 8);
	}
}

/* flags: results are passed pre-masked to sz */
static void
set_logic_flags(struct i486_vm *vm, uint32_t r, int sz)
{

	vm->s.eflags &= ~FL_W;
	if ((r & sz_mask(sz)) == 0)
		vm->s.eflags |= I486_ZF;
	if (r & sz_sign(sz))
		vm->s.eflags |= I486_SF;
}

static void
set_add_flags(struct i486_vm *vm, uint64_t a, uint64_t b, uint32_t r, int sz)
{
	uint32_t m = sz_mask(sz), sign = sz_sign(sz);
	uint32_t a32 = (uint32_t)a & m, b32 = (uint32_t)b & m;

	r &= m;
	vm->s.eflags &= ~FL_W;
	if (r == 0)
		vm->s.eflags |= I486_ZF;
	if (r & sign)
		vm->s.eflags |= I486_SF;
	if ((uint64_t)a32 + b > m)
		vm->s.eflags |= I486_CF;
	if (((a32 ^ r) & (b32 ^ r) & sign) != 0)
		vm->s.eflags |= I486_OF;
}

static void
set_sub_flags(struct i486_vm *vm, uint64_t a, uint64_t b, uint32_t r, int sz)
{
	uint32_t m = sz_mask(sz), sign = sz_sign(sz);
	uint32_t a32 = (uint32_t)a & m, b32 = (uint32_t)b & m;

	r &= m;
	vm->s.eflags &= ~FL_W;
	if (r == 0)
		vm->s.eflags |= I486_ZF;
	if (r & sign)
		vm->s.eflags |= I486_SF;
	if (a32 < b)
		vm->s.eflags |= I486_CF;
	if (((a32 ^ b32) & (a32 ^ r) & sign) != 0)
		vm->s.eflags |= I486_OF;
}

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
	case 0xA: return 0;		/* P (PF untracked) */
	case 0xB: return 1;		/* NP */
	case 0xC: return sf != of;	/* L */
	case 0xD: return sf == of;	/* GE */
	case 0xE: return zf || sf != of;	/* LE */
	default:  return !zf && sf == of;	/* G */
	}
}

/*
 * modrm decode.  reg = reg field; rmreg/isreg/va describe the r/m
 * operand; memory effective addresses get the active segment base.
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
		if ((sib & 7) == R_EBP && mod == 0) {
			/* base field means disp32 */
			if (fetch32(vm, &disp))
				return -1;
			rm->va = disp;
		} else
			rm->va = vm->s.regs[sib & 7];	/* incl. esp */
	} else if ((m & 7) == 5 && mod == 0) {
		if (fetch32(vm, &disp))
			return -1;
		rm->va = disp;
		rm->va += vm->seg;
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
	rm->va += vm->seg;
	return 0;
}

static int
rm_read(struct i486_vm *vm, struct rm *rm, int sz, uint32_t *out)
{

	if (rm->isreg) {
		*out = reg_r(vm, rm->rmreg, sz);
		return 0;
	}
	return rd_mem(vm, rm->va, sz, out);
}

static int
rm_write(struct i486_vm *vm, struct rm *rm, int sz, uint32_t v)
{

	if (rm->isreg) {
		reg_w(vm, rm->rmreg, sz, v);
		return 0;
	}
	return wr_mem(vm, rm->va, sz, v);
}

/* ALU cores: alu = (opcode >> 3) & 7 */
static int
alu_exec(struct i486_vm *vm, int alu, uint32_t a, uint32_t b, int sz,
    uint32_t *res)
{
	uint32_t m = sz_mask(sz), r, cf = vm->s.eflags & I486_CF;

	switch (alu) {
	case 0: /* add */
		r = (a + b) & m;
		set_add_flags(vm, a, b, r, sz);
		break;
	case 1: /* or */
		r = (a | b) & m;
		set_logic_flags(vm, r, sz);
		break;
	case 2: /* adc */
		r = (a + b + cf) & m;
		set_add_flags(vm, a, (uint64_t)b + cf, r, sz);
		break;
	case 3: /* sbb */
		r = (a - b - cf) & m;
		set_sub_flags(vm, a, (uint64_t)b + cf, r, sz);
		break;
	case 4: /* and */
		r = a & b;
		set_logic_flags(vm, r, sz);
		vm->s.eflags &= ~(I486_CF | I486_OF);
		break;
	case 5: /* sub */
		r = (a - b) & m;
		set_sub_flags(vm, a, b, r, sz);
		break;
	case 6: /* xor */
		r = (a ^ b) & m;
		set_logic_flags(vm, r, sz);
		vm->s.eflags &= ~(I486_CF | I486_OF);
		break;
	default: /* cmp */
		r = (a - b) & m;
		set_sub_flags(vm, a, b, r, sz);
		*res = a;
		return 1;		/* no writeback */
	}
	*res = r;
	return 0;
}

/* shift/rotate group: dir 0 left, 1 right; type 0 shl/shr, 1 sar */
static int
shift_exec(struct i486_vm *vm, struct rm *rm, int sz, uint32_t cnt, int dir,
    int arith)
{
	uint32_t v, r, m = sz_mask(sz), sign = sz_sign(sz), cf;

	if (rm_read(vm, rm, sz, &v))
		return -1;
	cnt &= 0x1f;
	if (cnt == 0)
		return 0;
	v &= m;
	cf = 0;
	if (dir == 0) {			/* shl */
		r = (v << cnt) & m;
		if (cnt <= sz * 8)
			cf = (v >> (sz * 8 - cnt)) & 1;
	} else if (arith && (v & sign)) {
		r = ((v | ~m) >> cnt) & m;
		cf = (v >> (cnt - 1)) & 1;
	} else {
		r = (v >> cnt) & m;
		cf = (v >> (cnt - 1)) & 1;
	}
	vm->s.eflags &= ~I486_OF;
	if (cnt == 1 && dir == 0 && ((r ^ v) & sign))
		vm->s.eflags |= I486_OF;
	set_logic_flags(vm, r, sz);
	vm->s.eflags = (vm->s.eflags & ~I486_CF) | (cf ? I486_CF : 0);
	return rm_write(vm, rm, sz, r);
}

/* f6/f7 group: test/not/neg/mul/imul/div/idiv */
static int
grp_unary(struct i486_vm *vm, struct rm *rm, int sz, int sub,
    uint32_t imm)
{
	uint32_t v, r, m = sz_mask(sz);
	uint64_t acc, dvd, q;

	if (sub <= 1) {		/* test */
		if (rm_read(vm, rm, sz, &v))
			return -1;
		set_logic_flags(vm, v & imm, sz);
		vm->s.eflags &= ~(I486_CF | I486_OF);
		return 0;
	}
	if (rm_read(vm, rm, sz, &v))
		return -1;
	switch (sub) {
	case 2: /* not */
		return rm_write(vm, rm, sz, (~v) & m);
	case 3: /* neg */
		r = (0 - v) & m;
		set_sub_flags(vm, 0, v, r, sz);
		return rm_write(vm, rm, sz, r);
	case 4: /* mul */
	case 5: /* imul */
		if (sz == 1) {
			acc = sub == 4 ? (uint64_t)reg_r(vm, R_EAX, 1) * v :
			    (uint64_t)(int64_t)(int8_t)reg_r(vm, R_EAX, 1) *
			    (int64_t)(int8_t)v;
			acc &= 0xffff;
			reg_w(vm, R_EAX, 2, acc);
			vm->s.eflags &= ~(I486_CF | I486_OF);
			if (acc & 0xff00)
				vm->s.eflags |= I486_CF | I486_OF;
			return 0;
		}
		if (sz == 2) {
			uint32_t ax = reg_r(vm, R_EAX, 2);
			acc = sub == 4 ? (uint64_t)ax * v :
			    (uint64_t)(int64_t)(int16_t)ax *
			    (int64_t)(int16_t)v;
			acc &= 0xffffffffu;
			reg_w(vm, R_EAX, 2, acc & 0xffff);
			reg_w(vm, R_EDX, 2, acc >> 16);
			vm->s.eflags &= ~(I486_CF | I486_OF);
			if (acc >> 16)
				vm->s.eflags |= I486_CF | I486_OF;
			return 0;
		}
		acc = sub == 4 ?
		    (uint64_t)vm->s.regs[R_EAX] * v :
		    (uint64_t)(int64_t)(int32_t)vm->s.regs[R_EAX] *
		    (int64_t)(int32_t)v;
		vm->s.regs[R_EAX] = acc & 0xffffffffu;
		vm->s.regs[R_EDX] = acc >> 32;
		vm->s.eflags &= ~(I486_CF | I486_OF);
		if (vm->s.regs[R_EDX])
			vm->s.eflags |= I486_CF | I486_OF;
		return 0;
	case 6: /* div */
	case 7: /* idiv */
		if (sz == 1) {
			dvd = reg_r(vm, R_EAX, 2);
			if (v == 0)
				return -1;
			if (sub == 6) {
				q = dvd / v;
				if (q > 0xff)
					return -1;
				r = (q << 8) | (dvd % v);
				reg_w(vm, R_EAX, 2, r);
			} else {
				int64_t n = (int64_t)(int16_t)dvd, d =
				    (int8_t)v, qq = n / d;
				if (qq > 127 || qq < -128)
					return -1;
				r = (((uint32_t)qq & 0xff) << 8) |
				    ((uint32_t)(n % d) & 0xff);
				reg_w(vm, R_EAX, 2, r);
			}
			return 0;
		}
		if (sz == 2) {
			uint32_t ax = reg_r(vm, R_EAX, 2), dx =
			    reg_r(vm, R_EDX, 2);
			dvd = ((uint64_t)dx << 16) | ax;
			if (v == 0)
				return -1;
			if (sub == 6) {
				q = dvd / v;
				if (q > 0xffff)
					return -1;
				reg_w(vm, R_EAX, 2, q);
				reg_w(vm, R_EDX, 2, dvd % v);
			} else {
				int64_t n = (int64_t)(int32_t)(uint32_t)dvd;
				int32_t d = (int16_t)v;
				int64_t qq = n / d;
				if (qq > 32767 || qq < -32768)
					return -1;
				reg_w(vm, R_EAX, 2, (uint32_t)qq);
				reg_w(vm, R_EDX, 2, (uint32_t)(n % d));
			}
			return 0;
		}
		if (v == 0)
			return -1;
		dvd = ((uint64_t)vm->s.regs[R_EDX] << 32) | vm->s.regs[R_EAX];
		if (sub == 6) {
			q = dvd / v;
			if (q > 0xffffffffu)
				return -1;
			vm->s.regs[R_EAX] = q;
			vm->s.regs[R_EDX] = dvd % v;
		} else {
			int64_t n = (int64_t)dvd, d = (int32_t)v, qq = n / d;
			if (qq > 0x7fffffffLL || qq < -0x80000000LL)
				return -1;
			vm->s.regs[R_EAX] = (uint32_t)qq;
			vm->s.regs[R_EDX] = (uint32_t)(n % d);
		}
		return 0;
	}
	return -1;
}

/* string op step; returns -1 on bad address */
static int
string_step(struct i486_vm *vm, int op, int sz, int step)
{
	uint32_t a, b, v;

	switch (op) {
	case 0xa4: case 0xa5:		/* movs */
		if (rd_mem(vm, vm->s.regs[R_ESI] + vm->seg, sz, &v) ||
		    wr_mem(vm, vm->s.regs[R_EDI], sz, v))
			return -1;
		vm->s.regs[R_ESI] += step;
		vm->s.regs[R_EDI] += step;
		break;
	case 0xaa: case 0xab:		/* stos */
		v = reg_r(vm, R_EAX, sz);
		if (wr_mem(vm, vm->s.regs[R_EDI], sz, v))
			return -1;
		vm->s.regs[R_EDI] += step;
		break;
	case 0xac: case 0xad:		/* lods */
		if (rd_mem(vm, vm->s.regs[R_ESI] + vm->seg, sz, &v))
			return -1;
		reg_w(vm, R_EAX, sz, v);
		vm->s.regs[R_ESI] += step;
		break;
	case 0xa6: case 0xa7:		/* cmps */
		if (rd_mem(vm, vm->s.regs[R_ESI] + vm->seg, sz, &a) ||
		    rd_mem(vm, vm->s.regs[R_EDI], sz, &b))
			return -1;
		set_sub_flags(vm, a, b, (a - b) & sz_mask(sz), sz);
		vm->s.regs[R_ESI] += step;
		vm->s.regs[R_EDI] += step;
		break;
	case 0xae: case 0xaf:		/* scas */
		if (rd_mem(vm, vm->s.regs[R_EDI], sz, &b))
			return -1;
		a = reg_r(vm, R_EAX, sz);
		set_sub_flags(vm, a, b, (a - b) & sz_mask(sz), sz);
		vm->s.regs[R_EDI] += step;
		break;
	default:
		return -1;
	}
	return 0;
}

int
i486_run(struct i486_vm *vm, uint64_t max_steps)
{
	struct i486_state *s = &vm->s;
	uint64_t n = 0;

	for (;;) {
		uint32_t op, op2, v, imm = 0, addr, cf32;
		struct rm rm;
		int sz, rep, alu, sub;

		if (vm->stop)
			return I486_OK;
		if (++n > max_steps && max_steps)
			return I486_STEP;
		vm->steps = n;

		sz = 4;
		rep = 0;
		vm->seg = 0;
		for (;;) {
			if (fetch8(vm, &op))
				return I486_FAULT;
			if (op == 0x66)
				sz = 2;
			else if (op == 0x67)
				;	/* address size: flat, ignore */
			else if (op == 0x64 || op == 0x65)
				vm->seg = vm->tls_base;	/* fs/gs: TLS on i386 */
			else if (op == 0x2e || op == 0x36 ||
			    op == 0x3e || op == 0x26)
				vm->seg = 0;
			else if (op == 0xf0)
				;	/* lock: single guest, no-op */
			else if (op == 0xf3)
				rep = 1;
			else if (op == 0xf2)
				rep = 2;
			else
				break;
		}

		switch (op) {
		case 0x90:				/* nop / xchg eax */
			break;

		case 0x00: case 0x01: case 0x02: case 0x03:
		case 0x08: case 0x09: case 0x0a: case 0x0b:
		case 0x10: case 0x11: case 0x12: case 0x13:
		case 0x18: case 0x19: case 0x1a: case 0x1b:
		case 0x20: case 0x21: case 0x22: case 0x23:
		case 0x28: case 0x29: case 0x2a: case 0x2b:
		case 0x30: case 0x31: case 0x32: case 0x33:
		case 0x38: case 0x39: case 0x3a: case 0x3b: {
			/* ALU r/m, r and r, r/m */
			int dstreg;
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			alu = (op >> 3) & 7;
			sz = (op & 1) ? sz : 1;
			dstreg = (op & 2) == 0;	/* 00/01: rm = dst */
			if (dstreg) {
				if (rm_read(vm, &rm, sz, &v))
					return I486_FAULT;
				imm = reg_r(vm, rm.reg, sz);
			} else {
				v = reg_r(vm, rm.reg, sz);
				if (rm_read(vm, &rm, sz, &imm))
					return I486_FAULT;
			}
			if (alu_exec(vm, alu, v, imm, sz, &addr))
				break;		/* cmp */
			if (dstreg) {
				if (rm_write(vm, &rm, sz, addr))
					return I486_FAULT;
			} else
				reg_w(vm, rm.reg, sz, addr);
			break;
		}

		case 0x04: case 0x0c: case 0x14: case 0x1c:
		case 0x24: case 0x2c: case 0x34: case 0x3c:
		case 0x05: case 0x0d: case 0x15: case 0x1d:
		case 0x25: case 0x2d: case 0x35: case 0x3d: {
			/* ALU eAX, imm */
			int is8 = (op & 3) == 0;
			sz = is8 ? 1 : sz;
			if (fetch_imm(vm, sz, &imm))
				return I486_FAULT;
			v = reg_r(vm, R_EAX, sz);
			alu = (op >> 3) & 7;
			if (alu_exec(vm, alu, v, imm, sz, &addr))
				break;
			reg_w(vm, R_EAX, sz, addr);
			break;
		}

		case 0x40: case 0x41: case 0x42: case 0x43:
		case 0x44: case 0x45: case 0x46: case 0x47:
		case 0x48: case 0x49: case 0x4a: case 0x4b:
		case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
			/* inc/dec reg32 (CF preserved) */
			int r = op & 7;
			uint32_t old = reg_r(vm, r, sz);

			cf32 = s->eflags & I486_CF;
			v = (op < 0x48) ? old + 1 : old - 1;
			if (op < 0x48)
				set_add_flags(vm, old, 1, v, sz);
			else
				set_sub_flags(vm, old, 1, v, sz);
			s->eflags = (s->eflags & ~I486_CF) | cf32;
			reg_w(vm, r, sz, v);
			break;
		}
		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57:
			push32(vm, vm->s.regs[op & 7]);
			break;
		case 0x58: case 0x59: case 0x5a: case 0x5b:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f:
			vm->s.regs[op & 7] = pop32(vm);
			break;

		case 0x68:				/* push imm */
			if (fetch_imm(vm, sz, &imm))
				return I486_FAULT;
			push32(vm, sz == 4 ? imm : sext(imm, sz));
			break;
		case 0x6a:				/* push imm8 */
			if (fetch8(vm, &imm))
				return I486_FAULT;
			push32(vm, (uint32_t)(int32_t)(int8_t)imm);
			break;

		case 0x69: case 0x6b: {			/* imul r, rm, imm */
			int32_t a, b, c;
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (rm_read(vm, &rm, 4, &v))
				return I486_FAULT;
			if (op == 0x6b) {
				if (fetch8(vm, &imm))
					return I486_FAULT;
				imm = (uint32_t)(int32_t)(int8_t)imm;
			} else if (fetch32(vm, &imm))
				return I486_FAULT;
			a = (int32_t)v; b = (int32_t)imm;
			c = (int32_t)((uint32_t)a * (uint32_t)b);
			reg_w(vm, rm.reg, 4, c);
			vm->s.eflags &= ~(I486_CF | I486_OF);
			if ((int64_t)a * b != c)
				vm->s.eflags |= I486_CF | I486_OF;
			break;
		}

		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b:
		case 0x7c: case 0x7d: case 0x7e: case 0x7f:
			if (fetch8(vm, &v))
				return I486_FAULT;
			if (cond_taken(vm, op & 0xf))
				s->eip += (int32_t)(int8_t)v;
			break;

		case 0x80: case 0x81: case 0x82: case 0x83: {
		/* group1: rm, imm */
		if (decode_modrm(vm, &rm))
				return I486_FAULT;
		alu = rm.reg;
		if (op == 0x81) {
				if (fetch_imm(vm, sz, &imm))
					return I486_FAULT;
		} else {
				if (fetch8(vm, &imm))
					return I486_FAULT;
				if (op == 0x83)
					imm = (uint32_t)(int32_t)(int8_t)imm;
				else
					sz = 1;
		}
			if (rm_read(vm, &rm, sz, &v))
				return I486_FAULT;
			if (alu_exec(vm, alu, v, imm, sz, &addr))
				break;
			if (rm_write(vm, &rm, sz, addr))
				return I486_FAULT;
			break;
		}

		case 0x84: case 0x85:			/* test */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			sz = (op & 1) ? sz : 1;
			if (rm_read(vm, &rm, sz, &v))
				return I486_FAULT;
			set_logic_flags(vm, v & reg_r(vm, rm.reg, sz), sz);
			vm->s.eflags &= ~(I486_CF | I486_OF);
			break;

		case 0x86: case 0x87: {			/* xchg */
			uint32_t t;
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			sz = (op & 1) ? sz : 1;
			if (rm_read(vm, &rm, sz, &v))
				return I486_FAULT;
			t = reg_r(vm, rm.reg, sz);
			if (rm_write(vm, &rm, sz, t))
				return I486_FAULT;
			reg_w(vm, rm.reg, sz, v);
			break;
		}

		case 0x88: case 0x89: case 0x8a: case 0x8b:	/* mov */
		case 0x8c: {
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (op == 0x8c) {		/* mov rm, sreg */
				uint32_t sv = rm.reg == 3 ? vm->tls_base : 0;
				if (rm_write(vm, &rm, 2, sv))
					return I486_FAULT;
				break;
			}
			sz = (op & 1) ? sz : 1;
			if (op == 0x88 || op == 0x89) {	/* rm = reg */
				if (rm_write(vm, &rm, sz,
				    reg_r(vm, rm.reg, sz)))
					return I486_FAULT;
			} else {
				if (rm_read(vm, &rm, sz, &v))
					return I486_FAULT;
				reg_w(vm, rm.reg, sz, v);
			}
			break;
		}

		case 0x8d:				/* lea */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			reg_w(vm, rm.reg, 4, rm.va);
			break;

		case 0x8f:				/* pop rm */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (rm_write(vm, &rm, 4, pop32(vm)))
				return I486_FAULT;
			break;

		case 0x91: case 0x92: case 0x93:
		case 0x94: case 0x95: case 0x96:
		case 0x97:				/* xchg eax, r */
			v = s->regs[R_EAX];
			s->regs[R_EAX] = s->regs[op & 7];
			s->regs[op & 7] = v;
			break;

		case 0x98:				/* cbw / cwde */
			if (sz == 2)
				reg_w(vm, R_EAX, 2, (uint32_t)(int32_t)
				    (int8_t)reg_r(vm, R_EAX, 1));
			else
				s->regs[R_EAX] = (uint32_t)(int32_t)(int16_t)
				    reg_r(vm, R_EAX, 2);
			break;
		case 0x99:				/* cwd / cdq */
			if (sz == 2)
				reg_w(vm, R_EDX, 2, (reg_r(vm, R_EAX, 1) &
				    0x80) ? 0xff : 0);
			else
				s->regs[R_EDX] = (s->regs[R_EAX] & 0x80000000u)
				    ? 0xffffffffu : 0;
			break;

		case 0x9b:				/* fwait: nop */
			break;
		case 0x9c:				/* pushf */
			push32(vm, (s->eflags & 0xffffffffu) | 2);
			break;
		case 0x9d:				/* popf */
			s->eflags = pop32(vm) & 0x00fcffffu;
			break;

		case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
			/* mov moffs <-> acc */
			int szm = (op & 1) ? sz : 1;
			if (fetch32(vm, &addr))
				return I486_FAULT;
			addr += vm->seg;
			if (op & 2) {			/* acc -> moffs */
				if (wr_mem(vm, addr, szm,
				    reg_r(vm, R_EAX, szm)))
					return I486_FAULT;
			} else {
				if (rd_mem(vm, addr, szm, &v))
					return I486_FAULT;
				reg_w(vm, R_EAX, szm, v);
			}
			break;
		}

		case 0xa4: case 0xa5: case 0xa6: case 0xa7:
		case 0xaa: case 0xab: case 0xac: case 0xad:
		case 0xae: case 0xaf: {
			int step = (s->eflags & I486_DF) ? -1 : 1;
			sz = (op & 1) ? sz : 1;
			step *= sz;
			if (rep) {
				/* ZF condition only for cmps/scas */
				int zc = op == 0xa6 || op == 0xa7 ||
				    op == 0xae || op == 0xaf;
				while (s->regs[R_ECX] != 0) {
					s->regs[R_ECX]--;
					if (string_step(vm, op, sz, step))
						return I486_FAULT;
					/* F3=repne: stop when equal (ZF=1);
					 * F2=repe: stop when unequal (ZF=0) */
					if (zc && rep == 1 &&
					    (s->eflags & I486_ZF))
						break;
					if (zc && rep == 2 &&
					    !(s->eflags & I486_ZF))
						break;
				}
			} else if (string_step(vm, op, sz, step))
				return I486_FAULT;
			break;
		}

		case 0xa8: case 0xa9:			/* test acc, imm */
			sz = (op & 1) ? sz : 1;
			if (fetch_imm(vm, sz, &imm))
				return I486_FAULT;
			set_logic_flags(vm, reg_r(vm, R_EAX, sz) & imm, sz);
			vm->s.eflags &= ~(I486_CF | I486_OF);
			break;

		case 0xb0: case 0xb1: case 0xb2: case 0xb3:
		case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			sz = (op & 8) ? sz : 1;
			if (fetch_imm(vm, sz, &imm))
				return I486_FAULT;
			reg_w(vm, op & 7, sz, imm);
			break;

		case 0xc0: case 0xc1:			/* shift, imm8 */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			sz = (op & 1) ? sz : 1;
			if (fetch8(vm, &imm))
				return I486_FAULT;
			if (shift_exec(vm, &rm, sz, imm, rm.reg & 1,
			    rm.reg == 7))
				return I486_FAULT;
			break;

		case 0xc2:				/* ret imm16 */
			s->eip = pop32(vm);
			if (fetch16(vm, &v))
				return I486_FAULT;
			s->regs[R_ESP] += v;
			break;
		case 0xc3:				/* ret */
			s->eip = pop32(vm);
			break;

		case 0xc6: case 0xc7:			/* mov rm, imm */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			sz = (op & 1) ? sz : 1;
			if (fetch_imm(vm, sz, &imm))
				return I486_FAULT;
			if (rm_write(vm, &rm, sz, imm))
				return I486_FAULT;
			break;

		case 0xc9:				/* leave */
			s->regs[R_ESP] = s->regs[R_EBP];
			s->regs[R_EBP] = pop32(vm);
			break;

		case 0xcc:				/* int3 */
			return I486_FAULT;
		case 0xcd:				/* int imm8 */
			if (fetch8(vm, &v))
				return I486_FAULT;
			if (v != 0x80)
				return I486_FAULT;
			vm->syscall(vm);
			break;

		case 0xd0: case 0xd1: case 0xd2: case 0xd3:	/* shift */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			sz = (op & 1) ? sz : 1;
			imm = (op & 2) ? (s->regs[R_ECX] & 0xff) : 1;
			if (shift_exec(vm, &rm, sz, imm, rm.reg & 1,
			    rm.reg == 7))
				return I486_FAULT;
			break;

		case 0xd4: case 0xd5:			/* aam/aad: rare */
			if (fetch8(vm, &v))
				return I486_FAULT;
			return I486_FAULT;
		case 0xd7:				/* xlat */
			v = reg_r(vm, R_EAX, 1);
			if (rd_mem(vm, s->regs[R_EBX] + v + vm->seg, 1, &v))
				return I486_FAULT;
			reg_w(vm, R_EAX, 1, v);
			break;

		case 0xd8: case 0xd9: case 0xda: case 0xdb:
		case 0xdc: case 0xdd: case 0xde: case 0xdf:
			return I486_FAULT;		/* x87: not yet */

		case 0xe0: case 0xe1: case 0xe2: case 0xe3: {
			/* loopne/loope/loop/jecxz */
			int32_t rel;
			if (fetch8(vm, &v))
				return I486_FAULT;
			rel = (int32_t)(int8_t)v;
			if (op == 0xe3) {
				if (s->regs[R_ECX] == 0)
					s->eip += rel;
				break;
			}
			s->regs[R_ECX]--;
			if ((op == 0xe0 && !(s->eflags & I486_ZF) &&
			    s->regs[R_ECX]) ||
			    (op == 0xe1 && (s->eflags & I486_ZF) &&
			    s->regs[R_ECX]) ||
			    (op == 0xe2 && s->regs[R_ECX]))
				s->eip += rel;
			break;
		}

		case 0xe8:				/* call rel32 */
			if (fetch32(vm, &v))
				return I486_FAULT;
			push32(vm, s->eip);
			s->eip += (int32_t)v;
			break;
		case 0xe9:				/* jmp rel32 */
			if (fetch32(vm, &v))
				return I486_FAULT;
			s->eip += (int32_t)v;
			break;
		case 0xeb:				/* jmp rel8 */
			if (fetch8(vm, &v))
				return I486_FAULT;
			s->eip += (int32_t)(int8_t)v;
			break;

		case 0xec: case 0xed: case 0xee: case 0xef:
			/* in/out: no ports, return all-ones */
			if (fetch16(vm, &v))
				return I486_FAULT;
			if (op == 0xec || op == 0xed)
				reg_w(vm, R_EAX, op == 0xec ? 1 : sz,
				    0xffffffffu);
			break;

		case 0xf4:				/* hlt */
			vm->stop = 1;
			return I486_OK;

		case 0xf5:				/* cmc */
			s->eflags ^= I486_CF;
			break;
		case 0xf8: s->eflags &= ~I486_CF; break;	/* clc */
		case 0xf9: s->eflags |= I486_CF; break;		/* stc */
		case 0xfa: case 0xfb: break;		/* cli/sti: nop */
		case 0xfc: s->eflags &= ~I486_DF; break;	/* cld */
		case 0xfd: s->eflags |= I486_DF; break;		/* std */

		case 0xf6: case 0xf7:			/* group3 */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			sz = (op & 1) ? sz : 1;
			sub = rm.reg;
			if (sub <= 1) {			/* test needs imm */
				if (fetch_imm(vm, sz, &imm))
					return I486_FAULT;
			}
			if (grp_unary(vm, &rm, sz, sub, imm))
				return I486_FAULT;
			break;

		case 0xfe:				/* inc/dec r/m8 */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			if (rm_read(vm, &rm, 1, &v))
				return I486_FAULT;
			cf32 = s->eflags & I486_CF;
			v = (rm.reg == 0) ? v + 1 : v - 1;
			if (rm.reg == 0)
				set_add_flags(vm, v - 1, 1, v, 1);
			else
				set_sub_flags(vm, v + 1, 1, v, 1);
			s->eflags = (s->eflags & ~I486_CF) | cf32;
			if (rm_write(vm, &rm, 1, v))
				return I486_FAULT;
			break;

		case 0xff: {				/* group5 */
			if (decode_modrm(vm, &rm))
				return I486_FAULT;
			switch (rm.reg) {
			case 0: case 1:			/* inc/dec */
				if (rm_read(vm, &rm, sz, &v))
					return I486_FAULT;
				cf32 = s->eflags & I486_CF;
				v = (rm.reg == 0) ? v + 1 : v - 1;
				if (rm.reg == 0)
					set_add_flags(vm, v - 1, 1, v, sz);
				else
					set_sub_flags(vm, v + 1, 1, v, sz);
				s->eflags = (s->eflags & ~I486_CF) | cf32;
				if (rm_write(vm, &rm, sz, v))
					return I486_FAULT;
				break;
			case 2: case 3: case 4: case 6: {	/* call/jmp/push */
				if (rm_read(vm, &rm, 4, &v))
					return I486_FAULT;
				if (rm.reg == 4)
					s->eip = v;
				else if (rm.reg == 6)
					push32(vm, v);
				else {
					push32(vm, s->eip);
					s->eip = v;
				}
				break;
			}
			default:
				return I486_FAULT;
			}
			break;
		}

		case 0x0f:
			if (fetch8(vm, &op2))
				return I486_FAULT;
			switch (op2) {
			case 0x05: case 0x0b:		/* syscall/ud2 */
				return I486_FAULT;
			case 0x0d:			/* prefetch: nop */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				break;
			case 0x18: case 0x1f:		/* hint nops */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				break;
			case 0x31:			/* rdtsc */
				s->regs[R_EAX] = (uint32_t)vm->steps;
				s->regs[R_EDX] = (uint32_t)(vm->steps >> 32);
				break;
			case 0x30: case 0x32:		/* rdmsr/wrmsr */
				return I486_FAULT;
			case 0x40: case 0x41: case 0x42: case 0x43:
			case 0x44: case 0x45: case 0x46: case 0x47:
			case 0x48: case 0x49: case 0x4a: case 0x4b:
			case 0x4c: case 0x4d: case 0x4e: case 0x4f:
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (cond_taken(vm, op2 & 0xf)) {
					if (rm_read(vm, &rm, 4, &v))
						return I486_FAULT;
					reg_w(vm, rm.reg, 4, v);
				}
				break;
			case 0x80: case 0x81: case 0x82: case 0x83:
			case 0x84: case 0x85: case 0x86: case 0x87:
			case 0x88: case 0x89: case 0x8a: case 0x8b:
			case 0x8c: case 0x8d: case 0x8e: case 0x8f:
				if (fetch32(vm, &v))
					return I486_FAULT;
				if (cond_taken(vm, op2 & 0xf))
					s->eip += (int32_t)v;
				break;
			case 0x90: case 0x91: case 0x92: case 0x93:
			case 0x94: case 0x95: case 0x96: case 0x97:
			case 0x98: case 0x99: case 0x9a: case 0x9b:
			case 0x9c: case 0x9d: case 0x9e: case 0x9f:
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_write(vm, &rm, 1,
				    cond_taken(vm, op2 & 0xf) ? 1 : 0))
					return I486_FAULT;
				break;
			case 0xa0: case 0xa1: case 0xa8: case 0xa9:
				return I486_FAULT;	/* push/pop sreg */
			case 0xa2:			/* cpuid */
				s->regs[R_EAX] = 0;
				s->regs[R_EBX] = 0;
				s->regs[R_ECX] = 0;
				s->regs[R_EDX] = 0;
				break;
			case 0xa3: case 0xab: case 0xb3: case 0xbb: {
							/* bt/bts/btr/btc r/m, r */
				uint32_t bit;
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, 4, &v))
					return I486_FAULT;
				bit = reg_r(vm, rm.reg, 4) & 31;
				s->eflags = (s->eflags & ~I486_CF) |
				    ((v >> bit) & 1 ? I486_CF : 0);
				if (op2 != 0xa3) {
					uint32_t b = 1u << bit;
					if (op2 == 0xab)
						v |= b;
					else if (op2 == 0xb3)
						v &= ~b;
					else
						v ^= b;
					if (rm_write(vm, &rm, 4, v))
						return I486_FAULT;
				}
				break;
			}
			case 0xaf: {			/* imul r, rm */
				int32_t a, b, c;
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, 4, &v))
					return I486_FAULT;
				a = (int32_t)s->regs[rm.reg];
				b = (int32_t)v;
				c = (int32_t)((uint32_t)a * (uint32_t)b);
				s->regs[rm.reg] = c;
				vm->s.eflags &= ~(I486_CF | I486_OF);
				if ((int64_t)a * b != c)
					vm->s.eflags |= I486_CF | I486_OF;
				break;
			}
			case 0xb0: case 0xb1: {		/* cmpxchg */
				uint32_t acc;
				if (op2 == 0xb0)
					sz = 1;
				acc = reg_r(vm, R_EAX, sz);
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, sz, &v))
					return I486_FAULT;
				if (acc == v) {
					if (rm_write(vm, &rm, sz,
					    reg_r(vm, rm.reg, sz)))
						return I486_FAULT;
					s->eflags |= I486_ZF;
				} else {
					reg_w(vm, R_EAX, sz, v);
					s->eflags &= ~I486_ZF;
				}
				break;
			}
			case 0xb6: case 0xb7:		/* movzx */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, op2 == 0xb6 ? 1 : 2, &v))
					return I486_FAULT;
				reg_w(vm, rm.reg, 4, v);
				break;
			case 0xbc: case 0xbd: {		/* bsf/bsr */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, 4, &v))
					return I486_FAULT;
				if (v == 0) {
					s->eflags |= I486_ZF;
					break;
				}
				s->eflags &= ~I486_ZF;
				for (imm = 0; ; imm++) {
					if (op2 == 0xbc ? (v & (1u << imm)) :
					    (v & (1u << (31 - imm)))) {
						reg_w(vm, rm.reg, 4,
						    op2 == 0xbc ? imm :
						    31 - imm);
						break;
					}
				}
				break;
			}
			case 0xbe: case 0xbf:		/* movsx */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				{
					int osz = op2 == 0xbe ? 1 : 2;
					if (rm_read(vm, &rm, osz, &v))
						return I486_FAULT;
					reg_w(vm, rm.reg, 4, sext(v, osz));
				}
				break;
			case 0xba: {			/* bt group /4-/7 */
				uint32_t bit;
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm.reg < 4)
					return I486_FAULT;
				if (fetch8(vm, &bit))
					return I486_FAULT;
				if (rm_read(vm, &rm, 4, &v))
					return I486_FAULT;
				s->eflags = (s->eflags & ~I486_CF) |
				    ((v >> (bit & 31)) & 1 ? I486_CF : 0);
				if (rm.reg != 4) {
					uint32_t b = 1u << (bit & 31);
					if (rm.reg == 5)
						v |= b;
					else if (rm.reg == 6)
						v &= ~b;
					else
						v ^= b;
					if (rm_write(vm, &rm, 4, v))
						return I486_FAULT;
				}
				break;
			}
			case 0xa4: case 0xa5: case 0xac: case 0xad: {
							/* shld/shrd */
				uint32_t d, src, cnt, res, w = sz * 8;
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (op2 == 0xa4 || op2 == 0xac) {
					if (fetch_imm(vm, 1, &cnt))
						return I486_FAULT;
				} else
					cnt = reg_r(vm, R_ECX, 1);
				if (rm_read(vm, &rm, sz, &d))
					return I486_FAULT;
				src = reg_r(vm, rm.reg, sz);
				cnt &= w - 1;
				if (cnt == 0)
					break;
				if (op2 == 0xa4 || op2 == 0xa5)
					res = (d << cnt) |
					    (src >> (w - cnt));
				else
					res = (d >> cnt) |
					    (src << (w - cnt));
				res &= sz_mask(sz);
				if (rm_write(vm, &rm, sz, res))
					return I486_FAULT;
				set_logic_flags(vm, res, sz);
				if (op2 == 0xa4 || op2 == 0xa5) {
					if ((d >> (w - cnt)) & 1)
						vm->s.eflags |= I486_CF;
					if (cnt == 1 &&
					    ((res ^ d) & sz_sign(sz)))
						vm->s.eflags |= I486_OF;
				} else {
					if ((d >> (cnt - 1)) & 1)
						vm->s.eflags |= I486_CF;
				}
				break;
			}
			case 0xb9:			/* ud1 */
				return I486_FAULT;
			case 0xc0: {			/* xadd */
				uint32_t r;
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, sz, &v))
					return I486_FAULT;
				r = reg_r(vm, rm.reg, sz);
				if (rm_write(vm, &rm, sz, (v + r) &
				    sz_mask(sz)))
					return I486_FAULT;
				set_add_flags(vm, v, r, v + r, sz);
				reg_w(vm, rm.reg, sz, v);
				break;
			}
			case 0xc1: {			/* xadd r/m8, r8 */
				uint32_t r;
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm_read(vm, &rm, 1, &v))
					return I486_FAULT;
				r = reg_r(vm, rm.reg, 1);
				if (rm_write(vm, &rm, 1, (v + r) & 0xff))
					return I486_FAULT;
				set_add_flags(vm, v, r, v + r, 1);
				reg_w(vm, rm.reg, 1, v);
				break;
			}
			case 0xc7:			/* 0f c7: /1 = cmpxchg8b */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				if (rm.reg != 1 || rm.isreg)
					return I486_FAULT;
				{
					uint64_t cur, acc, nw;
					uint8_t *p;
					p = i486_h(&vm->m, rm.va);
					if (p == NULL || (uint32_t)(rm.va -
					    vm->m.vbase) + 8 > vm->m.size)
						return I486_FAULT;
					cur = 0;
					for (imm = 0; imm < 8; imm++)
						cur |= (uint64_t)p[imm] <<
						    (8 * imm);
					acc = ((uint64_t)s->regs[R_EDX] <<
					    32) | s->regs[R_EAX];
					if (cur == acc) {
						nw = ((uint64_t)
						    s->regs[R_ECX] << 32) |
						    s->regs[R_EBX];
						for (imm = 0; imm < 8; imm++)
							p[imm] = nw >>
							    (8 * imm);
						s->eflags |= I486_ZF;
					} else {
						s->regs[R_EAX] = (uint32_t)cur;
						s->regs[R_EDX] = (uint32_t)
						    (cur >> 32);
						s->eflags &= ~I486_ZF;
					}
				}
				break;
			case 0xc8: case 0xc9: case 0xca: case 0xcb:
			case 0xcc: case 0xcd: case 0xce: case 0xcf: {
				/* bswap */
				int r = op2 & 7;
				v = s->regs[r];
				s->regs[r] = ((v & 0xff) << 24) |
				    ((v & 0xff00) << 8) |
				    ((v >> 8) & 0xff00) | ((v >> 24) & 0xff);
				break;
			}
			case 0xae: {			/* fence/xsave: nop */
				if (decode_modrm(vm, &rm))
					return I486_FAULT;
				break;
			}
			default:
				vm->fault_va = 0xf0000000u | op2;
				return I486_FAULT;
			}
			break;

		default:
			vm->fault_va = 0xf0000000u | op;
			return I486_FAULT;
		}
	}
}
