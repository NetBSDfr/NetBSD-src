#	$NetBSD$

IMAGE=			ramdisk-zfsinstall.fs
IMAGESIZE=		5000k
IMAGEDEPENDS=
MAKEDEVTARGETS=		all
CRUNCHENV=		INIT_CHROOT=1
SMALLPROG_INET6=	1

# Build as re-entrant because zpool requires libhack built this way.
CPPFLAGS+=		-D_REENTRANT

LISTS+=			${DISTRIBDIR}/common/list.zfsinstall

.include "${.CURDIR}/../common/Makefile.ramdisk"
.include "${DISTRIBDIR}/common/Makefile.makedev"

LISTS+=			${.CURDIR}/list

MTREECONF+=		${DISTRIBDIR}/common/mtree.zfsroot
