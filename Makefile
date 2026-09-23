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
include config.mak

LDFLAGS		=	-Bstatic							\
			-Wl,-Map=$(DIR_TARGETOUTPUT)/$(TARGET_NAME).map,--cref		\
			-T$(LDS_NAME).lds						\
			-Wl,--start-group						\
			-Lsrc/$(DIR_OBJOUTPUT)						\
			-Wl,--end-group							\
			-Wl,--build-id=none						\
			-nostdlib

SYS_OBJS	=	startup_$(OPMODE).o $(OPMODE)_libs.o $(OPMODE)_exception_handler.o secondboot.o subcpu.o sleep.o	\
				resetcon.o GPIO.o CRC32.o	SecureManager.o							\
				clockinit.o debug.o lib2ndboot.o buildinfo.o							\
				printf.o dumphex.o
SYS_OBJS	+=	sysbus.o

ifeq ($(MEMTYPE),DDR3)
SYS_OBJS	+=	init_DDR3.o
endif
ifeq ($(MEMTYPE),LPDDR3)
SYS_OBJS	+=	init_LPDDR3.o
endif

SYS_OBJS	+=	CRYPTO.o

ifeq ($(BOOT_LOGO),y)
SYS_OBJS	+=	display.o
endif
#SYS_OBJS	+=	nx_tieoff.o

ifeq ($(INITPMIC),YES)
SYS_OBJS	+=	i2c_gpio.o pmic.o
endif

ifeq ($(SUPPORT_USB_BOOT),y)
CFLAGS		+= -DSUPPORT_USB_BOOT
SYS_OBJS	+=	iUSBBOOT.o
endif

ifeq ($(SUPPORT_SDMMC_BOOT),y)
CFLAGS		+= -DSUPPORT_SDMMC_BOOT
SYS_OBJS	+=	iSDHCBOOT.o
endif

ifeq ($(MEMTEST),y)
SYS_OBJS	+=	memtester.o
endif

# psci.c is AArch64 EL3 SMC handling: part of whatever AArch64 code stays
# resident at EL3 under SKIP_ATF - this whole BL1 for OPMODE=aarch64, the
# stage2 below for OPMODE=aarch32.
ifeq ($(SKIP_ATF),y)
ifeq ($(OPMODE),aarch64)
SYS_OBJS	+=	psci.o
endif
endif

# SKIP_ATF + OPMODE=aarch32 + AArch64 u-boot: AArch32 BL1 hands off to
# an AArch64 stage2 (src/stage2_main.c) appended to its image - EL3
# vectors + PSCI, secondary cores parked, u-boot entered at EL2. Built by
# a sub-make with the AArch64 toolchain (STAGE2=y), see the stage2 target
# below. Not needed for UBOOT_ARCH=aarch32.
ifeq ($(SKIP_ATF)-$(OPMODE)-$(UBOOT_ARCH),y-aarch32-aarch64)
BUILD_STAGE2		= y
STAGE2_CROSS_TOOL	?= aarch64-none-elf-
STAGE2_ELF		= $(DIR_TARGETOUTPUT)/stage2.elf
STAGE2_BIN		= $(DIR_TARGETOUTPUT)/stage2.bin
endif

ifeq ($(STAGE2),y)
SYS_OBJS	=	startup_aarch64.o aarch64_libs.o aarch64_exception_handler.o	\
			stage2_main.o subcpu.o psci.o SecureManager.o			\
			debug.o printf.o lib2ndboot.o clockinit.o GPIO.o resetcon.o
endif

SYS_OBJS_LIST	=	$(addprefix $(DIR_OBJOUTPUT)/,$(SYS_OBJS))

SYS_INCLUDES	=	-I src				\
			-I prototype/base 		\
			-I prototype/module

###################################################################################################
$(DIR_OBJOUTPUT)/%.o: src/%.c
	@echo [compile....$<]
	$(Q)$(CC) -MMD $< -c -o $@ $(CFLAGS) $(SYS_INCLUDES)
###################################################################################################
$(DIR_OBJOUTPUT)/%.o: src/%.S
	@echo [compile....$<]
	$(Q)$(CC) -MMD $< -c -o $@ $(ASFLAG) $(CFLAGS) $(SYS_INCLUDES)
###################################################################################################


all: mkobjdir $(SYS_OBJS_LIST) link $(if $(BUILD_STAGE2),stage2) bin

stage2:
	@echo [stage2.... $(STAGE2_BIN)]
	$(Q)$(MAKE) STAGE2=y OPMODE=aarch64 CROSS_TOOL=$(STAGE2_CROSS_TOOL)	\
		DIR_OBJOUTPUT=obj-stage2 TARGET_NAME=stage2			\
		LDS_NAME=peridot_2ndboot_aarch64_stage2 stage2-image

stage2-image: mkobjdir $(SYS_OBJS_LIST) link
	$(Q)$(MAKEBIN) -O binary $(DIR_TARGETOUTPUT)/$(TARGET_NAME).elf $(DIR_TARGETOUTPUT)/$(TARGET_NAME).bin

mkobjdir:
ifeq ($(OS),Windows_NT)
	@if not exist $(DIR_OBJOUTPUT)			\
		@$(MKDIR) $(DIR_OBJOUTPUT)
	@if not exist $(DIR_TARGETOUTPUT)		\
		@$(MKDIR) $(DIR_TARGETOUTPUT)
else
#	@if [ ! -L prototype ] ; then			\
#		ln -s ../../../prototype/s5p6818/ prototype ; \
	fi
	@if	[ ! -e $(DIR_OBJOUTPUT) ]; then 	\
		$(MKDIR) $(DIR_OBJOUTPUT);		\
	fi;
	@if	[ ! -e $(DIR_TARGETOUTPUT) ]; then 	\
		$(MKDIR) $(DIR_TARGETOUTPUT);		\
	fi;
endif

link:
	@echo [link.... $(DIR_TARGETOUTPUT)/$(TARGET_NAME).elf]

	$(Q)$(CC) $(SYS_OBJS_LIST) $(LDFLAGS) -o $(DIR_TARGETOUTPUT)/$(TARGET_NAME).elf

bin:
	@echo [binary.... $(DIR_TARGETOUTPUT)/$(TARGET_NAME).bin]
	$(Q)$(MAKEBIN) -O binary $(DIR_TARGETOUTPUT)/$(TARGET_NAME).elf $(DIR_TARGETOUTPUT)/$(TARGET_NAME)-raw.bin
	@echo [header.... $(DIR_TARGETOUTPUT)/$(TARGET_NAME).bin, BOOT_PORT=$(BOOT_PORT)]
	$(Q)python3 tools/mk_bl1_image.py $(DIR_TARGETOUTPUT)/$(TARGET_NAME)-raw.bin -o $(DIR_TARGETOUTPUT)/$(TARGET_NAME).bin --port $(BOOT_PORT) --opmode $(OPMODE)	\
		$(if $(BUILD_STAGE2),--stage2 $(STAGE2_BIN)							\
		--stage2-addr 0x$$($(STAGE2_CROSS_TOOL)nm $(STAGE2_ELF) | awk '$$3 == "Startup" {print $$1}')	\
		--stage2-end 0x$$($(STAGE2_CROSS_TOOL)nm $(STAGE2_ELF) | awk '$$3 == "__bss_end__" {print $$1}')	\
		--bl1-end 0x$$($(CROSS_TOOL)nm $(DIR_TARGETOUTPUT)/$(TARGET_NAME).elf | awk '$$3 == "__bss_end__" {print $$1}'))
ifeq ($(OS),Windows_NT)
	@if exist $(DIR_OBJOUTPUT)			\
		@$(RM) $(DIR_OBJOUTPUT)\buildinfo.o
else
	@if	[ -e $(DIR_OBJOUTPUT) ]; then 		\
		$(RM) $(DIR_OBJOUTPUT)/buildinfo.o;	\
	fi;
endif

###################################################################################################
clean:
ifeq ($(OS),Windows_NT)
	@if exist $(DIR_OBJOUTPUT)			\
		@$(RMDIR) $(DIR_OBJOUTPUT)
	@if exist $(DIR_TARGETOUTPUT)			\
		@$(RMDIR) $(DIR_TARGETOUTPUT)
else
	@if [ -L prototype ] ; then			\
		$(RM) prototype ;			\
	fi
	@if	[ -e $(DIR_OBJOUTPUT) ]; then 		\
		$(RMDIR) $(DIR_OBJOUTPUT);		\
	fi;
	@$(RMDIR) obj-stage2
	@if	[ -e $(DIR_TARGETOUTPUT) ]; then 	\
		$(RMDIR) $(DIR_TARGETOUTPUT);		\
	fi;
endif

-include $(SYS_OBJS_LIST:.o=.d)

