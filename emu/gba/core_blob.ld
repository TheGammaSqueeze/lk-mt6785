/* Link script for the loadable GBA core blob (see gba_core_abi.h).
 *
 * The blob is a flat binary linked at a FIXED DRAM VMA in the k85v1_64 WB window
 * [0x4E000000,0x56000000). It sits in the free gap [0x4E340000,0x50000000) between the
 * gpSP JIT caches (0x4E000000, 3.25 MB) and the core arena (0x50000000, 64 MB), clear of
 * the menu buffers (0x51800000+). Placing it at 0x4E400000 keeps the core's helper
 * functions within ARM BL's +-32 MB reach of the JIT caches at 0x4E000000 (the dynarec
 * emits direct BLs from generated code in the caches to those helpers - see gba_wrap.c),
 * which is the constraint that forbids the far arena. ~3 MB span, ~25 MB of headroom
 * below the arena. LK loads the flat image to this VMA, zeroes [bss_start,bss_end],
 * flushes caches, then calls the entry (found via the 16-byte header at the image start).
 *
 * Header (image offset 0):
 *   u32 magic   = 0x41424743 "GCBA"
 *   u32 version = 1
 *   u32 entry   = offset of gba_core_blob_init from the image start
 *   u32 bss_end = offset of the end of BSS (image bytes to load = this; zero up to here)
 */
ENTRY(gba_core_blob_init)

BLOB_VMA = 0x4E400000;

SECTIONS
{
	. = BLOB_VMA;

	.header : {
		LONG(0x41424743);                    /* [0] magic "GCBA" */
		LONG(1);                             /* [1] version */
		LONG(gba_core_blob_init - BLOB_VMA); /* [2] entry offset */
		LONG(__blob_load_end - BLOB_VMA);    /* [3] load_size: bytes present in the file */
		LONG(__blob_end - BLOB_VMA);         /* [4] total_span: zero [load_size, total_span] */
	}

	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }

	. = ALIGN(16);
	__blob_load_end = .;               /* end of bytes present in the flat image */

	.bss (NOLOAD) : {
		__blob_bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__blob_bss_end = .;
	}
	__blob_end = .;

	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note.*) }
}
