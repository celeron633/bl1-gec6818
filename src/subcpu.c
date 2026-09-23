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
#include "sysheader.h"

extern void ResetCon(U32 devicenum, CBOOL en);
extern void DMC_Delay(int milisecond);

//------------------------------------------------------------------------------
#if (MULTICORE_BRING_UP == 1)
#ifdef aarch32
void BringUpSlaveCPU(U32 CPUID)
{
	WriteIO32(&pReg_ClkPwr->CPURESETMODE, 0x1);
	WriteIO32(&pReg_ClkPwr->CPUPOWERDOWNREQ, (1 << CPUID));
	WriteIO32(&pReg_ClkPwr->CPUPOWERONREQ, (1 << CPUID));
}

void SetVectorLocation(U32 CPUID, CBOOL LowHigh)
{
	U32 addr, bits, regvalue;

	if (CPUID & 0x4) // cpu 4, 5, 6, 7
	{
		addr = (U32)&pReg_Tieoff->TIEOFFREG[95];
		bits = 1 << (12 + (CPUID & 0x3));
	} else {
		addr = (U32)&pReg_Tieoff->TIEOFFREG[78];
		bits = 1 << (20 + (CPUID & 0x3));
	}

	regvalue = ReadIO32(addr);
	if (LowHigh)
		regvalue |= bits;
	else
		regvalue &= ~bits;
	WriteIO32(addr, regvalue);
}
#endif

#ifdef aarch64
extern void Startup(void);

void BringUpSlaveCPU(U32 CPUID)
{
	WriteIO32(&pReg_ClkPwr->CPURESETMODE, 0x1);
	WriteIO32(&pReg_ClkPwr->CPUPOWERONREQ, (1 << CPUID));
	ClearIO32(&pReg_ClkPwr->CPUPOWERONREQ, (1 << CPUID));
}

void SetVectorLocation(U32 CPUID, CBOOL LowHigh)
{
	U32 regvalue;
	LowHigh = LowHigh; // for AArch32 comfortable
	if (CPUID & 0x4)   // cpu 4, 5, 6, 7
	{
		CPUID &= 0x3;
		regvalue = ReadIO32(&pReg_Tieoff->TIEOFFREG[96]);
		regvalue |= 1 << (4 + CPUID);
		WriteIO32(&pReg_Tieoff->TIEOFFREG[96], regvalue);
		WriteIO32(&pReg_Tieoff->TIEOFFREG[97 + (CPUID << 1)],
			  (U32)(MPTRS)Startup >> 2);	// this image's entry
	} else // cpu 0, 1, 2, 3
	{
		regvalue = ReadIO32(&pReg_Tieoff->TIEOFFREG[79]);
		regvalue |= 1 << (12 + CPUID); // set cpu mode to AArch64
		WriteIO32(&pReg_Tieoff->TIEOFFREG[79], regvalue);
		WriteIO32(&pReg_Tieoff->TIEOFFREG[80 + (CPUID << 1)],
			  (U32)(MPTRS)Startup >> 2);	// this image's entry
	}
}

#if defined(SKIP_ATF)
/* PSCI CPU_ON (psci.c): power the core on into this image's Startup. */
void PowerOnSecondaryCPU(U32 CPUID)
{
	SetVectorLocation(CPUID, CTRUE);
	BringUpSlaveCPU(CPUID);
}
#endif
#endif

#endif // #if (MULTICORE_BRING_UP == 1)

struct NX_SubCPUBringUpInfo {
	volatile U32 JumpAddr;
	volatile U32 CPUID;
	volatile U32 WakeupFlag;
};
void SwitchToEL2(void);
void SetGIC_All(void);
void psciSecondaryEntry(U32 CPUID);
#define CPU_ALIVE_FLAG_ADDR 0xC0010230

#if defined(aarch64) && defined(SKIP_ATF)
static void EnableCCIPort(U32 port)
{
	if ((ReadIO32(&pReg_CCI400->CSI[port].SCR) & 0x3) == 0x3)
		return;
	WriteIO32(&pReg_CCI400->CSI[port].SCR, 0x3); // Snoop & DVM Req
	while (ReadIO32(&pReg_CCI400->STSR) & 0x1)
		;
}

/*
 * Put this core's cluster into CCI-400 coherency, the way BL31 does on
 * cluster power-on: a port may only take snoop/DVM requests while its
 * cluster is powered, so BL1 leaves cpu 4~7's off (secondboot.c
 * initCCI400()). Cpu 0~3's port too, for the OPMODE=aarch32 stage2,
 * whose BL1 left both off; CPU0's cluster is always up, so that's safe.
 */
static void JoinCCI(U32 CPUID)
{
	EnableCCIPort(BUSID_CPUG0);
	if (CPUID & 0x4)
		EnableCCIPort(BUSID_CPUG1);
}
#endif

void SubCPUBoot(U32 CPUID)
{
	register struct NX_SubCPUBringUpInfo *pCPUStartInfo =
	    (struct NX_SubCPUBringUpInfo *)CPU_ALIVE_FLAG_ADDR;

#if defined(aarch64) && defined(SKIP_ATF)
	JoinCCI(CPUID);
#endif

	SetGIC_All();
	WriteIO32(&pReg_GIC400->GICD.ISENABLER[0], 0xFF); // enable sgi all
	WriteIO32(&pReg_ClkPwr->CPUPOWERONREQ,
		  0x00); // clear own wakeup req bit

	//    printf("Hello World. I'm CPU %d!\r\n", CPUID);
	pCPUStartInfo->WakeupFlag = 1;
	DebugPutch('0' + CPUID);
#if defined(aarch64) && defined(SKIP_ATF)
	psciSecondaryEntry(CPUID);	// parks at EL3 until PSCI CPU_ON
#endif
#ifdef aarch64
	SwitchToEL2();
#endif
	printf("CPU%d parked, waiting\r\n", CPUID);
	do {
		register void (*pLaunch)(void);
		__asm__ __volatile__("wfi");

		//        WriteIO32(&pReg_GIC400->GICD.ICPENDR[0], 1<<CPUID);
		//        WriteIO32(&pReg_GIC400->GICC.EOIR,
		//        ReadIO32(&pReg_GIC400->GICC.IAR));
		//        __asm__ __volatile__ ("wfe");
		pLaunch = (void (*)(void))((MPTRS)pCPUStartInfo->JumpAddr);
		if ((MPTRS)pLaunch != (MPTRS)0xFFFFFFFF) {
			if (CPUID == pCPUStartInfo->CPUID) {
				printf("CPU%d woken, jumping to 0x%x\r\n",
				       CPUID, (unsigned)pCPUStartInfo->JumpAddr);
				pLaunch();
			}
		}
	} while (1);
}

//------------------------------------------------------------------------------
CBOOL SubCPUBringUp(U32 CPUID)
{
	register struct NX_SubCPUBringUpInfo *pCPUStartInfo =
	    (struct NX_SubCPUBringUpInfo *)CPU_ALIVE_FLAG_ADDR;
	S32 CPUNumber, retry = 0;
	S32 result = CPUID;

	WriteIO32(&pReg_GIC400->GICC.CTLR, 0x07); // enable cpu interface
	WriteIO32(&pReg_GIC400->GICC.PMR, 0xFF);  // all high priority
	WriteIO32(&pReg_GIC400->GICD.CTLR, 0x03); // distributor enable

#if defined(aarch64) && defined(SKIP_ATF)
	/*
	 * Leave the secondaries off: psci.c powers each one on when the
	 * kernel asks for it (PSCI CPU_ON), like the vendor BL31 does.
	 * Parking them at EL3 instead didn't work on the board - a parked
	 * core never woke from wfi on the CPU_ON SGI, and one spinning
	 * instead of sitting in wfi kept the next core from powering on.
	 */
	(void)pCPUStartInfo;
	(void)retry;
	(void)CPUNumber;
	printf("Sub CPUs stay off until PSCI CPU_ON\r\n");
	printf("CPU%d is Master!\r\n\n", CPUID);
	return result;
#endif

	printf("Wakeup Sub CPU ");

#if (MULTICORE_BRING_UP == 1)

	pCPUStartInfo->JumpAddr =
	    (U32)0xFFFFFFFF; // set cpu jump info to invalid

	for (CPUNumber = 1; CPUNumber < 8;) {
		register volatile U32 delay;

		pCPUStartInfo->WakeupFlag = 0;
		delay = 0x10000;
		SetVectorLocation(CPUNumber, CTRUE); // CTRUE: High
						     // Vector(0xFFFF0000),
						     // CFALSE: Low Vector (0x0)
		BringUpSlaveCPU(CPUNumber);
		DMC_Delay(10000);
		while ((pCPUStartInfo->WakeupFlag == 0) && (--delay))
			;
		if (delay == 0) {
			if (retry > 3) {
				printf("maybe cpu %d is dead. -_-;\r\n",
				       CPUNumber);
				CPUNumber++; // try next cpu bringup
				retry = 0;
				result = CFALSE;
			} else {
				printf("cpu %d is not bringup, retry\r\n",
				       CPUNumber);
				retry++;
			}
		} else {
			retry = 0;
			result++;
			CPUNumber++; // try next cpu bringup
		}
		DMC_Delay(10000);
	}
#endif // #if (MULTICORE_BRING_UP == 1)

	printf("\r\nCPU Wakeup done! WFI is expected.\r\n");
	printf("CPU%d is Master!\r\n\n", CPUID);
	return result;
}
