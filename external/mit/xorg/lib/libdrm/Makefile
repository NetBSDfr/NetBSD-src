#	$NetBSD: Makefile,v 1.19 2022/07/11 19:33:33 mrg Exp $

.include <bsd.own.mk>

LIB=	drm

# smolBSD: vendored copy of upstream libdrm 2.4.134 plus our small
# patch, kept here instead of maintaining an xsrc fork:
#   - xf86drm.c: virtio-mmio busid fallback (microvm has no PCI);
#   - meson.build: cc.find_library('pci') on NetBSD (meson only).
# Re-apply when bumping xsrc libdrm; shipped via diverges/amd64.list.
X11SRCDIR.drm=		${NETBSDSRCDIR}/xsrc-smol/external/mit/libdrm/dist

.PATH:	${X11SRCDIR.${LIB}}

SRCS=	xf86drm.c xf86drmHash.c xf86drmMode.c xf86drmRandom.c xf86drmSL.c

INCS=	xf86drm.h xf86drmMode.h libsync.h
INCSDIR=${X11INCDIR}

CPPFLAGS+=	-I${DESTDIR}${X11INCDIR}/libdrm -I${X11SRCDIR.${LIB}}/../include

SUBDIR=	drm

PKGCONFIG=	libdrm
PKGDIST=	${LIB}

COPTS.xf86drm.c+=	${${ACTIVE_CC} == "gcc":? -Wno-error=cpp :}
COPTS.xf86drm.c+=	${${ACTIVE_CC} == "clang":? -Wno-error=\#warnings :}

CWARNFLAGS.clang+=	-Wno-conversion -Wno-tautological-compare

LIBDPLIBS=	m	${.CURDIR}/../../../../../lib/libm
LIBDPLIBS+=	pci	${.CURDIR}/../../../../../lib/libpci

.if 0
MAN=		drm.7 \
		drm-kms.7 \
		drm-memory.7 \
		drmAvailable.3 \
		drmHandleEvent.3 \
		drmModeGetResources.3
# XXX is this right?
MLINKS+=	drm-mm.7 drm-gem.7 \
		drm-mm.7 drm-ttm.7
.endif

.include "Makefile.defines"

.include <bsd.x11.mk>
.include <bsd.lib.mk>
.include <bsd.subdir.mk>
