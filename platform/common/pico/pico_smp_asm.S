/*
 * GammaOS Pico: a second CPU core in the LK (see pico_smp.c).
 *
 * pico_smp_arm_call   asks the firmware above us for its "start the kernel" service with an entry
 *                     of our own, and comes back to the caller. That service is the one place the
 *                     firmware readies the CPU power controller, without which a core power on
 *                     request never ends. It starts a 32 bit entry in Hyp mode with a fresh
 *                     processor state, so everything the LK's own mode needs is saved here first
 *                     and put back by the entry code before it drops to the LK's mode again.
 * pico_smp_sec_entry  where a core started later begins: MMU and caches off, Hyp or SVC mode.
 *
 * Both entries run with the MMU off, so they read only memory the caller cleaned out of the cache
 * and write only memory the caller invalidates before it reads it.
 */
#include <asm.h>

/* The parameter and mark page the workers use: the tail of the SRAM the RAM console sits in.
 * Konkr Pocket Advance 0x0011D7C0, Konkr Pocket Block 0x0010EF00 (pico_smp.c: SMP_SRAM). The
 * workers are stored as words, so their first two instructions (x7 or r7 = the page) are here. */
#if defined(KONKR_BLOCK)
#define SMP_A64_MOVZ_X7 0xd29de007	/* movz x7, #0xef00 */
#define SMP_A64_MOVK_X7 0xf2a00207	/* movk x7, #0x10, lsl #16 */
#define SMP_A64_MOVZ_X20 0xd29de014
#define SMP_A64_MOVK_X20 0xf2a00214
#define SMP_A32_MOVW_R7 0xe30e7f00	/* movw r7, #0xef00 */
#define SMP_A32_MOVT_R7 0xe3407010	/* movt r7, #0x10 */
#else
#define SMP_A64_MOVZ_X7 0xd29af807	/* movz x7, #0xd7c0 */
#define SMP_A64_MOVK_X7 0xf2a00227	/* movk x7, #0x11, lsl #16 */
#define SMP_A64_MOVZ_X20 0xd29af814
#define SMP_A64_MOVK_X20 0xf2a00234
#define SMP_A32_MOVW_R7 0xe30d77c0	/* movw r7, #0xd7c0 */
#define SMP_A32_MOVT_R7 0xe3407011	/* movt r7, #0x11 */
#endif

	.arch	armv7-a
	.arch_extension virt
	.arch_extension sec
	.fpu	neon
	.text
	.arm

/* word offsets in the saved state (pico_smp.c: struct smp_save) */
#define W(n) ((n) * 4)

.macro	EL2_OPEN
	mov	r0, #0
	mcr	p15, 4, r0, c1, c1, 0		@ HCR: nothing trapped, no second stage
	mcr	p15, 4, r0, c1, c1, 3		@ HSTR
	movw	r0, #0x33ff
	mcr	p15, 4, r0, c1, c1, 2		@ HCPTR: FP and SIMD not trapped
	mov	r0, #3
	mcr	p15, 4, r0, c14, c1, 0		@ CNTHCTL: the physical counter and timer are PL1's
	mov	r0, #0
	mov	r1, #0
	mcrr	p15, 4, r0, r1, c14		@ CNTVOFF
	mrc	p15, 0, r0, c0, c0, 0
	mcr	p15, 4, r0, c0, c0, 0		@ VPIDR = MIDR
	mrc	p15, 0, r0, c0, c0, 5
	mcr	p15, 4, r0, c0, c0, 5		@ VMPIDR = MPIDR
	isb
.endm

/* a mark in the page that outlives a watchdog restart (uncached when the MMU is off) */
.macro	MARK n, slot=0
	.word	SMP_A32_MOVW_R7
	.word	SMP_A32_MOVT_R7
	mov	r6, #\n
	str	r6, [r7, #(\slot * 4)]
.endm

/* unsigned pico_smp_arm_call(unsigned *save, unsigned *diag, unsigned fid)
 * returns 0 when it came back through the Hyp entry, 1 when the call simply returned
 * (diag[8] then holds what it returned). Interrupts must be off. */
FUNCTION(pico_smp_arm_call)
	push	{r4-r11, lr}
	mov	r12, r0
	mov	r11, r1
	mov	r10, r2
	mrs	r0, cpsr
	str	r0, [r12, #W(0)]
	str	sp, [r12, #W(1)]
	mrs	r0, spsr
	str	r0, [r12, #W(2)]
	mrs	r0, SP_irq
	str	r0, [r12, #W(3)]
	mrs	r0, LR_irq
	str	r0, [r12, #W(4)]
	mrs	r0, SPSR_irq
	str	r0, [r12, #W(5)]
	mrs	r0, SP_abt
	str	r0, [r12, #W(6)]
	mrs	r0, LR_abt
	str	r0, [r12, #W(7)]
	mrs	r0, SPSR_abt
	str	r0, [r12, #W(8)]
	mrs	r0, SP_und
	str	r0, [r12, #W(9)]
	mrs	r0, LR_und
	str	r0, [r12, #W(10)]
	mrs	r0, SPSR_und
	str	r0, [r12, #W(11)]
	mrs	r0, SP_fiq
	str	r0, [r12, #W(12)]
	mrs	r0, LR_fiq
	str	r0, [r12, #W(13)]
	mrs	r0, SPSR_fiq
	str	r0, [r12, #W(14)]
	mrc	p15, 0, r0, c1, c0, 0		@ SCTLR
	str	r0, [r12, #W(15)]
	mrc	p15, 0, r0, c1, c0, 1		@ ACTLR
	str	r0, [r12, #W(16)]
	mrc	p15, 0, r0, c1, c0, 2		@ CPACR
	str	r0, [r12, #W(17)]
	mrrc	p15, 0, r0, r1, c2		@ TTBR0
	str	r0, [r12, #W(18)]
	str	r1, [r12, #W(19)]
	mrrc	p15, 1, r0, r1, c2		@ TTBR1
	str	r0, [r12, #W(20)]
	str	r1, [r12, #W(21)]
	mrc	p15, 0, r0, c2, c0, 2		@ TTBCR
	str	r0, [r12, #W(22)]
	mrc	p15, 0, r0, c3, c0, 0		@ DACR
	str	r0, [r12, #W(23)]
	mrc	p15, 0, r0, c10, c2, 0		@ MAIR0
	str	r0, [r12, #W(24)]
	mrc	p15, 0, r0, c10, c2, 1		@ MAIR1
	str	r0, [r12, #W(25)]
	mrc	p15, 0, r0, c12, c0, 0		@ VBAR
	str	r0, [r12, #W(26)]
	mrc	p15, 0, r0, c13, c0, 1		@ CONTEXTIDR
	str	r0, [r12, #W(27)]
	mrc	p15, 0, r0, c13, c0, 2		@ TPIDRURW
	str	r0, [r12, #W(28)]
	mrc	p15, 0, r0, c13, c0, 3		@ TPIDRURO
	str	r0, [r12, #W(29)]
	mrc	p15, 0, r0, c13, c0, 4		@ TPIDRPRW
	str	r0, [r12, #W(30)]
	mrc	p15, 2, r0, c0, c0, 0		@ CSSELR
	str	r0, [r12, #W(31)]
	mrc	p15, 0, r0, c14, c1, 0		@ CNTKCTL
	str	r0, [r12, #W(32)]
	vmrs	r0, fpexc
	str	r0, [r12, #W(33)]
	mrc	p15, 0, r0, c14, c2, 1		@ CNTP_CTL
	str	r0, [r12, #W(34)]
	mrrc	p15, 2, r0, r1, c14		@ CNTP_CVAL
	str	r0, [r12, #W(35)]
	str	r1, [r12, #W(36)]
	mrc	p15, 0, r0, c14, c3, 1		@ CNTV_CTL
	str	r0, [r12, #W(37)]
	mrrc	p15, 3, r0, r1, c14		@ CNTV_CVAL
	str	r0, [r12, #W(38)]
	str	r1, [r12, #W(39)]
	mrc	p15, 0, r0, c5, c0, 0		@ DFSR
	str	r0, [r12, #W(40)]
	mrc	p15, 0, r0, c5, c0, 1		@ IFSR
	str	r0, [r12, #W(41)]
	adr	r0, 9f
	str	r0, [r12, #W(42)]		@ where SVC mode goes on
	str	r11, [r12, #W(43)]		@ the diag block
	@ the saved state out of the cache: the Hyp entry reads it with the MMU off
	mov	r0, r12
	add	r1, r12, #256
1:	mcr	p15, 0, r0, c7, c10, 1		@ DCCMVAC
	add	r0, r0, #64
	cmp	r0, r1
	blo	1b
	dsb
	isb
	MARK	0x50			@ reached the firmware call
	mov	r0, r10				@ the service
	ldr	r1, =pico_smp_hyp_back		@ its "kernel"
	mov	r4, #0				@ 0 = a 32 bit entry
	cmp	r3, #0
	ldrne	r1, =pico_smp_a64_probe
	movne	r4, #1
	mov	r2, r12				@ handed on (not relied on)
	movw	r3, #0x5031
	movt	r3, #0x534d
	mov	r5, #0
	mov	r6, #0
	mov	r7, #0
	smc	#0
	@ only reached when the call returned to us
	MARK	0x51
	str	r0, [r11, #W(8)]
	mov	r0, #1
	pop	{r4-r11, pc}
	.ltorg
9:	@ back in SVC mode, on the LK's stack, MMU and caches as before
	MARK	7, 2
	mcr	p15, 0, r7, c7, c10, 1		@ DCCMVAC
	mov	r1, #0
	mcr	p15, 0, r1, c8, c7, 0		@ TLBIALL
	mcr	p15, 0, r1, c7, c5, 0		@ ICIALLU
	mcr	p15, 0, r1, c7, c5, 6		@ BPIALL
	dsb
	isb
	mov	r0, #0
	pop	{r4-r11, pc}

	.balign	64
	.global	pico_smp_hyp_back
pico_smp_hyp_back:
	@ Hyp mode, MMU off; r1, r2 are what the firmware handed on
	mov	r9, r0
	mov	r10, r1
	mov	r11, r2
	MARK	2
	mrs	r0, cpsr
	str	r0, [r7, #4]
	ldr	r12, 8f				@ the saved state
	ldr	r8, [r12, #W(43)]		@ the diag block
	mrs	r0, cpsr
	str	r0, [r8, #W(0)]
	str	r9, [r8, #W(1)]
	str	r10, [r8, #W(2)]
	str	r11, [r8, #W(3)]
	mrc	p15, 4, r0, c1, c1, 0		@ HCR as found
	str	r0, [r8, #W(4)]
	mrc	p15, 4, r0, c1, c0, 0		@ HSCTLR as found
	str	r0, [r8, #W(5)]
	mrc	p15, 0, r0, c1, c0, 0		@ SCTLR as found
	str	r0, [r8, #W(6)]
	mrc	p15, 4, r0, c1, c1, 2		@ HCPTR as found
	str	r0, [r8, #W(7)]
	MARK	3
	EL2_OPEN
	MARK	4
	ldr	r0, [r12, #W(17)]
	mcr	p15, 0, r0, c1, c0, 2		@ CPACR
	isb
	ldr	r0, [r12, #W(33)]
	vmsr	fpexc, r0
	ldr	r0, [r12, #W(22)]
	mcr	p15, 0, r0, c2, c0, 2		@ TTBCR
	isb
	ldr	r0, [r12, #W(18)]
	ldr	r1, [r12, #W(19)]
	mcrr	p15, 0, r0, r1, c2		@ TTBR0
	ldr	r0, [r12, #W(20)]
	ldr	r1, [r12, #W(21)]
	mcrr	p15, 1, r0, r1, c2		@ TTBR1
	ldr	r0, [r12, #W(23)]
	mcr	p15, 0, r0, c3, c0, 0		@ DACR
	ldr	r0, [r12, #W(24)]
	mcr	p15, 0, r0, c10, c2, 0		@ MAIR0
	ldr	r0, [r12, #W(25)]
	mcr	p15, 0, r0, c10, c2, 1		@ MAIR1
	ldr	r0, [r12, #W(26)]
	mcr	p15, 0, r0, c12, c0, 0		@ VBAR
	ldr	r0, [r12, #W(27)]
	mcr	p15, 0, r0, c13, c0, 1		@ CONTEXTIDR
	ldr	r0, [r12, #W(28)]
	mcr	p15, 0, r0, c13, c0, 2		@ TPIDRURW
	ldr	r0, [r12, #W(29)]
	mcr	p15, 0, r0, c13, c0, 3		@ TPIDRURO
	ldr	r0, [r12, #W(30)]
	mcr	p15, 0, r0, c13, c0, 4		@ TPIDRPRW
	ldr	r0, [r12, #W(31)]
	mcr	p15, 2, r0, c0, c0, 0		@ CSSELR
	ldr	r0, [r12, #W(32)]
	mcr	p15, 0, r0, c14, c1, 0		@ CNTKCTL
	ldr	r0, [r12, #W(35)]
	ldr	r1, [r12, #W(36)]
	mcrr	p15, 2, r0, r1, c14		@ CNTP_CVAL
	ldr	r0, [r12, #W(34)]
	mcr	p15, 0, r0, c14, c2, 1		@ CNTP_CTL
	ldr	r0, [r12, #W(38)]
	ldr	r1, [r12, #W(39)]
	mcrr	p15, 3, r0, r1, c14		@ CNTV_CVAL
	ldr	r0, [r12, #W(37)]
	mcr	p15, 0, r0, c14, c3, 1		@ CNTV_CTL
	ldr	r0, [r12, #W(40)]
	mcr	p15, 0, r0, c5, c0, 0		@ DFSR
	ldr	r0, [r12, #W(41)]
	mcr	p15, 0, r0, c5, c0, 1		@ IFSR
	ldr	r0, [r12, #W(16)]
	mcr	p15, 0, r0, c1, c0, 1		@ ACTLR
	ldr	r0, [r12, #W(15)]
	mcr	p15, 0, r0, c1, c0, 0		@ SCTLR: the LK's MMU and caches, for its own mode
	isb
	ldr	r0, [r12, #W(1)]
	msr	SP_svc, r0
	ldr	r0, [r12, #W(2)]
	msr	SPSR_svc, r0
	ldr	r0, [r12, #W(3)]
	msr	SP_irq, r0
	ldr	r0, [r12, #W(4)]
	msr	LR_irq, r0
	ldr	r0, [r12, #W(5)]
	msr	SPSR_irq, r0
	ldr	r0, [r12, #W(6)]
	msr	SP_abt, r0
	ldr	r0, [r12, #W(7)]
	msr	LR_abt, r0
	ldr	r0, [r12, #W(8)]
	msr	SPSR_abt, r0
	ldr	r0, [r12, #W(9)]
	msr	SP_und, r0
	ldr	r0, [r12, #W(10)]
	msr	LR_und, r0
	ldr	r0, [r12, #W(11)]
	msr	SPSR_und, r0
	ldr	r0, [r12, #W(12)]
	msr	SP_fiq, r0
	ldr	r0, [r12, #W(13)]
	msr	LR_fiq, r0
	ldr	r0, [r12, #W(14)]
	msr	SPSR_fiq, r0
	MARK	5
	ldr	r0, [r12, #W(42)]
	msr	ELR_hyp, r0
	ldr	r0, [r12, #W(0)]
	msr	spsr_cxsf, r0
	eret
	.balign	4
8:	.word	0x4E004000

/* AArch64: marks 0x64 and CurrentEL in the SRAM, then stays (the watchdog restarts the console) */
	.balign	64
	.global	pico_smp_a64_probe
pico_smp_a64_probe:
	.word	SMP_A64_MOVZ_X7
	.word	SMP_A64_MOVK_X7
	.word	0x52800c86
	.word	0xb90000e6
	.word	0xd5384246
	.word	0xb90004e6
	.word	0xb90010e0
	.word	0xb90014e1
	.word	0xd5381006
	.word	0xb90018e6
	.word	0x14000000

/* A started core. Comms block (pico_smp.c): the first 64 bytes are the boot core's to write,
 * the second 64 bytes this core's. */
#define C_PING	0
#define C_MAGIC	64
#define C_CPSR0	68
#define C_MIDR	72
#define C_MPIDR	76
#define C_COUNT	80
#define C_PONG	84
#define C_CPSR1	88
#define C_SCTLR	92
#define C_HCR	96
#define C_ARG	100

	.balign	64
	.global	pico_smp_sec_entry
pico_smp_sec_entry:
	mov	r9, r0
	MARK	0x21, 3
	ldr	r12, 8f
	mrs	r4, cpsr
	str	r4, [r12, #C_CPSR0]
	str	r9, [r12, #C_ARG]
	and	r0, r4, #0x1f
	cmp	r0, #0x1a
	bne	2f
	mrc	p15, 4, r0, c1, c1, 0
	str	r0, [r12, #C_HCR]
	EL2_OPEN
	adr	r0, 2f
	msr	ELR_hyp, r0
	movw	r0, #0x1d3			@ SVC, A I F masked, ARM
	msr	spsr_cxsf, r0
	eret
2:	mrc	p15, 0, r0, c0, c0, 0
	str	r0, [r12, #C_MIDR]
	mrc	p15, 0, r0, c0, c0, 5
	str	r0, [r12, #C_MPIDR]
	mrs	r0, cpsr
	str	r0, [r12, #C_CPSR1]
	mrc	p15, 0, r0, c1, c0, 0
	str	r0, [r12, #C_SCTLR]
	movw	r0, #0x0c0e
	movt	r0, #0x5ec0
	str	r0, [r12, #C_MAGIC]
	mov	r2, #0
3:	add	r2, r2, #1
	str	r2, [r12, #C_COUNT]
	ldr	r0, [r12, #C_PING]
	str	r0, [r12, #C_PONG]
	b	3b
	.balign	4
8:	.word	0x4E000000

/* A faithful copy of the LK's own kernel handoff, so the firmware drops to an address we chose as
 * if it were the kernel. Convention (app/mt_boot: lk_jump64):
 *   64-bit: (entry, tags, 0, 1)   32-bit: (entry, machtype, tags, 0)
 * void pico_smp_kboot(unsigned entry, unsigned a1, unsigned a2, unsigned width)
 * Marks 0x50 in the SRAM tail just before the call; it does not return (the firmware jumps to the
 * kernel, so the console restarts if our entry does not take hold). */
FUNCTION(pico_smp_kboot)
	MARK	0x50
	mov	r5, r3				@ width -> r4 (fourth arg)
	mov	r3, r2				@ a2 -> r3 (third arg)
	mov	r2, r1				@ a1 -> r2 (second arg)
	mov	r1, r0				@ entry -> r1 (first arg)
	mov	r4, r5
	movw	r0, #0x0115			@ MTK_SIP_KERNEL_BOOT 0x82000115
	movt	r0, #0x8200
	dsb
	isb
	smc	#0
	MARK	0x51				@ only if it returned to us
	bx	lr

/* minimal AArch32 landing: a mark, then spin (the watchdog restarts the console) */
	.balign	64
	.global	pico_smp_a32_land
pico_smp_a32_land:
	movw	r7, #0xd7c0
	movt	r7, #0x0011
	mov	r6, #0x32
	str	r6, [r7, #8]
	dsb
1:	b	1b

/* minimal AArch64 landing */
	.balign	64
	.global	pico_smp_a64_land
	@ AArch64 worker, x0 = shared-block physical base (PSCI context_id).
pico_smp_a64_land:
	.word	SMP_A64_MOVZ_X7
	.word	SMP_A64_MOVK_X7
	.word	0x52800ee6
	.word	0xb90008e6
	.word	0xb90010e0
	.word	0xd5384241
	.word	0xd342fc21
	.word	0x92400421
	.word	0xb90014e1
	.word	0xb94020e9
	.word	0xd5033f9f
	.word	0x528181c1
	.word	0x72abd801
	.word	0xb9004121
	.word	0xd5380001
	.word	0xb9004921
	.word	0xd53800a1
	.word	0xb9004d21
	.word	0x52800002
	.word	0xb9407121
	.word	0x35000121
	.word	0x11000442
	.word	0xb9005122
	.word	0xb9400121
	.word	0x11000421
	.word	0xb9005521
	.word	0xb90018e2
	.word	0xd5033fbf
	.word	0x17fffff7
	.word	0x52800fe6
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd2800040
	.word	0xf2b08000
	.word	0xd4000003
	.word	0x17fffffa

	.balign 64
	.global pico_smp_coh_land
pico_smp_coh_land:
	.word	SMP_A64_MOVZ_X7
	.word	SMP_A64_MOVK_X7
	.word	0x52801106
	.word	0xb90008e6
	.word	0xb94020e9
	.word	0xb94024ea
	.word	0xd5384242
	.word	0xd342fc42
	.word	0x92400442
	.word	0xb90014e2
	.word	0x52801206
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd2801fe1
	.word	0xd518a201
	.word	0x52801146
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd286a321
	.word	0xf2a01001
	.word	0xd5182041
	.word	0x52801166
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd518200a
	.word	0x52801186
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd5033f9f
	.word	0xd5033fdf
	.word	0xd508871f
	.word	0xd5033f9f
	.word	0xd5033fdf
	.word	0xd5381001
	.word	0x528011c6
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xb2400021
	.word	0xb27e0021
	.word	0xb2740021
	.word	0xd5181001
	.word	0xd5033fdf
	.word	0x52801126
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd5380001
	.word	0xb9004921
	.word	0xd53800a1
	.word	0xb9004d21
	.word	0x52800003
	.word	0xb9407121
	.word	0x35000181
	.word	0x11000463
	.word	0xb9005123
	.word	0xb9406121
	.word	0x340000c1
	.word	0xb9406522
	.word	0xb9006922
	.word	0xd5033fbf
	.word	0x52800022
	.word	0xb9006d22
	.word	0xd5033fbf
	.word	0x17fffff4
	.word	0x528011e6
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xd2800040
	.word	0xf2b08000
	.word	0xd4000003
	.word	0x17fffffa

	.balign 64
	.global pico_smp_elsw_land
pico_smp_elsw_land:
	.word	SMP_A64_MOVZ_X7
	.word	SMP_A64_MOVK_X7
	.word	0x52801406
	.word	0xb90008e6
	.word	0xd5384241
	.word	0xb90014e1
	.word	0xd5033f9f
	.word	0x100001a2
	.word	0xd342fc20
	.word	0x92400400
	.word	0xf100081f
	.word	0x540000a0
	.word	0xd2800203
	.word	0xd5184003
	.word	0xd5184022
	.word	0xd69f03e0
	.word	0xd2803a63
	.word	0xd51c4003
	.word	0xd51c4022
	.word	0xd69f03e0
	.word	SMP_A32_MOVW_R7
	.word	SMP_A32_MOVT_R7
	.word	0xe3a06032
	.word	0xe5876018
	.word	0xf57ff04f
	.word	0xeafffffe

/* AArch64 EL1 host for a 32 bit EL0 guest (source: pico_smp_gbahost64.S.src). Exception vectors
 * first (2 KB aligned), the entry at +0x800. */
	.balign	2048
	.global	pico_smp_gba_land
pico_smp_gba_land:
	.word	0x140002ab
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400028b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400026b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400024b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400022b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400020b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x140001eb
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x140001cb
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x140001ab
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400018b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400016b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400014b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x140000b6
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x1400010b
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x140000eb
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0x140000cb
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	0xd503201f
	.word	SMP_A64_MOVZ_X7
	.word	SMP_A64_MOVK_X7
	.word	0x52801606
	.word	0xb90008e6
	.word	0xb94020e9
	.word	0xb94024ea
	.word	0xb94028eb
	.word	0xb9402cec
	.word	0xd5033f9f
	.word	0xd2801fe1
	.word	0xd518a201
	.word	0xd286a321
	.word	0xf2a01001
	.word	0xd5182041
	.word	0xd518200a
	.word	0x10ffbe21
	.word	0xd518c001
	.word	0xd2a00601
	.word	0xd5181041
	.word	0xd2800061
	.word	0xd518e101
	.word	0xd5033f9f
	.word	0xd5033fdf
	.word	0xd508871f
	.word	0xd5033f9f
	.word	0xd5033fdf
	.word	0xd500409f
	.word	0xd5381001
	.word	0xb2400021
	.word	0xb27e0021
	.word	0xb2740021
	.word	0xb27b0021
	.word	0xb2700021
	.word	0xb26e0021
	.word	0xb2660021
	.word	0xb2710021
	.word	0x927ef821
	.word	0x926cf821
	.word	0xb2690021
	.word	0xd5181001
	.word	0xd5033fdf
	.word	0x52801626
	.word	0xb90008e6
	.word	0xd2803a03
	.word	0x3600006b
	.word	0xb27b0063
	.word	0x927ff96b
	.word	0xd5184003
	.word	0xd518402b
	.word	0xaa0c03ed
	.word	0xd518410c
	.word	0xd280000e
	.word	0xaa0903e0
	.word	0xd69f03e0
	.word	0xd5385210
	.word	0xd35afe11
	.word	0xb50009f1
	.word	0xd5384032
	.word	0xd5384013
	.word	0x36280e13
	.word	0x79400254
	.word	0x79400655
	.word	0x529dc0f6
	.word	0x6b16029f
	.word	0x54000d61
	.word	0x12180eb6
	.word	0x713c02df
	.word	0x54000d01
	.word	0x530c3eb7
	.word	0x10000078
	.word	0x8b170f18
	.word	0xd61f0300
	.word	0xaa0003f9
	.word	0x1400001f
	.word	0xaa0103f9
	.word	0x1400001d
	.word	0xaa0203f9
	.word	0x1400001b
	.word	0xaa0303f9
	.word	0x14000019
	.word	0xaa0403f9
	.word	0x14000017
	.word	0xaa0503f9
	.word	0x14000015
	.word	0xaa0603f9
	.word	0x14000013
	.word	0xaa0703f9
	.word	0x14000011
	.word	0xaa0803f9
	.word	0x1400000f
	.word	0xaa0903f9
	.word	0x1400000d
	.word	0xaa0a03f9
	.word	0x1400000b
	.word	0xaa0b03f9
	.word	0x14000009
	.word	0xaa0c03f9
	.word	0x14000007
	.word	0xaa0d03f9
	.word	0x14000005
	.word	0xaa0e03f9
	.word	0x14000003
	.word	0xaa1f03f9
	.word	0x14000001
	.word	0x12000eba
	.word	0x53051ebb
	.word	0x71002f5f
	.word	0x54000061
	.word	0xd50b7b39
	.word	0x14000011
	.word	0x71002b5f
	.word	0x54000061
	.word	0xd50b7a39
	.word	0x1400000d
	.word	0x71003b5f
	.word	0x54000061
	.word	0xd50b7e39
	.word	0x14000009
	.word	0x7100175f
	.word	0x540000e1
	.word	0x7100077f
	.word	0x54000061
	.word	0xd50b7539
	.word	0x14000003
	.word	0x3500005b
	.word	0xd508751f
	.word	0x91001252
	.word	0xd5184032
	.word	SMP_A64_MOVZ_X20
	.word	SMP_A64_MOVK_X20
	.word	0xb9402295
	.word	0xb94092b6
	.word	0x110006d6
	.word	0xb90092b6
	.word	0xd69f03e0
	.word	0xf100463f
	.word	0x54000461
	.word	0x52800051
	.word	0x72a001f1
	.word	0x6b1100ff
	.word	0x54000120
	.word	0x52801ff1
	.word	0x72a001f1
	.word	0x6b1100ff
	.word	0x54000361
	.word	0xd2800040
	.word	0xf2b08000
	.word	0xd4000003
	.word	0x14000017
	.word	0x927a6412
	.word	0x2a0103f3
	.word	0xd50b7b32
	.word	0x91010252
	.word	0xeb13025f
	.word	0x54ffffa3
	.word	0xd5033b9f
	.word	0x927a6412
	.word	0xd50b7532
	.word	0x91010252
	.word	0xeb13025f
	.word	0x54ffffa3
	.word	0xd5033b9f
	.word	0xd5033fdf
	.word	SMP_A64_MOVZ_X20
	.word	SMP_A64_MOVK_X20
	.word	0xb9402295
	.word	0xb94092b6
	.word	0x110006d6
	.word	0xb90092b6
	.word	0xd2800000
	.word	0xd69f03e0
	.word	SMP_A64_MOVZ_X20
	.word	SMP_A64_MOVK_X20
	.word	0xb9402295
	.word	0xd5385210
	.word	0xb90082b0
	.word	0xd5384030
	.word	0xb90086b0
	.word	0xd5386010
	.word	0xb9008ab0
	.word	0xd5384010
	.word	0xb9008eb0
	.word	0x52800030
	.word	0xb90096b0
	.word	0x528017f0
	.word	0xb9000a90
	.word	0xd5033fbf
	.word	0x17ffffff

	.balign 64
	.global pico_smp_clk_land
pico_smp_clk_land:
	.word	SMP_A64_MOVZ_X7
	.word	SMP_A64_MOVK_X7
	.word	0xb94020e9
	.word	0xd28000e0
	.word	0xd51b9c00
	.word	0xd2b00000
	.word	0xd51b9c20
	.word	0xd5033fdf
	.word	0xd53be021
	.word	0xd53b9d02
	.word	0xd5033fdf
	.word	0xd2884803
	.word	0xf2a02623
	.word	0xf1000463
	.word	0x54ffffe1
	.word	0xd5033fdf
	.word	0xd53be024
	.word	0xd53b9d05
	.word	0xcb010084
	.word	0xcb0200a5
	.word	0xb9006125
	.word	0xd360fca5
	.word	0xb9006525
	.word	0xb9006924
	.word	0xd360fc84
	.word	0xb9006d24
	.word	0xd5380006
	.word	0xb9004926
	.word	0xd53800a6
	.word	0xb9004d26
	.word	0xd5033f9f
	.word	0x52801806
	.word	0xb90008e6
	.word	0xd5033f9f
	.word	0xb9407121
	.word	0x35000041
	.word	0x17fffffe
	.word	0xd2800040
	.word	0xf2b08000
	.word	0xd4000003
	.word	0x17fffffd
