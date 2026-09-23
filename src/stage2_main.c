/*
 * SKIP_ATF, OPMODE=aarch32 only: the AArch64 stage2.
 *
 * AArch32 BL1 does the real bring-up (clock/DDR init, reading
 * u-boot-direct.img into DDR), then resets CPU0 into AArch64 here
 * (secondboot.c LaunchStage2()). This image is linked at
 * STAGE2_AARCH64_ADDR and appended to BL1's own image, so BootROM has
 * already put it in SRAM. It takes the role BL31 has in the ATF chain:
 * EL3 vectors with the PSCI handler (psci.c), secondary cores powered on
 * and parked at EL3 for CPU_ON, and u-boot entered at EL2 non-secure.
 * It stays resident in SRAM for as long as the system runs.
 */
#define __SET_GLOBAL_VARIABLES
#include "sysheader.h"

extern U32 GetCurrentSMode(void);
extern CBOOL SubCPUBringUp(U32 CPUID);
extern void psciInit(U32 bootCpu);
extern void EnterNonSecure(unsigned long entry, unsigned long context);

void BootMain(U32 CPUID)
{
	volatile U32 *handoff = (volatile U32 *)STAGE2_HANDOFF_ADDR;
	U32 entry, t;

	/* UART0 is still set up the way AArch32 BL1 left it */
	DebugSetPort(0);

	printf("\r\nstage2: AArch64 EL%d, CPU%d\r\n", GetCurrentSMode(), CPUID);

	if (handoff[0] != STAGE2_AARCH64_SIGNATURE || handoff[1] == 0) {
		printf("stage2: no handoff from BL1 (0x%08X 0x%08X), halting\r\n",
		       handoff[0], handoff[1]);
		while (1)
			;
	}
	entry = handoff[1];
	handoff[0] = 0;

	psciInit(CPUID);
	SubCPUBringUp(CPUID);

	printf("stage2: entering u-boot at 0x%08X, EL2 non-secure\r\n", entry);
	t = 0x10000000;
	while (!DebugIsUartTxDone() && t--)
		;

	EnterNonSecure(entry, 0);
}
