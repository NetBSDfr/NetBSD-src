/*	$NetBSD$	*/

/*
 * E5b: x86 block -> wasm translator.  See jit.h.
 *
 * Single pass: decode guest instructions, emit wasm.  Branches
 * terminate the block, so every branch target is a compile-time
 * constant and no label fixups are needed.  Guest registers are
 * shadowed in wasm locals and committed to the vm struct only at the
 * end; a mid-block fault branches to the fault tail and commits
 * nothing, so the interpreter can replay the block safely.
 *
 * wasm opcodes used: 0x20 local.get 0x21 local.set 0x28 i32.load
 * 0x36 i32.store(offset imm) 0x41 i32.const 0x45 i32.eqz
 * 0x47 i32.ne 0x49 i32.lt_u 0x6a i32.add 0x6b i32.sub 0x71 i32.and
 * 0x72 i32.or 0x73 i32.xor 0x76 i32.shr_u 0x74 i32.shl 0x6c i32.mul 0x1b select
 * 0x02 block 0x0b end 0x0c br 0x0d br_if 0x0f return.
 */

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/param.h>
#ifdef JITDBG
#include <sys/systm.h>
#define	DBG(...) printf(__VA_ARGS__)
#else
#define	DBG(...) do {} while (0)
#endif
#else
#include <string.h>
#ifdef JITDBG
#include <stdio.h>
#define	DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define	DBG(...) do {} while (0)
#endif
#endif

#include "jit.h"

/* ---- emit helpers ---- */

static void
e8(struct jit_out *o, uint8_t b)
{
	if (o->len + 1 > o->cap) { o->error = 1; return; }
	o->buf[o->len++] = b;
}

static void
euleb(struct jit_out *o, uint32_t v)
{
	do {
		uint8_t x = v & 0x7f;
		v >>= 7;
		e8(o, v ? x | 0x80 : x);
	} while (v && !o->error);
}

static void
esleb(struct jit_out *o, int32_t v)
{
	int more;
	uint8_t x;
	do {
		x = v & 0x7f;
		v >>= 7;
		more = !((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40)));
		e8(o, more ? x | 0x80 : x);
	} while (more && !o->error);
}

static void	e1(struct jit_out *o, uint8_t op) { e8(o, op); }
static void	e2(struct jit_out *o, uint8_t a, uint8_t b) { e8(o, a); e8(o, b); }

#define	L_VM	0
#define	L_R0	1		/* locals 1..8: eax..edi shadows */
#define	L_EF	9
#define	L_A	10
#define	L_B	11
#define	L_R	12
#define	L_T	13
#define	L_NXT	14
#define	L_CF	15
#define	NLOCALS	16

#define	FL_W	0x08c1u		/* CF|SF|ZF|OF */

static void
lget(struct jit_out *o, int l) { e2(o, 0x20, l); }
static void
lset(struct jit_out *o, int l) { e2(o, 0x21, l); }
static void
icon(struct jit_out *o, int32_t v) { e8(o, 0x41); esleb(o, v); }
static void
vload(struct jit_out *o, uint32_t off)
{
	lget(o, L_VM); e8(o, 0x28); e8(o, 2); euleb(o, off);
}

/* ---- guest fetch (decode side) ---- */

struct cur {
	struct i486_vm *vm;
	uint32_t eip;
};

static int
fetch8g(struct cur *c, uint32_t *out)
{
	uint8_t *p = i486_h(&c->vm->m, c->eip);
	if (p == NULL)
		return 1;
	*out = *p;
	c->eip++;
	return 0;
}

static int
fetch32g(struct cur *c, uint32_t *out)
{
	uint32_t b0, b1, b2, b3;
	if (fetch8g(c, &b0) || fetch8g(c, &b1) || fetch8g(c, &b2) ||
	    fetch8g(c, &b3))
		return 1;
	*out = b0 | b1 << 8 | b2 << 16 | b3 << 24;
	return 0;
}

/* ---- flag emission (32-bit; mirrors set_add/sub/logic_flags) ---- */

/* Flag bits are disjoint and pre-cleared, so i32.add composes them. */
static void
emit_add_flags(struct jit_out *o)
{
	lget(o, L_EF); icon(o, ~FL_W); e1(o, 0x71);
	lget(o, L_R); e1(o, 0x45); icon(o, 6); e1(o, 0x74); e1(o, 0x6a);
	lget(o, L_R); icon(o, 31); e1(o, 0x76); icon(o, 7); e1(o, 0x74);
	e1(o, 0x6a);
	lget(o, L_R); lget(o, L_A); e1(o, 0x49); e1(o, 0x6a);
	lget(o, L_A); lget(o, L_R); e1(o, 0x73);
	lget(o, L_B); lget(o, L_R); e1(o, 0x73); e1(o, 0x71);
	icon(o, 31); e1(o, 0x76); icon(o, 11); e1(o, 0x74); e1(o, 0x6a);
	lset(o, L_EF);
}

static void
emit_sub_flags(struct jit_out *o)
{
	lget(o, L_EF); icon(o, ~FL_W); e1(o, 0x71);
	lget(o, L_R); e1(o, 0x45); icon(o, 6); e1(o, 0x74); e1(o, 0x6a);
	lget(o, L_R); icon(o, 31); e1(o, 0x76); icon(o, 7); e1(o, 0x74);
	e1(o, 0x6a);
	lget(o, L_A); lget(o, L_B); e1(o, 0x49); e1(o, 0x6a);
	lget(o, L_A); lget(o, L_B); e1(o, 0x73);
	lget(o, L_A); lget(o, L_R); e1(o, 0x73); e1(o, 0x71);
	icon(o, 31); e1(o, 0x76); icon(o, 11); e1(o, 0x74); e1(o, 0x6a);
	lset(o, L_EF);
}

static void
emit_logic_flags(struct jit_out *o)
{
	lget(o, L_EF); icon(o, ~FL_W); e1(o, 0x71);
	lget(o, L_R); e1(o, 0x45); icon(o, 6); e1(o, 0x74); e1(o, 0x6a);
	lget(o, L_R); icon(o, 31); e1(o, 0x76); icon(o, 7); e1(o, 0x74);
	e1(o, 0x6a);
	lset(o, L_EF);
}

static void
emit_alu_flags(struct jit_out *o, int alu)
{
	switch (alu) {
	case 0: case 2: emit_add_flags(o); break;
	case 5: case 7: emit_sub_flags(o); break;
	default: emit_logic_flags(o); break;
	}
}

/* condition cc -> i32 0/1 on stack (mirrors cond_taken) */
static void
emit_cc(struct jit_out *o, int cc)
{
#define	FLAG(sh) lget(o, L_EF); icon(o, sh); e1(o, 0x76); \
    icon(o, 1); e1(o, 0x71)
	switch (cc) {
	case 0x0: FLAG(11); break;
	case 0x1: FLAG(11); e1(o, 0x45); break;
	case 0x2: FLAG(0); break;
	case 0x3: FLAG(0); e1(o, 0x45); break;
	case 0x4: FLAG(6); break;
	case 0x5: FLAG(6); e1(o, 0x45); break;
	case 0x6: FLAG(0); FLAG(6); e1(o, 0x72); break;
	case 0x7: FLAG(0); e1(o, 0x45); FLAG(6); e1(o, 0x45); e1(o, 0x71);
	    break;
	case 0x8: FLAG(7); break;
	case 0x9: FLAG(7); e1(o, 0x45); break;
	case 0xA: icon(o, 0); break;
	case 0xB: icon(o, 1); break;
	case 0xC: FLAG(7); FLAG(11); e1(o, 0x47); break;
	case 0xD: FLAG(7); FLAG(11); e1(o, 0x47); e1(o, 0x45); break;
	case 0xE: FLAG(6); FLAG(7); FLAG(11); e1(o, 0x47); e1(o, 0x72);
	    break;
	default: FLAG(6); e1(o, 0x45); FLAG(7); FLAG(11); e1(o, 0x47);
	    e1(o, 0x45); e1(o, 0x71); break;
	}
#undef FLAG
}

/*
 * gva on stack -> host linear address on stack; br 0 ($fault) when out
 * of bounds.  delta = gva - vbase; OOB when delta >=u size.
 */
static void
emit_check(struct jit_out *o)
{
	lget(o, L_T); vload(o, JIT_O_VBASE); e1(o, 0x6b);	/* delta */
	lget(o, L_VM); vload(o, JIT_O_SIZE);
	/* need delta for both compare and addr: stash delta in L_T first */
	/* (caller left gva in L_T; recompute delta after compare) */
	e1(o, 0x49);			/* delta <u size ? */
	e1(o, 0x45);			/* eqz -> OOB */
	e2(o, 0x0d, 0x00);		/* br_if 0 */
	lget(o, L_VM); vload(o, JIT_O_HBASE);
	lget(o, L_T); vload(o, JIT_O_VBASE); e1(o, 0x6b);
	e1(o, 0x6a);			/* hbase + delta */
}

/* gva must be in L_T; value on stack -> store, consuming both */
static void
emit_store_at_T(struct jit_out *o)
{
	/* stack: [value]; emit host addr under it: reorder via local */
	lset(o, L_R);			/* value -> L_R */
	o->stores = 1;
	emit_check(o);			/* host addr */
	lget(o, L_R);
	e8(o, 0x36); e8(o, 2); euleb(o, 0);	/* i32.store */
}

/* gva in L_T -> i32 value on stack */
static void
emit_load_at_T(struct jit_out *o)
{
	emit_check(o);
	e8(o, 0x28); e8(o, 2); euleb(o, 0);	/* i32.load */
}

/* commit: L_NXT holds next eip; return 0 */
static void
emit_commit(struct jit_out *o)
{
	int i;
	for (i = 0; i < 8; i++) {
		lget(o, L_VM); lget(o, L_R0 + i);
		e8(o, 0x36); e8(o, 2); euleb(o, 4 * i);
	}
	lget(o, L_VM); lget(o, L_EF);
	e8(o, 0x36); e8(o, 2); euleb(o, JIT_O_EFLAGS);
	lget(o, L_VM); lget(o, L_NXT);
	e8(o, 0x36); e8(o, 2); euleb(o, JIT_O_EIP);
	icon(o, 0);
	e1(o, 0x0f);			/* return 0 */
}

/*
 * modrm address computation (mod != 3): emits gva into L_T.
 * Returns 0 ok, 1 unsupported (needs a byte we won't decode).
 */
static int
emit_addr(struct jit_out *o, struct cur *c, int mod, int rm)
{
	int base = -1, idx = -1, scale = 0, have_disp = 0;
	int32_t disp = 0;
	uint32_t b;

	if (rm == 4) {			/* SIB */
		if (fetch8g(c, &b))
			return 1;
		scale = (b >> 6) & 3;
		idx = (b >> 3) & 7;
		base = b & 7;
		if (base == 5 && mod == 0)
			base = -1;
	} else if (mod == 0 && rm == 5) {
		base = -1;
	} else {
		base = rm;
	}
	if (mod == 1) {
		if (fetch8g(c, &b))
			return 1;
		disp = (int32_t)(int8_t)b;
		have_disp = 1;
	} else if (mod == 2 || (mod == 0 && rm == 5) ||
	    (rm == 4 && base == -1 && mod == 0)) {
		uint32_t d;
		if (fetch32g(c, &d))
			return 1;
		disp = (int32_t)d;
		have_disp = 1;
	}
	/* sum: disp + base + idx<<scale */
	{
		int terms = 0;
		if (have_disp) {
			icon(o, disp);
			terms++;
		}
		if (base >= 0) {
			lget(o, L_R0 + base);
			terms++;
		}
		if (rm == 4 && idx != 4) {
			/* index only present with SIB (rm=4), and not
			 * when idx=4 (ESP: no index).  scale field s
			 * means multiplier 2^s. */
			lget(o, L_R0 + idx);
			if (scale) { icon(o, 1 << scale); e1(o, 0x6c); }
			terms++;
		}
		if (terms == 0)
			icon(o, 0);
		while (--terms > 0)
			e1(o, 0x6a);
	}
	lset(o, L_T);
	return 0;
}

/* alu with $a/$b already set; writes $r, flags, optional writeback */
static void
emit_alu_core(struct jit_out *o, int alu, int wb, int wbreg)
{
	lget(o, L_A); lget(o, L_B);	/* operands from locals */
	switch (alu) {
	case 0: e1(o, 0x6a); break;			/* add */
	case 1: e1(o, 0x72); break;			/* or */
	case 2: lget(o, L_CF); e1(o, 0x6a); e1(o, 0x6a); break; /* adc */
	case 3: e1(o, 0x6b); lget(o, L_CF); e1(o, 0x6b); break; /* sbb */
	case 4: e1(o, 0x71); break;			/* and */
	case 5: e1(o, 0x6b); break;			/* sub */
	case 6: e1(o, 0x73); break;			/* xor */
	default: e1(o, 0x6b); break;			/* cmp */
	}
	lset(o, L_R);
	if (wb) {
		lget(o, L_R);
		lset(o, L_R0 + wbreg);
	}
	emit_alu_flags(o, alu);
	/* refresh cf shadow after flag update (adc/sbb of next insn) */
	lget(o, L_EF); icon(o, 1); e1(o, 0x71); lset(o, L_CF);
}

/* ---- main translate ---- */

int
jit_translate(struct i486_vm *vm, struct jit_out *out, int *ninsns)
{
	struct cur c = { vm, vm->s.eip };
	uint8_t body[16384];
	struct jit_out b = { body, sizeof(body), 0, 0 };
	uint32_t stop_at = vm->s.eip;
	int n = 0, i;

	*ninsns = 0;

	/* prologue: shadow regs, eflags, cf */
	for (i = 0; i < 8; i++) {
		vload(&b, 4 * i);
		lset(&b, L_R0 + i);
	}
	vload(&b, JIT_O_EFLAGS);
	lset(&b, L_EF);
	lget(&b, L_EF); icon(&b, 1); e1(&b, 0x71); lset(&b, L_CF);

	/* block $fault */
	e1(&b, 0x02); e1(&b, 0x40);

	while (n < JIT_MAX_BLOCK) {
		uint32_t start = c.eip, op, mrm, imm;
		int mod, reg, rm, alu;

		stop_at = start;
		if (fetch8g(&c, &op)) { DBG("stop: fetch fail @%x\n", start); break; }
		DBG("dec %x op=%02x\n", start, op);

		switch (op) {
		case 0x90:		/* nop */
			n++;
			continue;

		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57:
			/* push r32 */
			lget(&b, L_R0 + 4); icon(&b, 4); e1(&b, 0x6b);
			lset(&b, L_R0 + 4);
			lget(&b, L_R0 + 4); lset(&b, L_T);
			lget(&b, L_R0 + (op & 7));
			emit_store_at_T(&b);
			n++;
			continue;

		case 0x58: case 0x59: case 0x5a: case 0x5b:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f:
			/* pop r32 */
			lget(&b, L_R0 + 4); lset(&b, L_T);
			emit_load_at_T(&b);
			lset(&b, L_R0 + (op & 7));
			lget(&b, L_R0 + 4); icon(&b, 4); e1(&b, 0x6a);
			lset(&b, L_R0 + 4);
			n++;
			continue;

		case 0x68:		/* push imm32 */
			if (fetch32g(&c, &imm))
				goto stop;
			lget(&b, L_R0 + 4); icon(&b, 4); e1(&b, 0x6b);
			lset(&b, L_R0 + 4);
			lget(&b, L_R0 + 4); lset(&b, L_T);
			icon(&b, (int32_t)imm);
			emit_store_at_T(&b);
			n++;
			continue;

		case 0xb0: case 0xb1: case 0xb2: case 0xb3:
		case 0xb4: case 0xb5: case 0xb6: case 0xb7:
			goto stop;	/* 8-bit mov: v0 unsupported */

		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			if (fetch32g(&c, &imm))
				goto stop;
			icon(&b, (int32_t)imm);
			lset(&b, L_R0 + (op & 7));
			n++;
			continue;

		case 0x84: case 0x86:
			goto stop;	/* 8-bit test/xchg */

		case 0x85: {		/* test rm32, r32 */
			if (fetch8g(&c, &mrm))
				goto stop;
			mod = mrm >> 6; reg = (mrm >> 3) & 7; rm = mrm & 7;
			if (mod != 3)
				goto stop;
			lget(&b, L_R0 + rm); lset(&b, L_A);
			lget(&b, L_R0 + reg); lset(&b, L_B);
			emit_alu_core(&b, 4, 0, 0);	/* and, no wb */
			n++;
			continue;
		}

		case 0x89: case 0x8b: {	/* mov rm32,r32 / mov r32,rm32 */
			int dstreg;
			if (fetch8g(&c, &mrm))
				goto stop;
			mod = mrm >> 6; reg = (mrm >> 3) & 7; rm = mrm & 7;
			dstreg = (op == 0x89) ? rm : reg;
			if (mod == 3) {
				int src = (op == 0x89) ? reg : rm;
				lget(&b, L_R0 + src);
				lset(&b, L_R0 + dstreg);
				n++;
				continue;
			}
			if (op == 0x89) {	/* store reg to mem */
				if (emit_addr(&b, &c, mod, rm))
					goto stop;
				/* addr in L_T; value reg -> store */
				lget(&b, L_R0 + reg);
				emit_store_at_T(&b);
			} else {		/* load mem into reg */
				if (emit_addr(&b, &c, mod, rm))
					goto stop;
				emit_load_at_T(&b);
				lset(&b, L_R0 + reg);
			}
			n++;
			continue;
		}

		case 0x8d: {		/* lea r32, m */
			if (fetch8g(&c, &mrm))
				goto stop;
			mod = mrm >> 6; reg = (mrm >> 3) & 7; rm = mrm & 7;
			if (mod == 3)
				goto stop;	/* lea reg,reg: rare */
			if (emit_addr(&b, &c, mod, rm))
				goto stop;
			lget(&b, L_T);
			lset(&b, L_R0 + reg);
			n++;
			continue;
		}

		case 0x81: case 0x83: {	/* group1 rm32, imm */
			if (fetch8g(&c, &mrm))
				goto stop;
			mod = mrm >> 6; reg = (mrm >> 3) & 7; rm = mrm & 7;
			alu = reg;
			if (alu == 1 || alu == 3)
				goto stop;	/* keep v0 narrow */
			if (op == 0x83) {
				if (fetch8g(&c, &imm))
					goto stop;
				imm = (uint32_t)(int32_t)(int8_t)imm;
			} else if (fetch32g(&c, &imm))
				goto stop;
			if (mod == 3) {
				lget(&b, L_R0 + rm); lset(&b, L_A);
			} else {
				if (emit_addr(&b, &c, mod, rm))
					goto stop;
				emit_load_at_T(&b);
				lset(&b, L_A);
			}
			icon(&b, (int32_t)imm);
			lset(&b, L_B);
			emit_alu_core(&b, alu, alu != 7 && mod == 3, rm);
			if (mod != 3 && alu != 7) {
				/* write result back to memory */
				lget(&b, L_T); lget(&b, L_R);
				emit_store_at_T(&b);
			}
			n++;
			continue;
		}

		case 0x01: case 0x09: case 0x21: case 0x29:
		case 0x31: case 0x39:
		case 0x03: case 0x0b: case 0x23: case 0x2b:
		case 0x33: case 0x3b: {
			/* alu rm32,r32 (op&2==0) / r32,rm32 */
			int rm_is_dst = (op & 2) == 0;
			alu = (op >> 3) & 7;
			if (fetch8g(&c, &mrm))
				goto stop;
			mod = mrm >> 6; reg = (mrm >> 3) & 7; rm = mrm & 7;
			if (rm_is_dst) {
				if (mod == 3) {
					lget(&b, L_R0 + rm); lset(&b, L_A);
				} else {
					if (emit_addr(&b, &c, mod, rm))
						goto stop;
					emit_load_at_T(&b);
					lset(&b, L_A);
				}
				lget(&b, L_R0 + reg); lset(&b, L_B);
			} else {
				lget(&b, L_R0 + reg); lset(&b, L_A);
				if (mod == 3) {
					lget(&b, L_R0 + rm); lset(&b, L_B);
				} else {
					if (emit_addr(&b, &c, mod, rm))
						goto stop;
					emit_load_at_T(&b);
					lset(&b, L_B);
				}
			}
			emit_alu_core(&b, alu, alu != 7 && !(rm_is_dst && mod != 3),
			    rm_is_dst ? rm : reg);
			if (rm_is_dst && mod != 3 && alu != 7) {
				lget(&b, L_T); lget(&b, L_R);
				emit_store_at_T(&b);
			}
			n++;
			stop_at = c.eip;
			continue;
		}

		case 0x40: case 0x41: case 0x42: case 0x43:
		case 0x44: case 0x45: case 0x46: case 0x47:
		case 0x48: case 0x49: case 0x4a: case 0x4b:
		case 0x4c: case 0x4d: case 0x4e: case 0x4f:
			/* inc/dec r32 (CF preserved) */
			lget(&b, L_R0 + (op & 7)); lset(&b, L_A);
			icon(&b, 1); lset(&b, L_B);
			lget(&b, L_A); lget(&b, L_B);
			e1(&b, op < 0x48 ? 0x6a : 0x6b);
			lset(&b, L_R);
			lget(&b, L_R); lset(&b, L_R0 + (op & 7));
			if (op < 0x48)
				emit_add_flags(&b);
			else
				emit_sub_flags(&b);
			/* restore CF */
			lget(&b, L_EF); icon(&b, ~1); e1(&b, 0x71);
			lget(&b, L_CF); e1(&b, 0x72); lset(&b, L_EF);
			n++;
			continue;

		case 0xc3:		/* ret */
			lget(&b, L_R0 + 4); lset(&b, L_T);
			emit_load_at_T(&b);
			lset(&b, L_NXT);
			lget(&b, L_R0 + 4); icon(&b, 4); e1(&b, 0x6a);
			lset(&b, L_R0 + 4);
			emit_commit(&b);
			n++;
			*ninsns = n;
			goto emit_end;

		case 0xe8: {		/* call rel32 */
			int32_t rel;
			if (fetch32g(&c, &imm))
				goto stop;
			rel = (int32_t)imm;
			/* push return eip (c.eip = after insn) */
			lget(&b, L_R0 + 4); icon(&b, 4); e1(&b, 0x6b);
			lset(&b, L_R0 + 4);
			lget(&b, L_R0 + 4); lset(&b, L_T);
			icon(&b, (int32_t)c.eip);
			emit_store_at_T(&b);
			icon(&b, (int32_t)(c.eip + rel));
			lset(&b, L_NXT);
			emit_commit(&b);
			n++;
			*ninsns = n;
			goto emit_end;
		}

		case 0xe9: case 0xeb: {	/* jmp rel32 / rel8 */
			int32_t rel;
			if (op == 0xe9) {
				if (fetch32g(&c, &imm))
					goto stop;
				rel = (int32_t)imm;
			} else {
				if (fetch8g(&c, &imm))
					goto stop;
				rel = (int32_t)(int8_t)imm;
			}
			icon(&b, (int32_t)(c.eip + rel));
			lset(&b, L_NXT);
			emit_commit(&b);
			n++;
			*ninsns = n;
			goto emit_end;
		}

		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b:
		case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
			int32_t rel;
			if (fetch8g(&c, &imm))
				goto stop;
			rel = (int32_t)(int8_t)imm;
			icon(&b, (int32_t)(c.eip + rel));	/* taken */
			icon(&b, (int32_t)c.eip);		/* fall */
			emit_cc(&b, op & 0xf);
			e8(&b, 0x1b);		/* select */
			lset(&b, L_NXT);
			emit_commit(&b);
			n++;
			*ninsns = n;
			goto emit_end;
		}

		default:
			DBG("stop: default op=%02x\n", op);
			goto stop;
		}
	}

stop:
	/* Block ends before the instruction we could not translate:
	 * commit with eip = stop_at (start of that instruction), or the
	 * post-loop eip when the instruction budget was exhausted. */
	if (n == JIT_MAX_BLOCK)
		stop_at = c.eip;
	icon(&b, (int32_t)stop_at);
	lset(&b, L_NXT);
	emit_commit(&b);

emit_end:
	e1(&b, 0x0b);			/* end $fault */
	icon(&b, -1);			/* fault tail: return -1 */
	e1(&b, 0x0b);			/* end func */

	if (b.error) {
		out->error = 1;
		return -1;
	}
	if (n == 0) {
		*ninsns = 0;
		return 0;		/* nothing translatable */
	}
	*ninsns = n;

	/* ---- assemble module ---- */
	{
		uint8_t sec[256];
		size_t bl = b.len;
		int i2;

		out->len = 0;
		e8(out, 0x00); e8(out, 'a'); e8(out, 's'); e8(out, 'm');
		e8(out, 0x01); e8(out, 0x00); e8(out, 0x00); e8(out, 0x00);
		/* type: (i32)->(i32) */
		{
			uint8_t t[] = { 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f };
			e8(out, 1); euleb(out, sizeof(t));
			for (i2 = 0; i2 < (int)sizeof(t); i2++)
				e8(out, t[i2]);
		}
		/* import env.memory min-only */
		{
			uint8_t im[] = { 0x01, 0x03, 'e','n','v', 0x06,
			    'm','e','m','o','r','y', 0x02, 0x00, 0x00 };
			e8(out, 2); euleb(out, sizeof(im));
			for (i2 = 0; i2 < (int)sizeof(im); i2++)
				e8(out, im[i2]);
		}
		/* func: type 0 */
		e8(out, 3); euleb(out, 2); e8(out, 0x01); e8(out, 0x00);
		/* export "b" */
		{
			uint8_t ex[] = { 0x01, 0x01, 'b', 0x00, 0x00 };
			e8(out, 7); euleb(out, sizeof(ex));
			for (i2 = 0; i2 < (int)sizeof(ex); i2++)
				e8(out, ex[i2]);
		}
		/* code */
		{
			struct jit_out ph = { sec, sizeof(sec), 0, 0 };
			size_t bodylen = 3 + bl;	/* 1+2 bytes locals decl */
			e8(out, 10);
			euleb(&ph, bodylen);
			euleb(out, ph.len + 1 + bodylen);	/* section size */
			euleb(out, 1);				/* 1 function */
			for (i2 = 0; i2 < (int)ph.len; i2++)
				e8(out, sec[i2]);
			euleb(out, 1);		/* 1 local group */
			euleb(out, NLOCALS - 1); e8(out, 0x7f);
			for (i2 = 0; i2 < (int)bl; i2++)
				e8(out, b.buf[i2]);
		}
	}
	return out->error ? -1 : 1;
}
