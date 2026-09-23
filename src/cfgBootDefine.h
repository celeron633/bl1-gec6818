/*
 *      Copyright (C) 2012 Nexell Co., All Rights Reserved
 *      Nexell Co. Proprietary & Confidential
 *
 *      NEXELL INFORMS THAT THIS CODE AND INFORMATION IS PROVIDED "AS IS" BASE
 *      AND WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING
 *      BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR
 *FITNESS
 *      FOR A PARTICULAR PURPOSE.
 *
 *      Module          : Base
 *      File            : cfgBootDefine.h
 *      Description     :
 *      Author          : Russell
 *      History         : 2014.09.29 Russell - First draft
 */

#ifndef __CFG_BOOT_DEFINE_H__
#define __CFG_BOOT_DEFINE_H__

//------------------------------------------------------------------------------
//  Load from NSIH On/Off.
//------------------------------------------------------------------------------
#define CFG_NSIH_EN (1)

//------------------------------------------------------------------------------
//  Chip product
//------------------------------------------------------------------------------
//#define ARCH_S5P4418
//#define ARCH_NXP4330
#define ARCH_NXP5430

//------------------------------------------------------------------------------
//  Memory Type
//------------------------------------------------------------------------------
#ifdef MEMTYPE_DDR3
#define MEM_TYPE_DDR3
#endif
#ifdef MEMTYPE_LPDDR3
#define MEM_TYPE_LPDDR23
#endif

//------------------------------------------------------------------------------
//  System optional.
//------------------------------------------------------------------------------
#define MULTICORE_SLEEP_CONTROL (1)
#if defined(SKIP_ATF) && defined(aarch64)
/* No BL31 will ever run to power on secondary cores for PSCI CPU_ON, so
 * the resident AArch64 code (the whole BL1 for OPMODE=aarch64, the
 * stage2 stub for OPMODE=aarch32) powers them on itself and parks them
 * at EL3 (subcpu.c SubCPUBoot(), psci.c). Off (0) for the normal ATF
 * chain, where BL31 owns secondary CPU power-on, and for the AArch32
 * BL1 build, which leaves that to its stage2. */
#define MULTICORE_BRING_UP (1)
#else
#define MULTICORE_BRING_UP (0)
#endif
#define CCI400_COHERENCY_ENABLE (1)
#define CONFIG_RESET_AFFINITY_ID (1) // Dependent kernel 3.18.x

#if defined(SKIP_ATF)
/*
 * SKIP_ATF with OPMODE=aarch32: the AArch64 stage2 (src/stage2_main.c,
 * peridot_2ndboot_aarch64_stage2.lds) is appended to BL1's image at this
 * address, so BootROM loads it into SRAM together with BL1. AArch32 BL1
 * resets CPU0 into it once u-boot is in DDR; it stays resident as the
 * EL3 PSCI firmware. Must match the .lds file's ". =".
 *
 * STAGE2_HANDOFF_ADDR: two words just below it - [0] =
 * STAGE2_AARCH64_SIGNATURE, [1] = u-boot entry - written by AArch32 BL1
 * before the reset (SRAM survives a CPU-only warm reset, registers don't).
 */
/*
 * 0xFFFF9000: leaves AArch32 BL1 about 6.6KB below the handoff words and
 * stage2 about 5KB below STAGE2_LIMIT (tools/mk_bl1_image.py checks both).
 */
#define STAGE2_AARCH64_ADDR (0xFFFF9000)
#define STAGE2_HANDOFF_ADDR (STAGE2_AARCH64_ADDR - 0x10)
#endif

#endif //	__CFG_BOOT_DEFINE_H__
