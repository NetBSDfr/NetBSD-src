# 
#	$NetBSD: files.adb,v 1.10 2020/08/31 17:54:18 macallan Exp $
#
# Apple Desktop Bus protocol and drivers

defflag	adbdebug.h	ADB_DEBUG
defflag	adbdebug.h	ADBKBD_DEBUG
defflag	adbdebug.h	ADBMS_DEBUG
defflag	adbdebug.h	ADBBT_DEBUG
defflag adbdebug.h	ADBKBD_POWER_DDB
defflag	adbdebug.h	KTM_DEBUG

define adb_bus {}

device nadb {}
attach nadb at adb_bus

device adbkbd : wskbddev, wsmousedev, sysmon_power, sysmon_taskq
attach adbkbd at nadb
defflag	opt_adbkbd.h	ADBKBD_EMUL_USB
defparam opt_adbkbd.h	ADBKBD_LAYOUT

device adbbt : wskbddev
attach adbbt at nadb

device adbms : wsmousedev
attach adbms at nadb

device ktm : wsmousedev
attach ktm at nadb
