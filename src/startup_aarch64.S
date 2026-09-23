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
#include "nx_peridot.h"
#include "cfgBootDefine.h"

#include "aarch64_Vectors.inc"

//        .align 9, 0    // 2^9 = 512 bytes sector size
//	.skip 0x200, 0x00 

/*
 * entry point of main function
 */
.global BootMain
.global SubCPUBoot
.global GetCPUID

//;==================================================================
//; Vectors
//;==================================================================
.global Startup
Startup:
        msr     DAIFSet, #(I_Bit|F_Bit|A_Bit)        //; disable interrupt & fast interrupt and Abort

        bl      GetCPUID
        mov     x29, x0

        bl      remap_vectors

        ands    x0, x0, #3
        b.ne    CPUBRINGUP


        b       Reset_Handler
        b       .
.global Sleep
Sleep:
        SMC     12345

BuildInfo:
        .word   0x68180306      //; 24, Chip name - 6818, Build num - v0.3.06

Reset_Handler:
        cmp     x29, xzr
        b.ne    clbss_e
//;==================================================================
//; Clear SRAM
//;==================================================================
        //; Clear area of global data.
        ldr     x1, =__bss_start__                  // this is auto-relocated!
        ldr     x2, =__bss_end__                    // this is auto-relocated!

        mov     x3, xzr                             // prepare zero to clear BSS

clbss_l:
        cmp     x1, x2                              // while not at end of BSS
        b.hs    clbss_e                             // higher or same
        str     x3, [x1], #8                        // clear 64-bit BSS word
        b.lo    clbss_l
clbss_e:
.if 1
//=============================================================================
// Set L2ACTLR
//=============================================================================
        // L2CTLR_EL1
        mrs     x0, S3_1_c11_c0_2                   // Read L2 Control Register
        orr     x0, x0, #(1<<21)                    // [21]ECC, parity enable.
        orr     x0, x0, #(1<<20)                    // [22]Data inline ECC enable.
                                                    //     only applies if ECC is enabled
//        and     x0, x0, #~(1<<5)                    // [5]L2 Data RAM input latency (1 cycle)
        orr     x0, x0, #(1<<5)                     // [5]L2 Data RAM input latency (2 cycle)
//        and     x0, x0, #~(1<<5)                    // [0]L2 Data RAM output latency (2 cycle)
        orr     x0, x0, #(1<<5)                     // [0]L2 Data RAM output latency (3 cycle)
        msr     S3_1_c11_c0_2, x0                   // Write L2 Control Register

        // L2ECTLR_EL1 - do not touch yet
        // L2 internal asynchronous error
        // AXI or Skyros asynchronous error
        // L2 dynamic retention control
//        mrs     x0, S3_1_C11_C0_3                   // Read L2 Extented Control Register
//        and     x0, x0, #~(1<<30)                   // clear internal asynchronous error pending irq
//        and     x0, x0, #~(1<<29)                   // clear AXI asynchronous error irq
//        bic     x0, x0, #(7<<0)                     // L2 dynamic retention disabled.
//        msr     S3_1_C11_C0_3, x0                   // Write L2 Extented Control Register

        // L2ACTLR_EL1
        mrs     x0, s3_1_c15_c0_0
        and     x0, x0, #~(1<<14)                   // Disables UniqueClean evictions with data. This is the reset value for ACE.
        and     x0, x0, #~(1<<3)                    // Enable clean/evict to be pushed out to external. This is the reset value for ACE.
        msr     s3_1_c15_c0_0, x0

        // CPUACTLR_EL1
//        mrs     x0, S3_1_c15_c2_0
//        no touch yet
//        msr     S3_1_c15_c2_0, x0
        mrs     x0, ACTLR_EL3                       // Read ACTLR_ELx into Xt
        orr     x0, x0, #(1<<6)                     // L2ACTLR accessible from lower ELs
        orr     x0, x0, #(1<<5)                     // L2ECTLR accessible from lower ELs
        orr     x0, x0, #(1<<4)                     // L2CTLR accessible from lower ELs
        orr     x0, x0, #(1<<1)                     // CPUECTLR accessible from lower ELs
        orr     x0, x0, #(1<<0)                     // CPUACTLR accessible from lower ELs
        msr     ACTLR_EL3, x0                       // Write Xt to ACTLR_ELx
//        dsb     sy
.endif
//;==================================================================
//; Setup stacks
//;==================================================================
CPUBRINGUP:

        mrs     x0, SCR_EL3
        orr     x0, x0, #(3<<4)                     // RES1
        bic     x0, x0, #(1<<0)                     // 0: secure mode
        orr     x0, x0, #(1<<2)                     // 1: route fiq to EL3
        bic     x0, x0, #(1<<7)                     // 0: SMC is enabled at EL1, EL2, or EL3    1: SMC is undefined at all exception level
        bic     x0, x0, #(1<<8)                     // disable HVC. to be NOP
//;        orr     x0, x0, #(1<<12)                    // trap WFI
        orr     x0, x0, #(1<<10)                    // RW: 0: aarch32, 1:aarch64
        msr     SCR_EL3, x0

        msr     CPTR_EL3, xzr                       //; not traped FP, SIMD

        mrs     x0, S3_1_c15_c2_1
        orr     x0, x0, #(1<<6)                     // [6] SMPEN
        msr     S3_1_c15_c2_1, x0
		isb

//        ldr     x0, =200000000
//        msr     CNTFRQ_EL0, x0

        mrs     x0, CPACR_EL1                       //; printf use fpu, neon register. so for test, exception trap must be disabled.
        orr     x0, x0, #(3<<20)                    //; access fpu is not traped EL0, EL1
        msr     CPACR_EL1, x0

        mrs     x0, HCR_EL2
        orr     x0, x0, #(1<<31)                    //; rw 0:lower levels are all aarch32, 1: EL1 is aarch64
        bic     x0, x0, #(1<<27)                    //; TGE    - el1 exception routed to el2
//;        orr     x0, x0, #(1<<13)                 //; wfi traped
        orr     x0, x0, #(1<<4)                     //; IMO
        msr     HCR_EL2, x0

//;        mrs     x0, CPTR_EL2
        mov     x0, #0x33FF                         //; RES1
//;        bic     x0, x0, #(1<<31)                    //; TCPAC
//;        bic     x0, x0, #(1<<20)                    //; TTA
//;        bic     x0, x0, #(1<<10)                    //; TFP
        msr     CPTR_EL2, x0

        cmp     x29, xzr
        b.ne    0f
.if 1

        mrs     x0, SCTLR_EL3
        orr		x0, x0, #(1<<29 | 1<<28)			//; SBO
        orr		x0, x0, #(1<<23 | 1<<22)			//; SBO
        orr		x0, x0, #(1<<11)					//; SBO
        bic     x0, x0, #(1<<12)                    //; icache disable
        msr     SCTLR_EL3, x0
        ic      ialluis                             //; invalidate icache all
        isb     sy
        mrs     x0, SCTLR_EL3
        orr     x0, x0, #(1<<12)
        msr     SCTLR_EL3, x0                       //; icache enable
.endif

        mov     x0, #0x0830                         //; RES1
        movk    x0, #0x30C5, lsl #16                //; RES1
        msr     sctlr_el2, x0                       //; MMU off, I and C bit off, Align bit off, little endian, execute never
0:
        mov     w0, #0xFF000000
        orr     w0, w0, #0x00FF0000
        add     x0, x0, #INTERNAL_SRAM_SIZE

        mov     w1, #0x400                          // 1KB per CPU: also the EL3 stack for SMCs (psci.c, printf); 16-byte aligned
        sub     w2, w29, #1
        and     w2, w2, #0x7                        // cpu 0: -0x1C00, cpu 1: -0, cpu 2: -0x400, 3: -0x800, ... 7: -0x1800
        mul     w1, w1, w2
        sub     x0, x0, x1

        mov     sp, x0
		msr		sp_el2, x0

        mov     x0, x29
		bl remap_vectors

        mov     x0, x29

        cmp     x0, xzr
        b.ne    1f
        bl      BootMain                            //; save this in register for possible long jump
        b       .
1:
        bl      SubCPUBoot
        b       .

        .ltorg
//;==================================================================
//; PLL Change
//;==================================================================
        .align 6                                    //; below instruction number is 6, 2^6=64bytes

.global __pllchange
__pllchange:                                        //; r0:data r1:pll address r2:delay count
        mov     w3, #0x1000                         //; for icache prefetch
pllchangedelayloop:                                 //; this code will be already within i-cache. no bus transaction will make
        subs    w3, w3, #1                          //; wait for pll change done
        b.ne    notadapt
        str     w0, [x1]                            //; pll change start
        mov     w3, w2                              //; real delay time set
        cmp     w3, wzr
        b.ne    postloop
notadapt:
        cmp     w3, #0x1000
postloop:
        b.ne    pllchangedelayloop
        ret

        .ltorg

.global    remap_vectors
// void remap_vectors(U32 cpuid);
remap_vectors:
        adr     x3, vectors_el3
        msr     VBAR_EL3, x3                        //; reset exception vector
        ret

        .ltorg
//==================================================================
// Exception Handler entry
//
// Full AArch64 exception vector table, replacing the old 3-slot patch
// hack. Every entry saves the full register file, works out which
// exception level's ESR to read from the vector offset, and calls
// sync_c_handler_EL3(exc, esr, regs) in aarch64_exception_handler.c -
// exc is this vector's own byte offset (0x000..0x780), which doubles
// as the exception class (sync/IRQ/FIQ/SError x current/lower EL).
//==================================================================
.Lexc_common:
    stp     x27, x28, [sp, -16]!
    stp     x25, x26, [sp, -16]!
    stp     x23, x24, [sp, -16]!
    stp     x21, x22, [sp, -16]!
    stp     x19, x20, [sp, -16]!
    stp     x17, x18, [sp, -16]!
    stp     x15, x16, [sp, -16]!
    stp     x13, x14, [sp, -16]!
    stp     x11, x12, [sp, -16]!
    stp     x9,  x10, [sp, -16]!
    stp     x7,  x8,  [sp, -16]!
    stp     x5,  x6,  [sp, -16]!
    stp     x3,  x4,  [sp, -16]!
    stp     x1,  x2,  [sp, -16]!
    stp     xzr, x0,  [sp, -16]!
    mrs     x1, CurrentEL
    ubfx    x1, x1, 2, 2
    cmp     x1, 3
    b.eq    3f
    cmp     x1, 2
    b.eq    2f
    mrs     x1, esr_el1
    b       1f
3:  mrs     x1, esr_el3
    b       1f
2:  mrs     x1, esr_el2
1:  mov     x0, x29
    add     x2, sp, 8
    bl      sync_c_handler_EL3
    ldp     xzr, x0,  [sp], 16
    ldp     x1,  x2,  [sp], 16
    ldp     x3,  x4,  [sp], 16
    ldp     x5,  x6,  [sp], 16
    ldp     x7,  x8,  [sp], 16
    ldp     x9,  x10, [sp], 16
    ldp     x11, x12, [sp], 16
    ldp     x13, x14, [sp], 16
    ldp     x15, x16, [sp], 16
    ldp     x17, x18, [sp], 16
    ldp     x19, x20, [sp], 16
    ldp     x21, x22, [sp], 16
    ldp     x23, x24, [sp], 16
    ldp     x25, x26, [sp], 16
    ldp     x27, x28, [sp], 16
    ldp     x29, x30, [sp], 16
    eret

.macro vecentry
    .align  7
1:  stp     x29, x30, [sp, -16]!
    mov     x29, 1b - vectors_el3
    b       .Lexc_common
.endm

    // Own section: VBAR needs 2KB alignment, and inside .text that
    // would push the whole output section - and Startup - off
    // 0xFFFF0200, where the NSIH vector resets the core into.
    .section .vectors, "ax"
    .align 11
.global vectors_el3
vectors_el3:
    vecentry /* 0x000 Current EL Synchronous Thread   (SP_EL0) */
    vecentry /* 0x080 Current EL IRQ Thread           (SP_EL0) */
    vecentry /* 0x100 Current EL FIQ Thread           (SP_EL0) */
    vecentry /* 0x180 Current EL Error Thread         (SP_EL0) */
    vecentry /* 0x200 Current EL Synchronous Handler  (SP_ELx) */
    vecentry /* 0x280 Current EL IRQ Handler          (SP_ELx) */
    vecentry /* 0x300 Current EL FIQ Handler          (SP_ELx) */
    vecentry /* 0x380 Current EL Error Handler        (SP_ELx) */
    vecentry /* 0x400 Lower EL Synchronous, AArch64 */
    vecentry /* 0x480 Lower EL IRQ, AArch64 */
    vecentry /* 0x500 Lower EL FIQ, AArch64 */
    vecentry /* 0x580 Lower EL Error, AArch64 */
    vecentry /* 0x600 Lower EL Synchronous, AArch32 */
    vecentry /* 0x680 Lower EL IRQ, AArch32 */
    vecentry /* 0x700 Lower EL FIQ, AArch32 */
    vecentry /* 0x780 Lower EL Error, AArch32 */

    .text

.global ExceptionTest
.global Startup_EL1
Startup_EL1:
        mov     x0, 0xffffff00
        mov     sp, x0

//        mrs     x0, CPACR_EL1                       //; printf use fpu, neon register. so for test, exception trap must be disabled.
//        orr     x0, x0, #(3<<20)                    //; access fpu is not traped EL0, EL1
//        msr     CPACR_EL1, x0

//        b       ExceptionTest

//;==================================================================
//; End of startup.s
//;==================================================================
