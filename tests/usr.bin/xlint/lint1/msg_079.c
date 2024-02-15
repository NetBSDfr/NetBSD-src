/*	$NetBSD: msg_079.c,v 1.6 2024/02/02 19:07:58 rillig Exp $	*/
# 3 "msg_079.c"

// Test for message: dubious escape \%c [79]

/* \e is only accepted in GCC mode. */

/* lint1-extra-flags: -X 351 */

char char_e = '\e';
/* expect+1: warning: dubious escape \y [79] */
char char_y = '\y';
int wide_e = L'\e';
/* expect+1: warning: dubious escape \y [79] */
int wide_y = L'\y';

char char_string_e[] = "\e[0m";
/* expect+1: warning: dubious escape \y [79] */
char char_string_y[] = "\y[0m";
int wide_string_e[] = L"\e[0m";
/* expect+1: warning: dubious escape \y [79] */
int wide_string_y[] = L"\y[0m";
