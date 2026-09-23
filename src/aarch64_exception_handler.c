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
 *      Module          : Exception Handler
 *      File            : aarch32_exception_handler.c
 *      Description     :
 *      Author          : Firware Team
 *      History         : 2015.06.10 Hans
 */
#include "sysheader.h"

U32 GetCurrentSMode(void);
U32 GetCPUID(void);
#if defined(SKIP_ATF)
void psciHandler(unsigned long *regs);
void __attribute__((noreturn)) boardReset(void);
#endif

/*
 * exc: byte offset of the vector table entry that was taken (see
 *      vectors_el3 in startup_aarch64.S) - 0x400 means "synchronous
 *      exception from a lower EL, AArch64".
 * esr: ESR_ELx for whichever EL we trapped from (EL3 itself if exc
 *      indicates a current-EL exception, read by the caller).
 * regs: pointer to the trapping context's saved x0..x30 (regs[0..30]);
 *       for an SMC this doubles as the PSCI argument/return registers.
 */
void sync_c_handler_EL3(unsigned exc, unsigned esr, unsigned long *regs)
{
	unsigned excClass = esr >> 26, excSyndrome = esr & 0x1ffffff;

#if defined(SKIP_ATF)
	switch (exc) {
	case 0x280:     // Current-EL IRQ, SP_ELx: just ack it (parked CPUs never take one - see psci.c)
		WriteIO32(&pReg_GIC400->GICC.EOIR, ReadIO32(&pReg_GIC400->GICC.IAR));
		return;
	case 0x400:     // synchronous, lower EL, AArch64
		if (excClass == 0x17) { // SMC instruction execution
			if (excSyndrome == 0) {
				psciHandler(regs);
				return;
			}
		}
		break;
	default:
		break;
	}
	printf("\r\n>> PANIC! exception 0x%x on CPU%d level %d, class=0x%x syndrome=0x%x\r\n",
	       exc, GetCPUID(), GetCurrentSMode(), excClass, excSyndrome);
	boardReset();
#else
	printf("\r\nunhandled exception 0x%x on CPU%d level %d, class=0x%x syndrome=0x%x\r\n",
	       exc, GetCPUID(), GetCurrentSMode(), excClass, excSyndrome);
	while (1);
#endif
}
