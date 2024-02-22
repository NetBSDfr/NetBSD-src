/*	$NetBSD: snprintb.c,v 1.37 2024/02/20 20:31:56 rillig Exp $	*/

/*-
 * Copyright (c) 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * snprintb: print an interpreted bitmask to a buffer
 *
 * => returns the length of the buffer that would be required to print the
 *    string minus the terminating NUL.
 */
#ifndef _STANDALONE
# ifndef _KERNEL

#  if HAVE_NBTOOL_CONFIG_H
#   include "nbtool_config.h"
#  endif

#  include <sys/cdefs.h>
#  if defined(LIBC_SCCS) && !defined(lint)
__RCSID("$NetBSD: snprintb.c,v 1.37 2024/02/20 20:31:56 rillig Exp $");
#  endif

#  include <sys/types.h>
#  include <inttypes.h>
#  include <stdio.h>
#  include <util.h>
#  include <errno.h>
# else /* ! _KERNEL */
#  include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: snprintb.c,v 1.37 2024/02/20 20:31:56 rillig Exp $");
#  include <sys/param.h>
#  include <sys/inttypes.h>
#  include <sys/systm.h>
#  include <lib/libkern/libkern.h>
# endif /* ! _KERNEL */

# ifndef HAVE_SNPRINTB_M
typedef struct {
	char *const buf;
	size_t const bufsize;
	const char *bitfmt;
	uint64_t const val;
	size_t const line_max;

	const char *const num_fmt;
	unsigned const val_len;
	unsigned total_len;
	unsigned line_pos;
	unsigned comma_pos;
	char sep;
} state;

static void
store(state *s, char c)
{
	if (s->total_len < s->bufsize)
		s->buf[s->total_len] = c;
	s->total_len++;
}

static int
store_num(state *s, const char *fmt, uintmax_t num)
{
	int num_len = s->total_len < s->bufsize
	    ? snprintf(s->buf + s->total_len, s->bufsize - s->total_len,
		fmt, num)
	    : snprintf(NULL, 0, fmt, num);
	if (num_len > 0)
		s->total_len += num_len;
	return num_len;
}

static void
put_eol(state *s)
{
	if (s->total_len - s->line_pos > s->line_max) {
		s->total_len = (unsigned)(s->line_pos + s->line_max - 1);
		store(s, '#');
	}
	store(s, '\0');
	s->line_pos = s->total_len;
	s->comma_pos = 0;
	s->sep = '<';
}

static void
put_sep(state *s)
{
	if (s->sep == ',') {
		s->comma_pos = s->total_len;
		store(s, ',');
	} else {
		store(s, '<');
		s->sep = ',';
	}
}

static void
wrap_if_necessary(state *s, const char *bitfmt)
{
	if (s->line_max > 0
	    && s->comma_pos > 0
	    && s->total_len - s->line_pos >= s->line_max) {
		s->total_len = s->comma_pos;
		store(s, '>');
		put_eol(s);
		store_num(s, s->num_fmt, s->val);
		s->bitfmt = bitfmt;
	}
}

static int
old_style(state *s)
{
	while (*s->bitfmt != '\0') {
		const char *cur_bitfmt = s->bitfmt;
		uint8_t bit = *s->bitfmt;
		if (bit > ' ')
			return -1;
		if (s->val & (1U << (bit - 1))) {
			put_sep(s);
			while (*++s->bitfmt > ' ')
				store(s, *s->bitfmt);
			wrap_if_necessary(s, cur_bitfmt);
		} else
			while (*++s->bitfmt > ' ')
				continue;
	}
	return 0;
}

static int
new_style(state *s)
{
	uint64_t field = s->val;
	int matched = 1;
	const char *prev_bitfmt = s->bitfmt;
	while (*s->bitfmt != '\0') {
		const char *cur_bitfmt = s->bitfmt;
		uint8_t kind = *s->bitfmt++;
		uint8_t bit = *s->bitfmt++;
		switch (kind) {
		case 'b':
			prev_bitfmt = cur_bitfmt;
			if (((s->val >> bit) & 1) == 0)
				goto skip_description;
			put_sep(s);
			while (*s->bitfmt++ != '\0')
				store(s, s->bitfmt[-1]);
			wrap_if_necessary(s, cur_bitfmt);
			break;
		case 'f':
		case 'F':
			prev_bitfmt = cur_bitfmt;
			matched = 0;
			field = (s->val >> bit) &
			    (((uint64_t)1 << (uint8_t)*s->bitfmt++) - 1);
			put_sep(s);
			if (kind == 'F')
				goto skip_description;
			while (*s->bitfmt++ != '\0')
				store(s, s->bitfmt[-1]);
			store(s, '=');
			store_num(s, s->num_fmt, field);
			wrap_if_necessary(s, cur_bitfmt);
			break;
		case '=':
		case ':':
			/* Here "bit" is actually a value instead. */
			if (field != bit)
				goto skip_description;
			matched = 1;
			if (kind == '=')
				store(s, '=');
			while (*s->bitfmt++ != '\0')
				store(s, s->bitfmt[-1]);
			wrap_if_necessary(s, prev_bitfmt);
			break;
		case '*':
			s->bitfmt--;
			if (matched)
				goto skip_description;
			matched = 1;
			if (store_num(s, s->bitfmt, field) < 0)
				return -1;
			wrap_if_necessary(s, prev_bitfmt);
			/*FALLTHROUGH*/
		default:
		skip_description:
			while (*s->bitfmt++ != '\0')
				continue;
			break;
		}
	}
	return 0;
}

int
snprintb_m(char *buf, size_t bufsize, const char *bitfmt, uint64_t val,
	   size_t line_max)
{
#ifdef _KERNEL
	/*
	 * For safety; no other *s*printf() do this, but in the kernel
	 * we don't usually check the return value
	 */
	(void)memset(buf, 0, bufsize);
#endif /* _KERNEL */


	int old = *bitfmt != '\177';
	if (!old)
		bitfmt++;

	const char *num_fmt;
	switch (*bitfmt++) {
	case 8:
		num_fmt = "%#jo";
		break;
	case 10:
		num_fmt = "%ju";
		break;
	case 16:
		num_fmt = "%#jx";
		break;
	default:
		goto internal;
	}

	int val_len = snprintf(buf, bufsize, num_fmt, (uintmax_t)val);
	if (val_len < 0)
		goto internal;

	state s = {
		.buf = buf,
		.bufsize = bufsize,
		.bitfmt = bitfmt,
		.val = val,
		.line_max = line_max,

		.num_fmt = num_fmt,
		.val_len = val_len,
		.total_len = val_len,

		.sep = '<',
	};

	if ((old ? old_style(&s) : new_style(&s)) < 0)
		goto internal;

	if (s.sep != '<')
		store(&s, '>');
	if (s.line_max > 0) {
		put_eol(&s);
		if (s.bufsize >= 2 && s.total_len > s.bufsize - 2)
			s.buf[s.bufsize - 2] = '\0';
	}
	store(&s, '\0');
	if (s.bufsize >= 1 && s.total_len > s.bufsize - 1)
		s.buf[s.bufsize - 1] = '\0';
	return (int)(s.total_len - 1);
internal:
#ifndef _KERNEL
	errno = EINVAL;
#endif
	return -1;
}

int
snprintb(char *buf, size_t bufsize, const char *bitfmt, uint64_t val)
{
	return snprintb_m(buf, bufsize, bitfmt, val, 0);
}
# endif /* ! HAVE_SNPRINTB_M */
#endif /* ! _STANDALONE */
