/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: Sangjong, Han <hans@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#define __SET_GLOBAL_VARIABLES
#include "sysheader.h"
#include "nx_bootheader.h"

//#define SIMPLE_MEMTEST 			(1)

#define EMA_VALUE (1) // Manual setting - 1: 1.1V, 3: 1.0V, 4: 0.95V

extern void DMC_Delay(int milisecond);

// extern void     flushICache(void);
// extern void     enableICache(CBOOL enable);

extern void enterSelfRefresh(void);
extern void exitSelfRefresh(void);
extern void set_bus_config(void);
extern void set_drex_qos(void);

extern CBOOL iUSBBOOT(struct NX_SecondBootInfo *const pTBI);
extern CBOOL iUARTBOOT(struct NX_SecondBootInfo *const pTBI);
extern CBOOL iSPIBOOT(struct NX_SecondBootInfo *const pTBI);
extern CBOOL iSDXCBOOT(struct NX_SecondBootInfo *const pTBI);
extern CBOOL iNANDBOOTEC(struct NX_SecondBootInfo *const pTBI);
extern CBOOL iSDXCFSBOOT(struct NX_SecondBootInfo *const pTBI);
extern void initClock(void);
#ifdef MEM_TYPE_DDR3
extern CBOOL init_DDR3(U32);
#endif
#ifdef MEM_TYPE_LPDDR23
extern CBOOL init_LPDDR3(U32);
#endif
extern CBOOL buildinfo(void);

extern void printClkInfo(void);

extern void ResetCon(U32 devicenum, CBOOL en);
extern CBOOL SubCPUBringUp(U32 CPUID);

extern void initPMIC(void);
extern void dowakeup(void);
extern void RomUSBBoot(U32 RomJumpAddr);
extern void SetSecureState(void);
extern int memtester_main(unsigned int start, unsigned int end);

extern int CRC_Check(void* buf, unsigned int size, unsigned int ref_crc);
#if defined(SKIP_ATF)
extern void EnterNonSecure(unsigned long entry, unsigned long context);
extern void psciInit(U32 bootCpu);
#endif
#if defined(BOOT_LOGO)
extern void DisplayInit(const char *title);
extern void DisplayStep(U32 percent, const char *fmt, ...);
extern U32 getquotient(U32 dividend, U32 divisor);
extern U32 NX_CLKPWR_GetPLLFreq(U32 PllNumber);
extern U32 NX_CLKPWR_GetSrcPll(U32 Divider);
extern U32 NX_CLKPWR_GetDivideValue(U32 Divider);

/* first-stage output of clock divider dvo (0: CPU, 1: BCLK, 2: MDCLK) */
static U32 dvo_mhz(U32 dvo)
{
	return getquotient(getquotient(
		NX_CLKPWR_GetPLLFreq(NX_CLKPWR_GetSrcPll(dvo)),
		NX_CLKPWR_GetDivideValue(dvo) & 0xFF), 1000000);
}
#else
#define DisplayStep(...) do { } while (0)
#endif

/* what pTBI is: u-boot directly, or ATF's BL2 in front of it */
#if defined(SKIP_ATF)
#define NEXT_STAGE_NAME "u-boot"
#else
#define NEXT_STAGE_NAME "fip-loader"
#endif

/* the whole chain this BL1 was built for, for the LCD */
#if !defined(SKIP_ATF)
#define BOOT_CHAIN "BL1 -> ATF BL2/BL31 -> u-boot"
#elif defined(aarch64)
#define BOOT_CHAIN "BL1 AArch64 EL3 -> u-boot EL2"
#elif defined(UBOOT_AARCH32)
#define BOOT_CHAIN "BL1 AArch32 -> u-boot AArch32"
#else
#define BOOT_CHAIN "BL1 AArch32 -> stage2 EL3 -> u-boot EL2"
#endif
extern U32 GetCurrentSMode(void);

void simple_memtest(U32 *pStart, U32 *pEnd);

#if (defined(SKIP_ATF) && defined(aarch32) && !defined(UBOOT_AARCH32)) || \
	(!defined(SKIP_ATF) && defined(aarch64))
/*
 * Warm-reset CPU0 into AArch64 EL3 at 'entry', the same TIEOFF +
 * warm-reset trick BootROM's NSIH vector uses to start an AArch64 BL1.
 * SRAM, DDR and the peripherals survive the reset. BL1 runs with the MMU
 * off, so its data accesses were never cached and nothing needs flushing.
 */
static void ResetCPU0ToAArch64(U32 entry)
{
	U32 temp = 0x10000000;

	while (!DebugIsUartTxDone() && temp--)
		;

	SetIO32(&pReg_Tieoff->TIEOFFREG[79], 1 << 12);	// CPU0 AArch64
	WriteIO32(&pReg_Tieoff->TIEOFFREG[80], entry >> 2); // CPU0 RVBAR
	while (ReadIO32(&pReg_Tieoff->TIEOFFREG[80]) != entry >> 2)
		;
	SetIO32(&pReg_ClkPwr->CPUWARMRESETREQ, 1);	// CPU0 warm reset
	while (1)
		__asm__ __volatile__("wfi");
}
#endif

#if defined(SKIP_ATF) && defined(aarch32) && !defined(UBOOT_AARCH32)
/*
 * Reset CPU0 into AArch64 at the stage2 stub (STAGE2_AARCH64_ADDR).
 * u-boot's entry goes through SRAM, which survives the reset.
 */
static void LaunchStage2(U32 entry)
{
	volatile U32 *handoff = (volatile U32 *)STAGE2_HANDOFF_ADDR;

	handoff[1] = entry;
	handoff[0] = STAGE2_AARCH64_SIGNATURE;

	SYSMSG("reset CPU0 into AArch64 stage2 @0x%08X, u-boot entry 0x%08X\r\n",
	       STAGE2_AARCH64_ADDR, entry);
	ResetCPU0ToAArch64(STAGE2_AARCH64_ADDR);
}
#endif

#if !defined(SKIP_ATF) && defined(aarch64)
/*
 * The prebuilt fip-loader.img was made for the vendor's AArch32 BL1: its
 * StartAddr is a small AArch32 stub - A32 "b", then "BOOTMAGICNUMBER!" -
 * that points CPU0's RVBAR at ATF BL2's real, AArch64 EL3 entry and
 * warm-resets CPU0 into it. This BL1 is already AArch64 and can't run it,
 * so find the entry the stub would load into RVBAR ("ldr r6, [pc, #imm];
 * lsr r6, r6, #2") and do the reset itself.
 *
 * Returns that entry, or 0 if 'start' isn't such a stub.
 */
static U32 AtfLoaderEntry64(U32 start)
{
	const volatile U32 *p = (const volatile U32 *)(MPTRS)start;
	U32 code, insn, i;

	if ((p[0] & 0xFF000000) != 0xEA000000 ||	// b (always)
	    p[1] != 0x544F4F42 || p[2] != 0x4947414D ||	// "BOOTMAGI"
	    p[3] != 0x4D554E43 || p[4] != 0x21524542)	// "CNUMBER!"
		return 0;

	code = start + 8 + ((S32)(p[0] << 8) >> 6);
	p = (const volatile U32 *)(MPTRS)code;
	for (i = 0; i < 64; i++) {
		insn = p[i];
		if ((insn & 0xFFFFF000) == 0xE59F6000 &&	// ldr r6, [pc, #imm]
		    p[i + 1] == 0xE1A06126)			// lsr r6, r6, #2
			return *(const volatile U32 *)(MPTRS)
				(code + i * 4 + 8 + (insn & 0xFFF));
	}
	return 0;
}
#endif

#if defined(RAPTOR)
/*
 * Raptor board - revision check. (H/W: GPIOE 4,5,6)
 * GPIOE4 (Least Bit), GPIOE6(Most Bit)
 */
unsigned int raptor_check_hw_revision(void)
{
	volatile unsigned int *reg = 0;
	unsigned int val = 0;

	/*
	 * Check to GPIOE PORT
	 * Read to GPIOPAD Status Register
	 */
	reg = (volatile unsigned int*)(0xC001E000 + 0x18);
	val = (*reg & (0x7 << 4)) >> 4;

        return val;
}
#endif

/*
 * PWM Module Reset
 * This function is temporary code.
 */

void pwm_reset(void)
{
	int reset_number = RESETINDEX_OF_PWM_MODULE_PRESETn;

	ResetCon(reset_number, CTRUE);	// reset on
	ResetCon(reset_number, CFALSE); // reset negate
}

/*
 * TIMER Module Reset
 * This function is temporary code.
 */

void timer_reset(void)
{
	int reset_number = RESETINDEX_OF_TIMER_MODULE_PRESETn;

	ResetCon(reset_number, CTRUE);	// reset on
	ResetCon(reset_number, CFALSE); // reset negate
}

void device_set_env(void)
{
	/* (Device Port Number) for U-BOOT  */
	unsigned int dev_portnum = pSBI->DBI.SDMMCBI.PortNumber;
	WriteIO32(&pReg_ClkPwr->SCRATCH[1], dev_portnum );
}

//------------------------------------------------------------------------------
#if (CCI400_COHERENCY_ENABLE == 1)
void initCCI400(void)
{
	// before set barrier instruction.
	SetIO32(&pReg_CCI400->SCR, 1 << 0); // static bus disable speculative fetches
	SetIO32(&pReg_CCI400->SCR, 1 << 1); // SFR bus disable speculative fetches

	WriteIO32(&pReg_CCI400->COR, (1UL << 3)); // protect to send barrier command to drex

	WriteIO32(&pReg_CCI400->CSI[BUSID_CS].SCR, 0); // snoop request disable
	WriteIO32(&pReg_CCI400->CSI[BUSID_CODA].SCR,
		  0);					// snoop request disable
	WriteIO32(&pReg_CCI400->CSI[BUSID_TOP].SCR, 0); // snoop request disable

#if (MULTICORE_BRING_UP == 1)
	WriteIO32(&pReg_CCI400->CSI[BUSID_CPUG0].SCR,
		  0x3); // cpu 0~3 Snoop & DVM Req
	while (ReadIO32(&pReg_CCI400->STSR) & 0x1);

	/*
	 * Not cpu 4~7 yet: that cluster stays powered off until the kernel's
	 * PSCI CPU_ON, and with its port taking DVM requests, the kernel's
	 * first broadcast TLB/I-cache maintenance on CPU0 waits for it
	 * forever (hung right after "Starting kernel"). Its first core to
	 * come up enables the port itself - subcpu.c JoinCCI().
	 */
	WriteIO32(&pReg_CCI400->CSI[BUSID_CPUG1].SCR, 0x0);
#else
	WriteIO32(&pReg_CCI400->CSI[BUSID_CPUG0].SCR, 0x0);
	WriteIO32(&pReg_CCI400->CSI[BUSID_CPUG1].SCR, 0x0);
#endif
}
#endif // #if (CCI400_COHERENCY_ENABLE == 1)

struct NX_CLKPWR_RegisterSet *const clkpwr;

//------------------------------------------------------------------------------
void BootMain(U32 CPUID)
{
	struct NX_SecondBootInfo TBI;
	struct NX_SecondBootInfo *pTBI = &TBI; // third boot info
	CBOOL Result = CFALSE;
	register volatile U32 temp;
	U32 signature, isResume = 0;
	U32 debugCH = 0;

#ifdef RAPTOR_PMIC_INIT
	debugCH = 3;
#endif

	//--------------------------------------------------------------------------
	// Set EMA
	//--------------------------------------------------------------------------

	// Set EMA for CPU Cluster0
	temp = ReadIO32(&pReg_Tieoff->TIEOFFREG[94]) &
	       ~((0x7 << 23) | (0x7 << 17));
	temp |= ((EMA_VALUE << 23) | (EMA_VALUE << 17));
	WriteIO32(&pReg_Tieoff->TIEOFFREG[94], temp);

	// Set EMA for CPU Cluster1
	temp = ReadIO32(&pReg_Tieoff->TIEOFFREG[111]) &
	       ~((0x7 << 23) | (0x7 << 17));
	temp |= ((EMA_VALUE << 23) | (EMA_VALUE << 17));
	WriteIO32(&pReg_Tieoff->TIEOFFREG[111], temp);

	//--------------------------------------------------------------------------
	// Set Affinity ID
	//--------------------------------------------------------------------------
#if (CONFIG_RESET_AFFINITY_ID == 1)
	// Set Affinity level1 for CPU Cluster1
	temp = ReadIO32(&pReg_Tieoff->TIEOFFREG[95]) & 0x00FFFFFF;
	temp |= (1 << 24);
	WriteIO32(&pReg_Tieoff->TIEOFFREG[95], temp);

	// Set Affinity level2 for CPU Cluster1
	temp = ReadIO32(&pReg_Tieoff->TIEOFFREG[96]) & 0xF0;
	//    temp |= (1 << 0);
	WriteIO32(&pReg_Tieoff->TIEOFFREG[96], temp);
#endif

	//--------------------------------------------------------------------------
	// Init debug
	//--------------------------------------------------------------------------
	DebugInit(debugCH);

	printf("\r\n\r\n===== BL1 BootMain: CPU%d, debugCH=%d =====\r\n", CPUID, debugCH);

	WriteIO32(&pReg_Alive->ALIVEPWRGATEREG, 1);
	WriteIO32(&pReg_Alive->VDDCTRLSETREG, 0x000003FC); //; Retention off (Pad hold off)

	if (USBREBOOT_SIGNATURE == ReadIO32(&pReg_Alive->ALIVESCRATCHVALUE5))
		RomUSBBoot((U32)0x0000009C);
#if !defined(LOAD_FROM_USB)
		SetIO32(&pReg_RstCon->REGRST[RESETINDEX_OF_WDT_MODULE_PRESETn >> 5], 1 << (RESETINDEX_OF_WDT_MODULE_PRESETn & 0x1F));
		SetIO32(&pReg_RstCon->REGRST[RESETINDEX_OF_WDT_MODULE_nPOR >> 5], 1 << (RESETINDEX_OF_WDT_MODULE_nPOR & 0x1F));
		WriteIO32(&pReg_WDT->WTCON,
				0xFF << 8 |		// prescaler value
				0x03 << 3 |		// division factor (3:128)
				0x01 << 2);		// watchdog reset enable
		WriteIO32(&pReg_WDT->WTCNT, 0xFFFF);	// 200MHz/256/128 = 6103.515625, 65536/6103.5 = 10.74 sec
//		SetIO32  ( &pReg_WDT->WTCON, 0x01<<5);          // watchdog timer enable
#endif
	//--------------------------------------------------------------------------
	// Get resume information.
	//--------------------------------------------------------------------------
	signature = ReadIO32(&pReg_Alive->ALIVESCRATCHREADREG);
	if ((SUSPEND_SIGNATURE == (signature & 0xFFFFFF00)) && ReadIO32(&pReg_Alive->WAKEUPSTATUS)) {
		isResume = 1;
	}

	/* Arm Trusted Firmware */
	signature = ReadIO32(&pReg_Alive->ALIVESCRATCHVALUE4);
	if ((ATF_SUSPEND_SIGNATURE == (signature & 0xFFFFFF00)) && ReadIO32(&pReg_Alive->WAKEUPSTATUS)) {
		isResume = 1;
	}

	printf("isResume=%d (ALIVESCRATCHREADREG=0x%08X, ALIVESCRATCHVALUE4=0x%08X, WAKEUPSTATUS=0x%08X)\r\n",
	       isResume, (unsigned)ReadIO32(&pReg_Alive->ALIVESCRATCHREADREG),
	       (unsigned)ReadIO32(&pReg_Alive->ALIVESCRATCHVALUE4),
	       (unsigned)ReadIO32(&pReg_Alive->WAKEUPSTATUS));

	/*
	 * SD/MMC,SPI - port number stored for u-boot.
	 */
	device_set_env();

//--------------------------------------------------------------------------
// Initialize PMIC device.
//--------------------------------------------------------------------------
#if defined(INITPMIC_YES)
	initPMIC();
#endif

	SYSMSG("EMA is %s\r\n", (EMA_VALUE == 1) ? "1.1V" : (EMA_VALUE == 3) ? "1.0V" : "0.95V");
	SYSMSG("\r\n\nWorking to aarch%d\r\nwaiting for pll change..\r\n", sizeof(void *) * 8);

	while (!DebugIsUartTxDone());

	//--------------------------------------------------------------------------
	// Change to PLL.
	//--------------------------------------------------------------------------
	initClock();

	//--------------------------------------------------------------------------
	// Debug Console
	//--------------------------------------------------------------------------
	DebugInit(debugCH);

	//--------------------------------------------------------------------------
	// build information. version, build time and date
	//--------------------------------------------------------------------------
#if 1
	buildinfo();
#else
	if (buildinfo() == CFALSE) {
		printf("WARNING : NSIH mismatch...!!!\r\n");
		while (1);
	}
#endif

	//--------------------------------------------------------------------------
	// boot mode banner - which chain this binary was built for, and what
	// EL/CPU we're actually running on right now (for serial log triage)
	//--------------------------------------------------------------------------
#if defined(SKIP_ATF)
	printf("boot mode: SKIP_ATF (direct to u-boot, no fip-loader/fip-secure)\r\n");
#else
	printf("boot mode: ATF (fip-loader.img -> fip-secure.img -> fip-nonsecure.img)\r\n");
#endif
	printf("CPU%d, EL%d, MULTICORE_BRING_UP=%d\r\n",
	       CPUID, GetCurrentSMode(), MULTICORE_BRING_UP);

	//--------------------------------------------------------------------------
	// print clock information
	//--------------------------------------------------------------------------
	printClkInfo();

	SYSMSG("\r\nDDR3 POR Init Start %d\r\n", isResume);
#ifdef MEM_TYPE_DDR3
#if 0
	if (init_DDR3(isResume) == CFALSE)
		init_DDR3(isResume);
#else
	if (init_DDR3(0) == CFALSE)
		init_DDR3(0);
#endif
#endif
#ifdef MEM_TYPE_LPDDR23
	if (init_LPDDR3(0) == CFALSE)
		init_LPDDR3(0);
#endif

	if (isResume)
		exitSelfRefresh();

	SYSMSG("DDR3 Init Done!\r\n");

	set_bus_config();
	set_drex_qos();
	/* Temporary Code - PWM Reset */
	pwm_reset();
	/* Temporary Code - Timer Reset */
	timer_reset();

#if defined(BOOT_LOGO)
	/* needs DDR for the framebuffer; on resume the kernel owns the LCD */
	if (!isResume) {
		DisplayInit(BOOT_LOGO_TEXT);
		DisplayStep(5, "Built " __DATE__ " " __TIME__);
		DisplayStep(10, "Chain: " BOOT_CHAIN);
		DisplayStep(15, "Clocks: CPU %d MHz, DDR %d MHz, BUS %d MHz",
			    dvo_mhz(0), dvo_mhz(2), dvo_mhz(1));
		DisplayStep(25, "DDR3 init done");
	}
#endif

#if (CCI400_COHERENCY_ENABLE == 1)
	SYSMSG("CCI Init!\r\n");
	initCCI400();
#endif

	SYSMSG("SetSecureState (TZPC/TZASC/GIC)...\r\n");
	SetSecureState();
	SYSMSG("SetSecureState done\r\n");
	DisplayStep(35, "TrustZone set up (TZPC/TZASC/GIC)");

	SYSMSG("Wakeup CPU ");

#if (MULTICORE_BRING_UP == 1)
#if defined(SKIP_ATF)
	psciInit(CPUID);
#endif
	SubCPUBringUp(CPUID);
	DisplayStep(45, "Secondary CPUs: off until PSCI CPU_ON");
#else
	SYSMSG("(MULTICORE_BRING_UP=0, secondary cores left as-is)\r\n");
#endif

	if (isResume) {
		SYSMSG(" DDR3 SelfRefresh exit Done!\r\n0x%08X\r\n", 
			ReadIO32(&pReg_Alive->WAKEUPSTATUS));
		dowakeup();
	}
	WriteIO32(&pReg_Alive->ALIVEPWRGATEREG, 0);

	if (pSBI->SIGNATURE != HEADER_ID)
		printf("2nd Boot Header is invalid, Please check it out!\r\n");

#ifdef SIMPLE_MEMTEST
	simple_memtest((U32 *)0x40000000UL, (U32 *)0xBFFF0000);
#endif

	printf("LoadDevice=%d (0:USB 1:SPI 2:NAND 3:SDMMC 4:SDFS 5:UART)\r\n",
	       pSBI->DBI.SPIBI.LoadDevice);
#if defined(BOOT_LOGO)
	{
		static const char *const devices[] = {
			"USB", "SPI", "NAND", "SD/MMC", "SD FAT", "UART" };
		U32 dev = pSBI->DBI.SPIBI.LoadDevice;

		if (dev == BOOT_FROM_SDMMC)
			DisplayStep(55, "Loading " NEXT_STAGE_NAME
				    " from SD/MMC%d, offset 0x%X",
				    pSBI->DBI.SDMMCBI.PortNumber,
				    pSBI->DEVICEADDR);
		else
			DisplayStep(55, "Loading " NEXT_STAGE_NAME " from %s...",
				    dev < 6 ? devices[dev] : "?");
	}
#endif

	switch (pSBI->DBI.SPIBI.LoadDevice) {
#if defined(SUPPORT_USB_BOOT)
	case BOOT_FROM_USB:
		SYSMSG("Loading from usb...\r\n");
		Result = iUSBBOOT(pTBI); // for USB boot
		break;
#endif

#if defined(SUPPORT_SPI_BOOT)
	case BOOT_FROM_SPI:
		SYSMSG("Loading from spi...\r\n");
		Result = iSPIBOOT(pTBI); // for SPI boot
		break;
#endif

#if defined(SUPPORT_NAND_BOOT)
	case BOOT_FROM_NAND:
		SYSMSG( "Loading from nand...\r\n" );
		Result = iNANDBOOTEC(pTBI);     // for NAND boot
		break;
#endif

#if defined(SUPPORT_SDMMC_BOOT)
	case BOOT_FROM_SDMMC:
		SYSMSG("Loading from sdmmc...\r\n");
		Result = iSDXCBOOT(pTBI); // for SD boot
		break;
#endif

#if defined(SUPPORT_SDFS_BOOT)
	case BOOT_FROM_SDFS:
		SYSMSG("Loading from sd FATFS...\r\n");
		Result = iSDXCFSBOOT(pTBI); // for SDFS boot
		break;
#endif

#if defined(SUPPORT_UART_BOOT)
	case BOOT_FROM_UART:
		SYSMSG("Loading from uart...\r\n");
		Result = iUARTBOOT(pTBI);       // for UART boot
		break;
#endif
	default:
		printf("LoadDevice=%d has no boot method compiled in - check "
		       "config.mak SUPPORT_*_BOOT and the NSIH boot-from field\r\n",
		       pSBI->DBI.SPIBI.LoadDevice);
		break;
	}

	printf("boot device load result: %s\r\n", Result ? "OK" : "FAILED");

#ifdef CRC_CHECK_ON
	Result = CRC_Check((void*)pTBI->LOADADDR, (unsigned int)pTBI->LOADSIZE
				,(unsigned int)pTBI->DBI.SDMMCBI.CRC32);
#endif
	if (Result) {
		void (*pLaunch)(U32, U32) =
		    (void (*)(U32, U32))((MPTRS)pTBI->LAUNCHADDR);
		SYSMSG(" Image Loading Done!\r\n");
#if defined(BOOT_LOGO)
		{
			/* SD reads the 3rd-stage header layout (nx_bootheader.h),
			 * USB boot the old NX_SecondBootInfo one */
			const struct nx_tbbinfo *tbbi =
				&((struct nx_bootheader *)pTBI)->tbbi;
			int sd = pSBI->DBI.SPIBI.LoadDevice == BOOT_FROM_SDMMC;

			DisplayStep(85, "Loaded %d KB to 0x%08X",
				    (sd ? tbbi->loadsize : pTBI->LOADSIZE) >> 10,
				    sd ? (U32)tbbi->loadaddr : pTBI->LOADADDR);
		}
#endif
		DisplayStep(100, "Jumping to " NEXT_STAGE_NAME " at 0x%08X",
			    (U32)(MPTRS)pLaunch);
		SYSMSG("Launch to 0x%08X, currently EL%d\r\n",
		       (MPTRS)pLaunch, GetCurrentSMode());
#if defined(SKIP_ATF) && defined(aarch64)
		/*
		 * No fip-loader.img/fip-secure.img (ATF BL2/BL31) in this
		 * chain: pTBI->LAUNCHADDR points straight at the next stage
		 * (e.g. u-boot), which expects to run at EL2 non-secure like
		 * BL31 would normally hand it off - not at EL3, which is
		 * where we still are here.
		 *
		 * eret straight from EL3 to the entry. BL1 must not run
		 * another instruction non-secure: SetTZPC() makes all of the
		 * internal SRAM (BL1's code, stack and the resident PSCI
		 * handler) secure-only, so a non-secure fetch from it faults
		 * - and the fault vectors are in that SRAM too. The old
		 * SwitchToEL2()-and-return did exactly that and hung silently
		 * right after "Launch to" on the board.
		 */
		SYSMSG("entering u-boot at 0x%08X, EL2 non-secure\r\n",
		       (MPTRS)pLaunch);
		temp = 0x10000000;
		while (!DebugIsUartTxDone() && temp--);
		EnterNonSecure((MPTRS)pLaunch, 0);
#endif
#if defined(SKIP_ATF) && defined(aarch32) && !defined(UBOOT_AARCH32)
		/* u-boot is AArch64 and wants a resident EL3 for PSCI: hand
		 * over to the AArch64 stage2 instead of jumping there. */
		LaunchStage2(pTBI->LAUNCHADDR);
#endif
#if !defined(SKIP_ATF) && defined(aarch64)
		{
			U32 bl2 = AtfLoaderEntry64(pTBI->LAUNCHADDR);

			/* no stub: an AArch64 image, entered at EL3 below */
			if (bl2) {
				SYSMSG("0x%08X is an AArch32 loader stub, reset "
				       "CPU0 into its AArch64 entry 0x%08X\r\n",
				       (MPTRS)pLaunch, bl2);
				ResetCPU0ToAArch64(bl2);
			}
		}
#endif
		/* UBOOT_AARCH32: a 32-bit u-boot is entered directly, in
		 * secure SVC mode like BL1 itself. */
		temp = 0x10000000;
		while (!DebugIsUartTxDone() && temp--);
		pLaunch(0, 4330);
	}

	printf(" Image Loading Failure Try to USB boot\r\n");
	DisplayStep(0, "Loading " NEXT_STAGE_NAME " FAILED, trying USB boot");
	temp = 0x10000000;
	while (!DebugIsUartTxDone() && temp--);
	RomUSBBoot((U32)0x0000009C);
	while (1);
}
