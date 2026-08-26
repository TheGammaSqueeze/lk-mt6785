/*
 * Secondary-core (Cortex-A55, NON-SECURE PL1) PSCI CPU_ON entry stubs for the
 * multicore experiment. The core is powered by ATF (PSCI CPU_ON) and enters here
 * MMU-off, caches-off, in AArch32 NS SVC. Forced ARM state and 16-byte aligned.
 *
 *  bc_entry_arm  - MMU-off proof of life: magic + an unbounded uncached counter.
 *                  (Used to first prove the core powers on and executes.)
 *  bc_entry_arm2 - cached bring-up: adopt cpu0's LPAE MMU snapshot from the comms
 *                  block, enable MMU + I/D caches (A55 SMP coherency is hardware
 *                  managed - there is NO software SMPEN to set), then heartbeat
 *                  from CACHED memory. This is the real worker entry.
 * Debug/EXPT build only.
 */
#include "bigcore_comms.h"

	.text
	.arm
	.balign 16
	.global bc_entry_arm
	.type   bc_entry_arm, %function
bc_entry_arm:
	movw	r0, #0x0000		@ r0 = 0x54000000 (BC_COMMS_PA)
	movt	r0, #0x5400
	movw	r1, #0x0DE5		@ r1 = 0xB16C0DE5 (BC_MAGIC)
	movt	r1, #0xB16C
	str	r1, [r0, #BC_O_MAGIC]	@ comms.magic = BC_MAGIC
	dsb
	mov	r2, #0
1:	add	r2, r2, #1
	str	r2, [r0, #BC_O_COUNTER]	@ live heartbeat (uncached, MMU off)
	dsb
	b	1b
	.size	bc_entry_arm, . - bc_entry_arm

	.balign 16
	.global bc_entry_arm2
	.type   bc_entry_arm2, %function
bc_entry_arm2:
	movw	r0, #0x0000		@ comms base 0x54000000
	movt	r0, #0x5400
	movw	r1, #0x0DE5		@ magic 0xB16C0DE5
	movt	r1, #0xB16C
	str	r1, [r0, #BC_O_MAGIC]	@ reached-the-stub (uncached, MMU off)
	dsb

	ldr	sp, [r0, #BC_O_STACK_TOP]	@ worker stack top (DRAM Normal-WB)

	@ read cpu0's MMU snapshot (MMU off -> uncached DRAM read; cpu0 cache-cleaned)
	ldr	r4, [r0, #BC_O_TTBR0_LO]
	ldr	r5, [r0, #BC_O_TTBR0_HI]
	ldr	r6, [r0, #BC_O_TTBCR]
	ldr	r7, [r0, #BC_O_MAIR0]
	ldr	r8, [r0, #BC_O_MAIR1]
	ldr	r9, [r0, #BC_O_DACR]
	ldr	r10, [r0, #BC_O_SCTLR]

	@ force M/C/I off while we program the translation registers
	mrc	p15, 0, r1, c1, c0, 0
	bic	r1, r1, #0x1		@ SCTLR.M
	bic	r1, r1, #0x4		@ SCTLR.C
	bic	r1, r1, #0x1000		@ SCTLR.I
	mcr	p15, 0, r1, c1, c0, 0
	isb

	mov	r1, #0
	mcr	p15, 0, r1, c8, c7, 0	@ TLBIALL
	mcr	p15, 0, r1, c7, c5, 0	@ ICIALLU (I-cache invalidate to PoU)
	mcr	p15, 0, r1, c7, c5, 6	@ BPIALL  (branch predictor invalidate)
	dsb
	isb

	mcr	p15, 0, r6, c2, c0, 2	@ TTBCR (EAE selects LPAE)
	mcr	p15, 0, r7, c10, c2, 0	@ MAIR0
	mcr	p15, 0, r8, c10, c2, 1	@ MAIR1
	mcr	p15, 0, r9, c3, c0, 0	@ DACR
	mcrr	p15, 0, r4, r5, c2	@ TTBR0 = {hi:lo}  (64-bit LPAE write)
	isb

	@ enable MMU + I/D caches; keep cpu0's other SCTLR bits (A/TRE/AFE/...) from r10
	orr	r10, r10, #0x1		@ M
	orr	r10, r10, #0x4		@ C
	orr	r10, r10, #0x1000	@ I
	dsb
	mcr	p15, 0, r10, c1, c0, 0
	isb				@ now cached + Inner-Shareable coherent with cpu0

	movw	r1, #0x0DE5		@ cached_ok = magic (CACHED write, separate line)
	movt	r1, #0xB16C
	str	r1, [r0, #BC_O_CACHED_OK]
	dsb

	@ enter the C render worker loop (waits on comms.go, renders its band, sets
	@ comms.done). sp is set, MMU+caches on, so the full C environment is usable.
	bl	bc_worker_entry
	@ should never return; if it ever does, park in an uncached heartbeat so the
	@ OSD still shows the core is alive rather than silently wedging.
	mov	r2, #0
2:	add	r2, r2, #1
	str	r2, [r0, #BC_O_COUNTER]
	dsb
	b	2b
	.size	bc_entry_arm2, . - bc_entry_arm2
