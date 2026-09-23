 # Copyright (C) 2016  Nexell Co., Ltd.
 # Author: Sangjong, Han <hans@nexell.co.kr>
 #
 # This program is free software; you can redistribute it and/or
 # modify it under the terms of the GNU General Public License
 #
 # as published by the Free Software Foundation; either version 2
 # of the License, or (at your option) any later version.
 #
 # This program is distributed in the hope that it will be useful,
 # but WITHOUT ANY WARRANTY; without even the implied warranty of
 # MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 # GNU General Public License for more details.
 #
 # You should have received a copy of the GNU General Public License
 # along with this program.  If not, see <http://www.gnu.org/licenses/>.
###########################################################################
# Build Version info
###########################################################################
VERINFO				= V110

###########################################################################
# Build Environment
###########################################################################
# Values saved by `make menuconfig` (tools/menuconfig.py). They replace the
# ?= defaults below; variables given on the make command line still win.
-include .config.mak

DEBUG				= y

OPMODE				?= aarch64
#OPMODE				= aarch32

MEMTYPE				= DDR3
#MEMTYPE			= LPDDR3
MEMTEST				= n

#INITPMIC			= YES
INITPMIC			= NO

CRC_CHECK			= n

CHIPNAME			= S5P6818

CFLAGS				:=

SUPPORT_USB_BOOT		= y
SUPPORT_SDMMC_BOOT		= y

#BOARD				= SVT
#BOARD				= ASB
#BOARD				= DRONE
#BOARD				= AVN
#BOARD				= BF700
#BOARD				?= RAPTOR
BOARD				= GEC6818

# System Log Message
SYSLOG				?= y

# Secure Boot
SECURE_ON			?= 0

# Skip ARM Trusted Firmware (fip-loader.img/fip-secure.img) and jump
# directly from BL1 to the 3rd-stage image (u-boot). Brings up a minimal
# PSCI implementation in BL1 itself (see src/psci.c) since no BL31 will
# ever run. See also MULTICORE_BRING_UP in src/cfgBootDefine.h.
# SKIP_ATF=n (the fip-loader.img chain) needs OPMODE=aarch32.
SKIP_ATF			?= y

# SKIP_ATF: whether the u-boot BL1 jumps to is AArch64 (default) or
# AArch32. aarch32 needs OPMODE=aarch32: BL1 jumps straight to a 32-bit
# u-boot, still in secure SVC - no stage2, no PSCI, secondary cores left
# off. See docs/BOOT_MODES.md.
UBOOT_ARCH			?= aarch64

# Which device BL1 (and u-boot after it) boots from: sd or emmc. Picks
# the reference-nsih/raptor-*-64.txt header tools/mk_bl1_image.py builds
# BL1's header from, and patches its PortNumber byte to match.
BOOT_PORT			?= sd
# BOOT_LOGO=y: after DDR init, light up the RGB LCD (AT070TN92), show
# BOOT_LOGO_TEXT as a title, then one line per boot step (clocks, DDR,
# TrustZone, CPUs, loading and jumping to the next stage) and a progress
# bar at the bottom, pausing BOOT_LOGO_DELAY_MS after each step
# (src/display.c). LCD_BACKLIGHT=high|low also drives the backlight pad
# (GPIOD1/PWM0) to that level; the default leaves it alone, since which
# level means "on" is not known yet.
BOOT_LOGO			?= y
BOOT_LOGO_TEXT			?= S5P6818 BL1 Loading...
BOOT_LOGO_DELAY_MS		?= 500
LCD_BACKLIGHT			?= none

# cross-tool pre-header: one prefix per OPMODE, so `make OPMODE=...` picks
# the matching toolchain even when .config.mak was saved for the other one.
# CROSS_TOOL_aarch64 also builds the SKIP_ATF stage2 (STAGE2_CROSS_TOOL).
ifeq ($(OS),Windows_NT)
CROSS_TOOL_aarch32		?= arm-none-eabi-
else
CROSS_TOOL_aarch32		?= arm-eabi-
endif
CROSS_TOOL_aarch64		?= aarch64-none-elf-
#CROSS_TOOL_aarch64		?= aarch64-elf-
CROSS_TOOL			?= $(CROSS_TOOL_$(OPMODE))

###########################################################################
# Top Names
###########################################################################
PROJECT_NAME		= $(CHIPNAME)_2ndboot_$(OPMODE)_$(MEMTYPE)_$(VERINFO)

TARGET_NAME			= bl1-$(shell echo $(BOARD) | tr A-Z a-z)

LDS_NAME			= peridot_2ndboot_$(OPMODE)


###########################################################################
# Directories
###########################################################################
DIR_PROJECT_TOP			=

DIR_OBJOUTPUT			= obj
DIR_TARGETOUTPUT		= out

CODE_MAIN_INCLUDE		=

###########################################################################
# Build Environment
###########################################################################
ifeq ($(OPMODE) , aarch32)
ARCH			= armv7-a
CPU				= cortex-a15
endif
ifeq ($(OPMODE) , aarch64)
ARCH			= armv8-a
CPU				= cortex-a53+crc
endif

CC				= $(CROSS_TOOL)gcc
LD 				= $(CROSS_TOOL)ld
AS 				= $(CROSS_TOOL)as
AR 				= $(CROSS_TOOL)ar
MAKEBIN			= $(CROSS_TOOL)objcopy
OBJCOPY			= $(CROSS_TOOL)objcopy
RANLIB 			= $(CROSS_TOOL)ranlib

GCC_LIB			= $(shell $(CC) -print-libgcc-file-name)

ifeq ($(DEBUG), y)
CFLAGS			+= -DNX_DEBUG -Os
Q				=
else
CFLAGS			+= -DNX_RELEASE -Os
Q				= @
endif

###########################################################################
# MISC tools for MS-DOS
###########################################################################
ifeq ($(OS),Windows_NT)
MKDIR			= mkdir
RM				= del /q /F
MV				= move
CD				= cd
CP				= copy
ECHO			= echo
RMDIR			= rmdir /S /Q
else
MKDIR			= mkdir
RM				= rm -f
MV				= mv
CD				= cd
CP				= cp
ECHO				= echo
RMDIR				= rm -rf
endif
###########################################################################
# FLAGS
###########################################################################
ARFLAGS				= rcs
ARFLAGS_REMOVE			= -d
ARLIBFLAGS			= -v -s

ASFLAG				= -D__ASSEMBLY__ -D$(OPMODE)

# -fcommon: several prototype/module/*.h headers declare things like
# "enum {...} TZPORT;" (a missing `typedef`, so it's an accidental global
# variable definition, not a type) and get #included into many .c files.
# GCC <10 defaulted to -fcommon, letting the linker merge those repeated
# tentative definitions; GCC 10+ defaults to -fno-common and fails to
# link with "multiple definition of ...". Not something SKIP_ATF/this
# fork touches - it's just what building this old code with a modern
# toolchain needs.
CFLAGS				+=	-g -Wall -fcommon				\
					-Wextra -ffreestanding -fno-builtin	\
					-mlittle-endian						\
					-mcpu=$(CPU)						\
					$(CODE_MAIN_INCLUDE)				\
					-D__arm						\
					-DMEMTYPE_$(MEMTYPE)				\
					-DINITPMIC_$(INITPMIC)				\
					-D$(OPMODE) -D$(BOARD)
ifeq ($(SYSLOG), y)
CFLAGS				+=	-DSYSLOG_ON
endif

ifeq ($(SECURE_ON), 1)
CFLAGS				+=	-DSECURE_ON
endif

ifeq ($(SKIP_ATF), y)
CFLAGS				+=	-DSKIP_ATF
endif

ifeq ($(UBOOT_ARCH), aarch32)
ifneq ($(OPMODE)-$(SKIP_ATF), aarch32-y)
$(error UBOOT_ARCH=aarch32 needs OPMODE=aarch32 SKIP_ATF=y)
endif
CFLAGS				+=	-DUBOOT_AARCH32
else ifneq ($(UBOOT_ARCH), aarch64)
$(error UBOOT_ARCH must be aarch64 or aarch32)
endif

ifeq ($(BOOT_LOGO), y)
CFLAGS				+=	-DBOOT_LOGO -DBOOT_LOGO_TEXT='"$(BOOT_LOGO_TEXT)"'	\
					-DBOOT_LOGO_DELAY_MS=$(BOOT_LOGO_DELAY_MS)
ifeq ($(LCD_BACKLIGHT), high)
CFLAGS				+=	-DLCD_BACKLIGHT_LEVEL=1
endif
ifeq ($(LCD_BACKLIGHT), low)
CFLAGS				+=	-DLCD_BACKLIGHT_LEVEL=0
endif
endif

ifeq ($(OPMODE) , aarch32)
# -marm: the hand-written .S files (startup_aarch32.S etc.) are plain ARM
# with no Thumb interworking markup. Modern arm-linux-gnueabi- toolchains
# default C code to Thumb, and a Thumb "bl" into one of those ARM-only
# routines (e.g. __pllchange) has no state switch - the core decodes the
# target's ARM opcode bytes as Thumb and hits an Undefined Instruction
# almost immediately. Force ARM throughout so C and .S code share one
# instruction state and no interworking is needed at all.
CFLAGS				+=	-msoft-float					\
					-mstructure-size-boundary=32			\
					-marm
endif

ifeq ($(OPMODE) , aarch64)
ASFLAG				+=	-march=$(ARCH) -mcpu=$(CPU)

# -mstrict-align: avoid unaligned accesses in generated code. Without it,
# newer GCC (6/7+) can emit unaligned load/store pairs that fault when they
# land on this SoC's MMIO/packed-struct regions this code was never tuned
# against - see rafaello7/bl1-nanopi-m3 commit d7f82f0.
CFLAGS				+=	-mcmodel=small					\
					-march=$(ARCH) -mstrict-align
endif

ifeq ($(INITPMIC), YES)
CFLAGS				+=	-D$(BOARD)_PMIC_INIT
endif

ifeq ($(MEMTEST), y)
MEMTEST_TYPE		+=	SIMPLE
CFLAGS				+=	-D$(MEMTEST_TYPE)_MEMTEST
endif

ifeq ($(CRC_CHECK), y)
CHECKSUM			+=	CRC_CHECK
CFLAGS				+=	-D$(CHECKSUM)_ON
endif
