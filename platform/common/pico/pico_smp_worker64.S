/*
 * AArch64 secondary-core worker sources (REFERENCE).
 *
 * The LK builds as AArch32, so its assembler cannot assemble these A64 workers.
 * They are assembled offline and embedded as .word arrays inside pico_smp_asm.S
 * (labels pico_smp_a64_land and pico_smp_coh_land). This file is the readable
 * source for those arrays; keep the two in step. To regenerate the words:
 *
 *   aarch64-linux-gnu-as pico_smp_worker64.S -o w.o
 *   aarch64-linux-gnu-objdump -d w.o | awk '/:\t/{printf "\t.word\t0x%s\n",$2}'
 *
 * A freshly PSCI-powered core enters MMU-off, caches-off, AArch64 EL1. It gets
 * nothing useful in x0 (the PSCI context_id is not delivered on this firmware),
 * so cpu0 hands it parameters through a fixed SRAM page before CPU_ON:
 *   0x0011D7C0 + 8   mark byte (worker progress, readable by cpu0 via oem smp:bc)
 *   0x0011D7C0 + 32  shared-block physical base   (BC[8])
 *   0x0011D7C0 + 36  page-table physical base     (BC[9], coherent worker only)
 * The shared block layout (byte offsets) matches pico_smp.c:
 *   0 PING  64 MAGIC  72 MIDR  76 MPIDR  80 COUNT  84 PONG
 *   96 GO  100 CANARY  104 SEEN  108 DONE  112 STOP
 */

/* ---- pico_smp_a64_land: uncached worker (producer/consumer via DRAM) ------
 * cpu0 cleans before / invalidates after; proves the core runs and exchanges
 * data. Reads 32-bit SRAM slots with ldr w (NOT ldr x: an 8-byte load from a
 * 4-aligned slot alignment-faults on MMU-off Device memory). */
	movz	x7, #0xd7c0
	movk	x7, #0x11, lsl #16		// x7 = 0x0011D7C0 (SRAM param page)
	mov	w6, #0x77
	str	w6, [x7, #8]			// mark: alive
	str	w0, [x7, #16]
	mrs	x1, CurrentEL
	lsr	x1, x1, #2
	and	x1, x1, #3
	str	w1, [x7, #20]			// current EL
	ldr	w9, [x7, #32]			// shared-block base
	dsb	sy
	movz	w1, #0x0c0e
	movk	w1, #0x5ec0, lsl #16
	str	w1, [x9, #64]			// MAGIC = 0x5ec00c0e
	mrs	x1, midr_el1
	str	w1, [x9, #72]
	mrs	x1, mpidr_el1
	str	w1, [x9, #76]
	mov	w2, #0
0:	add	w2, w2, #1
	str	w2, [x9, #80]			// COUNT heartbeat
	ldr	w1, [x9, #0]			// PING from cpu0
	add	w1, w1, #1
	str	w1, [x9, #84]			// PONG = PING+1
	str	w2, [x7, #24]
	dmb	sy
	b	0b

/* ---- pico_smp_coh_land: cached, cache-coherent worker ---------------------
 * Sets up its own page table (Normal-WB Inner-Shareable), enables MMU + I/D
 * caches at EL1, then exchanges with cpu0 using barriers only (no clean /
 * invalidate) - proving DSU snoop coherency. Self-powers-off on the STOP flag.
 * cpu0 builds the level-1 page table (512 x 1GB blocks): block 0 = Device,
 * blocks 1..3 = Normal-WB Inner-Shareable; see pico_smp.c. */
	movz	x7, #0xd7c0
	movk	x7, #0x11, lsl #16
	mov	w6, #0x88
	str	w6, [x7, #8]			// mark: coherent worker reached
	ldr	w9, [x7, #32]			// shared-block base
	ldr	w10, [x7, #36]			// page-table base
	dsb	sy
	movz	x1, #0x00ff			// MAIR: attr0 Normal-WB, attr1 Device
	msr	mair_el1, x1
	movz	x1, #0x3519			// TCR: T0SZ25,IRGN/ORGN WB,SH0 IS,EPD1,4K
	movk	x1, #0x0080, lsl #16
	msr	tcr_el1, x1
	msr	ttbr0_el1, x10
	dsb	sy
	isb
	tlbi	vmalle1
	dsb	sy
	isb
	mrs	x1, sctlr_el1
	orr	x1, x1, #0x1			// M
	orr	x1, x1, #0x4			// C
	orr	x1, x1, #0x1000			// I
	msr	sctlr_el1, x1
	isb
	mov	w6, #0x89
	str	w6, [x7, #8]			// mark: MMU + caches on
	dsb	sy
	mrs	x1, midr_el1
	str	w1, [x9, #72]
	mrs	x1, mpidr_el1
	str	w1, [x9, #76]
	mov	w3, #0
1:	ldr	w1, [x9, #112]			// STOP
	cbnz	w1, 3f
	add	w3, w3, #1
	str	w3, [x9, #80]			// COUNT heartbeat (coherent)
	ldr	w1, [x9, #96]			// GO
	cbz	w1, 2f
	ldr	w2, [x9, #100]			// CANARY (cpu0's cached write, no clean)
	str	w2, [x9, #104]			// SEEN
	dmb	sy
	mov	w2, #1
	str	w2, [x9, #108]			// DONE
2:	dmb	sy
	b	1b
3:	mov	w6, #0x8f
	str	w6, [x7, #8]			// mark: stopping
	dsb	sy
	movz	x0, #0x0002
	movk	x0, #0x8400, lsl #16		// PSCI_CPU_OFF = 0x84000002
	smc	#0
	b	3b
