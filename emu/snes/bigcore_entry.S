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

	@ Invalidate the L1 D-cache by set/way BEFORE enabling it. A freshly powered
	@ core comes up with UNDEFINED D-cache contents; if we turn the cache on
	@ without invalidating, those stale valid lines are returned by loads and
	@ SHADOW the coherent data (the worker read the menu pointer from the comms
	@ block as a fixed garbage value every boot -> data abort). Clobbers r1-r9;
	@ r0 (comms base) is preserved.
	mov	r1, #0
	mcr	p15, 2, r1, c0, c0, 0	@ CSSELR = 0 -> select L1 data cache
	isb
	mrc	p15, 1, r2, c0, c0, 0	@ CCSIDR
	and	r3, r2, #7
	add	r3, r3, #4		@ r3 = Log2(line bytes)
	ubfx	r4, r2, #3, #10		@ r4 = ways - 1
	ubfx	r5, r2, #13, #15	@ r5 = sets - 1
	clz	r6, r4			@ r6 = way-field shift
1:	mov	r7, r4			@ way counter
2:	lsl	r8, r7, r6
	lsl	r9, r5, r3
	orr	r8, r8, r9		@ set/way argument
	mcr	p15, 0, r8, c7, c6, 2	@ DCISW: invalidate line by set/way
	subs	r7, r7, #1
	bge	2b
	subs	r5, r5, #1
	bge	1b
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

	@ Enable VFP/NEON access on this core: snes_menu_render is full of float + NEON,
	@ and a freshly-powered core has FP disabled (cpu0 enabled it at boot but this
	@ core did not) - the first FP op would trap to Undefined and wedge the worker.
	mrc	p15, 0, r1, c1, c0, 2	@ CPACR
	orr	r1, r1, #0x00f00000	@ cp10 + cp11 = full access (bits 20..23)
	bic	r1, r1, #0x80000000	@ clear ASEDIS so Advanced SIMD (NEON) is enabled
	mcr	p15, 0, r1, c1, c0, 2
	isb
	mov	r1, #0x40000000		@ FPEXC.EN (bit30)
	vmsr	fpexc, r1
	str	r1, [r0, #BC_O_W_FPEXC]	@ record the FPEXC we set (diag)
	isb

	@ Point this core's exception vectors at our own handler so a fault in the C
	@ render is CAPTURED (address + reason -> comms) instead of jumping to a
	@ garbage reset vector and wedging silently. (VBAR, 32-byte aligned table.)
	ldr	r1, =bc_vectors
	mcr	p15, 0, r1, c12, c0, 0	@ VBAR
	isb

	@ diagnostic: translate the comms VA through the worker's own MMU and record
	@ PAR (memory attributes + shareability) so we can see if this core's mapping
	@ is actually Normal Inner-Shareable cacheable (else no cross-core coherency).
	movw	r1, #0x0000
	movt	r1, #0x5400		@ VA = 0x54000000 (comms)
	mcr	p15, 0, r1, c7, c8, 0	@ ATS1CPR (stage-1 current PL1 read)
	isb
	mrrc	p15, 0, r2, r3, c7	@ PAR (LPAE 64-bit) -> {r3:r2}
	str	r2, [r0, #BC_O_PAR_LO]
	str	r3, [r0, #BC_O_PAR_HI]

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

	@ ---- worker exception vectors + fault capture ----
	@ On a fault the worker records {type, FAR, FSR, PC} into the comms block and
	@ parks, so cpu0 can read exactly where/why the render died (instead of the
	@ core silently wedging on a garbage reset vector). r0/r1 are clobbered - we
	@ never return.
	.balign 32
	.global bc_vectors
bc_vectors:
	b	.		@ 0x00 reset
	b	bc_undef	@ 0x04 undefined instruction
	b	.		@ 0x08 svc
	b	bc_pabt		@ 0x0C prefetch abort
	b	bc_dabt		@ 0x10 data abort
	b	.		@ 0x14 (hyp)
	b	.		@ 0x18 irq
	b	.		@ 0x1C fiq

bc_undef:
	movw	r0, #0x0000
	movt	r0, #0x5400
	mov	r1, #1
	str	r1, [r0, #BC_O_FAULT_TYPE]
	mrs	r1, spsr
	str	r1, [r0, #BC_O_FAULT_SPSR]
	sub	r1, lr, #4		@ ARM undef: lr = faulting PC + 4
	str	r1, [r0, #BC_O_FAULT_PC]
	dsb
1:	wfe
	b	1b

bc_pabt:
	movw	r0, #0x0000
	movt	r0, #0x5400
	mov	r1, #2
	str	r1, [r0, #BC_O_FAULT_TYPE]
	mrs	r1, spsr
	str	r1, [r0, #BC_O_FAULT_SPSR]
	mrc	p15, 0, r1, c6, c0, 2	@ IFAR
	str	r1, [r0, #BC_O_FAULT_FAR]
	mrc	p15, 0, r1, c5, c0, 1	@ IFSR
	str	r1, [r0, #BC_O_FAULT_FSR]
	sub	r1, lr, #4		@ prefetch abort: lr = faulting PC + 4
	str	r1, [r0, #BC_O_FAULT_PC]
	dsb
1:	wfe
	b	1b

bc_dabt:
	movw	r0, #0x0000
	movt	r0, #0x5400
	mov	r1, #3
	str	r1, [r0, #BC_O_FAULT_TYPE]
	mrs	r1, spsr
	str	r1, [r0, #BC_O_FAULT_SPSR]
	mrc	p15, 0, r1, c6, c0, 0	@ DFAR (faulting data address)
	str	r1, [r0, #BC_O_FAULT_FAR]
	mrc	p15, 0, r1, c5, c0, 0	@ DFSR
	str	r1, [r0, #BC_O_FAULT_FSR]
	sub	r1, lr, #8		@ data abort: lr = faulting PC + 8
	str	r1, [r0, #BC_O_FAULT_PC]
	dsb
1:	wfe
	b	1b
