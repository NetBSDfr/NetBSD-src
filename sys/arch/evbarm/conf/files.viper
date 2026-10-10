#	$NetBSD: files.viper,v 1.2 2005/12/11 12:17:07 christos Exp $
#
# Arcom Viper
#

# homegrown stuff since initarm_common seems dead

# CPU and I/O
include	"arch/arm/xscale/files.pxa2x0"

# SMSC LAN91C111
attach	sm at pxaip with sm_pxaip
