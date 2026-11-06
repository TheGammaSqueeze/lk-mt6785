@ GammaOS latency test ROM: the cartridge entry. The header that follows the branch is filled in
@ by build.sh (title, game code, checksum); the Nintendo logo area stays empty, so the ROM starts
@ on emulators that skip the BIOS (the GammaOS LK cores do), not through a real BIOS.
	.section .text.entry, "ax"
	.arm
	.global _start
_start:
	b	reset
	.space	0xC0 - 4
reset:
	ldr	sp, =0x03007F00
	ldr	r0, =main
	bx	r0
	.pool
