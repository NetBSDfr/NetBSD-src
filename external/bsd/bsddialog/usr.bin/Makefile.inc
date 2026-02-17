# $NetBSD$

.include "../Makefile.inc"

BINDIR=		/usr/bin

CPPFLAGS+=	-I${BSDDIALOG_DISTDIR}/lib

.PATH:		${BSDDIALOG_DISTDIR}/utility
