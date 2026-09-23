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
DEBUG				= y

OPMODE				= aarch64
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

# Which device BL1 (and u-boot after it) boots from: sd or emmc. Picks
# the reference-nsih/raptor-*-64.txt header tools/mk_bl1_image.py builds
# BL1's header from, and patches its PortNumber byte to match.
BOOT_PORT			?= sd

# cross-tool pre-header
ifeq ($(OPMODE), aarch32)
ifeq ($(OS),Windows_NT)
CROSS_TOOL_TOP			=
CROSS_TOOL			= $(CROSS_TOOL_TOP)arm-none-eabi-
else
CROSS_TOOL_TOP			=
CROSS_TOOL			= $(CROSS_TOOL_TOP)arm-eabi-
endif
endif

ifeq ($(OPMODE), aarch64)
ifeq ($(OS),Windows_NT)
CROSS_TOOL_TOP			=
CROSS_TOOL			= $(CROSS_TOOL_TOP)aarch64-none-elf-
else
CROSS_TOOL_TOP			=
CROSS_TOOL			= $(CROSS_TOOL_TOP)aarch64-none-elf-
#CROSS_TOOL			= $(CROSS_TOOL_TOP)aarch64-elf-
endif
endif

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

ifeq ($(OPMODE) , aarch32)
CFLAGS				+=	-msoft-float					\
					-mstructure-size-boundary=32
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
