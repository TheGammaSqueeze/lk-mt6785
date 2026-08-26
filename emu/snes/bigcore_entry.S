/*
 * Big-core (Cortex-A76) PSCI CPU_ON entry stub, EXPERIMENTAL proof-of-life.
 *
 * The core enters this MMU-OFF (fresh power-up, secure PL1). Forced ARM state
 * (bit0=0 entry address) and 16-byte aligned so it executes real ARM regardless
 * of the caller's Thumb build. No stack is used: it writes a magic then an
 * UNBOUNDED counter to the fixed DRAM comms scratch (VA==PA, uncached Device
 * because MMU is off), each with a DSB so the store reaches DRAM. The little
 * core's OSD live-reads comms.counter every frame, so a continuously-climbing
 * "c" value is unambiguous proof the second core is alive and still executing
 * (a one-shot bounded counter would show a static number that can't distinguish
 * "ran once then died" from "alive"). Debug-gated build only.
 */
	.text
	.arm
	.balign 16
	.global bc_entry_arm
	.type   bc_entry_arm, %function
bc_entry_arm:
	movw	r0, #0x0000		/* r0 = 0x54000000  (BC_COMMS_PA) */
	movt	r0, #0x5400
	movw	r1, #0x0DE5		/* r1 = 0xB16C0DE5  (BC_MAGIC) */
	movt	r1, #0xB16C
	str	r1, [r0]		/* comms.magic = BC_MAGIC */
	dsb
	mov	r2, #0
1:
	add	r2, r2, #1
	str	r2, [r0, #4]		/* comms.counter++  (live heartbeat, never parks) */
	dsb
	b	1b
	.size	bc_entry_arm, . - bc_entry_arm
