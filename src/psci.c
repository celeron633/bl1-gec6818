/*
 * Minimal PSCI (Power State Coordination Interface) implementation for BL1,
 * used only when SKIP_ATF is set: with no BL31 ever loaded, the kernel's
 * `psci { method = "smc"; }` calls have nowhere to trap to except back into
 * the resident AArch64 EL3 code - the whole BL1 for OPMODE=aarch64, the
 * stage2 stub (stage2_main.c) for OPMODE=aarch32.
 *
 * Ported/adapted from rafaello7/bl1-nanopi-m3 (same Nexell BL1 lineage,
 * S5P6818-family board), which implements this same trick for NanoPi M3.
 * https://github.com/rafaello7/bl1-nanopi-m3
 *
 * "Off" CPUs never actually lose power: they sit in wfi at EL3 (SubCPUBoot()
 * -> psciSecondaryEntry() at first boot, cpuOff() after a CPU_OFF) until
 * CPU_ON fills in their slot below and kicks them with an SGI.
 */
#include "sysheader.h"

extern U32 GetCPUID(void);
extern void PrepareNonSecureEntry(unsigned long entry);
extern void EnterNonSecure(unsigned long entry, unsigned long context);

#define PSCI_NUM_CPUS		8
#define PSCI_WAKE_SGI		1

enum PsciErrorCodes {
	PSCI_SUCCESS            =  0,
	PSCI_NOT_SUPPORTED      = -1,
	PSCI_INVALID_PARAMETERS = -2,
	PSCI_DENIED             = -3,
	PSCI_ALREADY_ON         = -4,
	PSCI_ON_PENDING         = -5,
	PSCI_INTERNAL_FAILURE   = -6,
	PSCI_NOT_PRESENT        = -7,
	PSCI_DISABLED           = -8,
	PSCI_INVALID_ADDRESS    = -9
};

/* AFFINITY_INFO return values, also used as the per-CPU state */
enum PsciCpuState {
	PSCI_STATE_ON         = 0,
	PSCI_STATE_OFF        = 1,
	PSCI_STATE_ON_PENDING = 2,
};

#define PSCI_VERSION		0x84000000
#define PSCI_CPU_SUSPEND	0x84000001
#define PSCI_CPU_SUSPEND64	0xC4000001
#define PSCI_CPU_OFF		0x84000002
#define PSCI_CPU_ON		0x84000003
#define PSCI_CPU_ON64		0xC4000003
#define PSCI_AFFINITY_INFO	0x84000004
#define PSCI_AFFINITY_INFO64	0xC4000004
#define PSCI_MIGRATE_INFO_TYPE	0x84000006
#define PSCI_SYSTEM_OFF		0x84000008
#define PSCI_SYSTEM_RESET	0x84000009
#define PSCI_FEATURES		0x8400000A

/*
 * MMU is off at EL3, so these are Device accesses - visible to the other
 * cores without any cache maintenance. Cleared with the rest of .bss by
 * CPU0 before any secondary is powered on.
 */
static struct {
	volatile U32 state;
	volatile unsigned long entry;
	volatile unsigned long context;
} cpus[PSCI_NUM_CPUS];

void __attribute__((noreturn)) boardReset(void)
{
	printf("\r\nBoard reset.\r\n");
	WriteIO32(&pReg_Alive->ALIVEPWRGATEREG, 0); // close alive gate
	SetIO32(&pReg_ClkPwr->PWRCONT, 1 << 3);     // Software reset enable
	SetIO32(&pReg_ClkPwr->PWRMODE, 1 << 12);    // Software reset
	while (1)
		;
}

/* Called on the boot CPU before any secondary is powered on. */
void psciInit(U32 bootCpu)
{
	U32 i;

	for (i = 0; i < PSCI_NUM_CPUS; i++)
		cpus[i].state = (i == bootCpu) ? PSCI_STATE_ON : PSCI_STATE_OFF;
}

/* MPIDR (Aff1 = cluster, Aff0 = core) -> GetCPUID()-style index 0..7 */
static int mpidrToCpu(unsigned long mpidr)
{
	if ((mpidr & 0xFF00FF0000UL) || (mpidr & 0xFF) > 3)	// Aff3/Aff2, Aff0
		return -1;
	return (mpidr & 3) | ((mpidr & 0xFF00) ? 4 : 0);
}

/*
 * Sleep at EL3 until CPU_ON marks this CPU ON_PENDING. IRQs aren't routed
 * to EL3 (SCR_EL3.IRQ=0) and are masked anyway, so the wake-up SGI is
 * never taken here - it only ends the wfi, and is then dropped so the
 * kernel doesn't see a stray IPI once it enables interrupts.
 */
static void waitForCpuOn(U32 cpu)
{
	/* the kernel may have shut this CPU interface down before CPU_OFF */
	SetIO32(&pReg_GIC400->GICC.CTLR, 1 << 1);	// Group 1 enable
	WriteIO32(&pReg_GIC400->GICD.ISENABLER[0], 1 << PSCI_WAKE_SGI);

	while (cpus[cpu].state != PSCI_STATE_ON_PENDING)
		__asm__ __volatile__("dsb sy\n\twfi" ::: "memory");

	WriteIO32(&pReg_GIC400->GICD.CPENDSGIR[PSCI_WAKE_SGI / 4],
		  0xFF << ((PSCI_WAKE_SGI % 4) * 8));
	cpus[cpu].state = PSCI_STATE_ON;
	__asm__ __volatile__("dsb sy" ::: "memory");
}

/* SubCPUBoot() tail on a freshly powered secondary. Never returns. */
void psciSecondaryEntry(U32 cpu)
{
	waitForCpuOn(cpu);
	EnterNonSecure(cpus[cpu].entry, cpus[cpu].context);
}

static long cpuOn(unsigned long targetCpu, unsigned long entryPoint,
		  unsigned long contextId)
{
	int cpu = mpidrToCpu(targetCpu);

	if (cpu < 0)
		return PSCI_INVALID_PARAMETERS;
	if (cpus[cpu].state == PSCI_STATE_ON)
		return PSCI_ALREADY_ON;
	if (cpus[cpu].state == PSCI_STATE_ON_PENDING)
		return PSCI_ON_PENDING;

	cpus[cpu].entry = entryPoint;
	cpus[cpu].context = contextId;
	__asm__ __volatile__("dsb sy" ::: "memory");
	cpus[cpu].state = PSCI_STATE_ON_PENDING;
	__asm__ __volatile__("dsb sy" ::: "memory");

	// NSATT=1: SGIs are Group 1 (SetGIC_All), which is all the target's
	// CPU interface - possibly left configured by the kernel - forwards
	WriteIO32(&pReg_GIC400->GICD.SGIR,
		  1 << (cpu + 16) | 1 << 15 | PSCI_WAKE_SGI);
	return PSCI_SUCCESS;
}

/*
 * Park the calling CPU until another CPU_ON, then come back out of this
 * very SMC as if freshly powered on: at entry, EL2, x0 = context_id.
 * Returning (rather than eret-ing from in here) lets the exception
 * handler unwind its frame, so repeated hotplug doesn't leak EL3 stack.
 */
static void cpuOff(unsigned long *regs)
{
	U32 cpu = GetCPUID();

	cpus[cpu].state = PSCI_STATE_OFF;
	__asm__ __volatile__("dsb sy" ::: "memory");

	waitForCpuOn(cpu);
	PrepareNonSecureEntry(cpus[cpu].entry);
	regs[0] = cpus[cpu].context;
}

static long affinityInfo(unsigned long targetAffinity, unsigned long lowestLevel)
{
	int cpu = mpidrToCpu(targetAffinity);

	if (lowestLevel != 0 || cpu < 0)
		return PSCI_INVALID_PARAMETERS;
	return cpus[cpu].state;
}

/*
 * The only cpuidle state this platform's device tree advertises is
 * `arm,psci-suspend-param = <0x0000000>` (see s5p6818.dtsi CPU_SLEEP) -
 * StateType bit clear, i.e. a "standby" state: no context is lost, so
 * just wfi and return to right after the smc. A pending interrupt ends
 * the wfi even though it's masked/not routed to EL3; the kernel takes
 * it once we eret. Powerdown states would need a CPU_ON-style resume,
 * refuse those.
 */
static long cpuSuspend(unsigned long powerState)
{
	if (powerState & (1 << 16))	// StateType: powerdown
		return PSCI_INVALID_PARAMETERS;

	__asm__ __volatile__("dsb sy\n\twfi" ::: "memory");
	return PSCI_SUCCESS;
}

static long psciFeatures(unsigned long function)
{
	switch (function) {
	case PSCI_CPU_SUSPEND:
	case PSCI_CPU_SUSPEND64:
		return 0;	// original power_state format, no OS-initiated mode
	case PSCI_VERSION:
	case PSCI_CPU_OFF:
	case PSCI_CPU_ON:
	case PSCI_CPU_ON64:
	case PSCI_AFFINITY_INFO:
	case PSCI_AFFINITY_INFO64:
	case PSCI_MIGRATE_INFO_TYPE:
	case PSCI_SYSTEM_OFF:
	case PSCI_SYSTEM_RESET:
	case PSCI_FEATURES:
		return PSCI_SUCCESS;
	default:
		return PSCI_NOT_SUPPORTED;
	}
}

void psciHandler(unsigned long *regs)
{
	unsigned long function = regs[0] & 0xFFFFFFFF;
	long result;

	/* CPU_SUSPEND comes from cpuidle and AFFINITY_INFO is polled - log
	 * those and the UART becomes the bottleneck of the whole system */
	if (function != PSCI_CPU_SUSPEND && function != PSCI_CPU_SUSPEND64 &&
	    function != PSCI_AFFINITY_INFO && function != PSCI_AFFINITY_INFO64)
		printf("psci: CPU%d fn=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx\r\n",
		       GetCPUID(), function, regs[1], regs[2], regs[3]);

	switch (function) {
	case PSCI_VERSION:
		result = 0x10000;	// 1.0
		break;
	case PSCI_CPU_SUSPEND:
	case PSCI_CPU_SUSPEND64:
		result = cpuSuspend(regs[1]);
		break;
	case PSCI_CPU_OFF:
		cpuOff(regs);	// comes back as a CPU_ON'd CPU, regs[0] set
		return;
	case PSCI_CPU_ON:
	case PSCI_CPU_ON64:
		result = cpuOn(regs[1], regs[2], regs[3]);
		break;
	case PSCI_AFFINITY_INFO:
	case PSCI_AFFINITY_INFO64:
		result = affinityInfo(regs[1], regs[2]);
		break;
	case PSCI_MIGRATE_INFO_TYPE:
		result = 2;	// no Trusted OS, nothing to migrate
		break;
	case PSCI_SYSTEM_OFF:
		// no PMIC control here - park forever instead
		printf("psci: system off\r\n");
		while (1)
			__asm__ __volatile__("wfi");
	case PSCI_SYSTEM_RESET:
		boardReset();
	case PSCI_FEATURES:
		result = psciFeatures(regs[1] & 0xFFFFFFFF);
		break;
	default:
		/* SMCCC: unknown function IDs (incl. SMCCC_VERSION, i.e.
		 * "SMCCC 1.0") return NOT_SUPPORTED, never reset */
		result = PSCI_NOT_SUPPORTED;
		break;
	}
	regs[0] = (unsigned long)result;
}
