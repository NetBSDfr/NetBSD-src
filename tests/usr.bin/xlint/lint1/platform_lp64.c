/*	$NetBSD: platform_lp64.c,v 1.9 2024/03/09 17:34:01 rillig Exp $	*/
# 3 "platform_lp64.c"

/*
 * Test features that only apply to platforms that have 32-bit int and 64-bit
 * long and pointer types.
 */

/* lint1-only-if: lp64 */
/* lint1-extra-flags: -c -h -a -p -b -r -z -X 351 */

int s32;
unsigned int u32;
long sl32;
unsigned long ul32;
__int128_t s128;
__uint128_t u128;

void
convert_between_int_and_long(void)
{
	/* expect+1: warning: conversion from 'long' to 'int' may lose accuracy [132] */
	s32 = sl32;
	sl32 = s32;
	/* expect+1: warning: conversion from 'unsigned long' to 'unsigned int' may lose accuracy [132] */
	u32 = ul32;
	ul32 = u32;
}

void to_size_t(typeof(sizeof(int)));

void
convert_unsigned_char_to_size_t(unsigned char uc)
{
	/* no warning, unlike platform_int */
	to_size_t(uc);
}

void
convert_128(void)
{
	/* expect+1: warning: conversion from '__int128_t' to 'int' may lose accuracy [132] */
	s32 = s128;
	/* expect+1: warning: conversion from '__uint128_t' to 'unsigned int' may lose accuracy [132] */
	u32 = u128;
}

unsigned char u8;
unsigned long long u64;
unsigned char u8_buf[20];
unsigned long long u64_buf[20];

void
array_index(void)
{

	/* expect+1: warning: array subscript cannot be > 19: 16777215 [168] */
	u8 += u8_buf[0x00ffffff];
	/* expect+1: warning: array subscript cannot be > 19: 2147483647 [168] */
	u8 += u8_buf[0x7fffffff];
	/* expect+1: warning: array subscript cannot be > 19: 2147483648 [168] */
	u8 += u8_buf[2147483648];
	/* expect+1: warning: array subscript cannot be > 19: 2147483648 [168] */
	u8 += u8_buf[0x80000000];
	/* expect+1: warning: array subscript cannot be > 19: 4294967295 [168] */
	u8 += u8_buf[0xffffffff];
	/* expect+1: warning: array subscript cannot be > 19: 2147483648 [168] */
	u8 += u8_buf[0x80000000];
	/* expect+1: warning: array subscript cannot be > 19: 4294967295 [168] */
	u8 += u8_buf[0xffffffff];
	/* expect+1: warning: array subscript cannot be > 19: 72057594037927935 [168] */
	u8 += u8_buf[0x00ffffffffffffff];
	/* expect+1: warning: array subscript cannot be negative: -1 [167] */
	u8 += u8_buf[0xffffffffffffffff];

	/* expect+1: warning: array subscript cannot be > 19: 16777215 [168] */
	u64 += u64_buf[0x00ffffff];
	/* expect+1: warning: array subscript cannot be > 19: 2147483647 [168] */
	u64 += u64_buf[0x7fffffff];
	/* expect+1: warning: array subscript cannot be > 19: 2147483648 [168] */
	u64 += u64_buf[2147483648];
	/* expect+1: warning: array subscript cannot be > 19: 2147483648 [168] */
	u64 += u64_buf[0x80000000];
	/* expect+1: warning: array subscript cannot be > 19: 4294967295 [168] */
	u64 += u64_buf[0xffffffff];
	/* expect+1: warning: array subscript cannot be > 19: 2147483648 [168] */
	u64 += u64_buf[0x80000000];
	/* expect+1: warning: array subscript cannot be > 19: 4294967295 [168] */
	u64 += u64_buf[0xffffffff];
	/* expect+1: warning: array subscript cannot be > 19: 72057594037927935 [168] */
	u64 += u64_buf[0x00ffffffffffffff];
	/* expect+1: warning: array subscript cannot be > 19: 1152921504606846975 [168] */
	u64 += u64_buf[0x0fffffffffffffff];
	// FIXME: integer overflow
	//u64 += u64_buf[0x1fffffffffffffff];
	// FIXME: integer overflow
	//u64 += u64_buf[0x3fffffffffffffff];
	// FIXME: integer overflow
	//u64 += u64_buf[0x7fffffffffffffff];
	/* expect+1: warning: array subscript cannot be negative: -1 [167] */
	u64 += u64_buf[0xffffffffffffffff];
}
