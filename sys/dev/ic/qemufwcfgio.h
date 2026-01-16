/* $NetBSD: qemufwcfgio.h,v 1.1 2017/11/25 16:31:03 jmcneill Exp $ */

/*-
 * Copyright (c) 2017 Jared McNeill <jmcneill@invisible.ca>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _QEMUFWCFGIO_H
#define _QEMUFWCFGIO_H

#include <sys/ioccom.h>

/* Fixed selector keys */
#define	FW_CFG_SIGNATURE	0x0000	/* Signature */
#define	FW_CFG_ID		0x0001	/* Revision / feature bitmap */
#define	FW_CFG_FILE_DIR		0x0019	/* File directory */
#define	FW_CFG_FILE_FIRST	0x0020	/* First file in directory */

#define	FWCFGIO_SET_INDEX	_IOW('q', 0, uint16_t)

/* Register locations are MD */
#if defined(__i386__) || defined(__x86_64__)
#define	FWCFG_SEL_REG		0x00
#define	FWCFG_SEL_SWAP		htole16
#define	FWCFG_DATA_REG		0x01
#define	FWCFG_DMA_ADDR		0x04
/*
 * fw_cfg I/O port base address and sizes for x86
 */
#define	FWCFG_IO_BASE		0x510
#define	FWCFG_IO_SIZE		0x0c
#define	FWCFG_DMA_IO_SIZE	0x14	/* includes DMA registers */
#elif defined(__arm__) || defined(__aarch64__)
#define	FWCFG_SEL_REG		0x08
#define	FWCFG_SEL_SWAP		htobe16
#define	FWCFG_DATA_REG		0x00
#define	FWCFG_DMA_ADDR		0x10
#elif defined(__riscv)
#define	FWCFG_SEL_REG		0x08
#define	FWCFG_SEL_SWAP		htobe16
#define	FWCFG_DATA_REG		0x00
#define	FWCFG_DMA_ADDR		0x10
#else
#error driver does not support this architecture
#endif

#endif /* !_QEMUFWCFGIO_H */
