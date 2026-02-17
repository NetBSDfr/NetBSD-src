#	$NetBSD$

.include <bsd.own.mk>

PROG=	bsddialog

SRCS=	bsddialog.c util_builders.c util_cli.c util_theme.c

LDADD+=	-lbsddialog -lcurses
DPADD+=	${LIBBSDDIALOG} ${LIBCURSES}

.include "bsd.prog.mk"
