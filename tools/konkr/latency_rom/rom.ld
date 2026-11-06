/* GammaOS latency test ROM: everything runs from the cartridge (no data in RAM) */
ENTRY(_start)
SECTIONS
{
	. = 0x08000000;
	.text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
	/DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
