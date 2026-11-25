#	$NetBSD: Makefile.inc,v 1.1 2026-11-25 00:00:00 imil Exp $
# smolBSD project
#
# wasm32 contributes no MD libkern sources: string/integer helpers come
# from the MI C sources, and the atomic operations from the compiler's
# __atomic builtins (which lower to native wasm atomic instructions).

# ffs() is not in the default SRCS; architectures supply their own,
# but the generic C implementation resolves from the libc gen .PATH.
SRCS+=	ffs.c
