#	$NetBSD: Makefile,v 1.4 2011/01/18 00:16:13 jym Exp $
#

.include "${.CURDIR}/../Makefile.cdrom"

CDBASE=		boot			# gives ${CDBASE}.iso
CDBUILDEXTRA+=	boot.cfg		# Add boot.cfg file
CDEXTRA+=	${${MKZFS} != "no":?${RELEASEDIR}/amd64/installation/miniroot/zfsinstall.kmod:}
CLEANFILES+=	boot.cfg

prepare_md_post:
.if ${MKZFS:Uno} != "no"
	${TOOL_SED} "s/@@VERSION@@/${DISTRIBVER}/" < ${.CURDIR}/zfsboot.cfg.in > boot.cfg
.else
	${TOOL_SED} "s/@@VERSION@@/${DISTRIBVER}/" < ${.CURDIR}/boot.cfg.in > boot.cfg
.endif

.include "${.CURDIR}/../../../common/Makefile.bootcd"
