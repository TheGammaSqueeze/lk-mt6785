/* Link script for the loadable Genesis-Plus-GX core blob (see genesis_core_abi.h).
 *
 * Flat binary at a FIXED DRAM VMA in the k85v1_64 WB window [0x4E000000,0x56000000).
 * Placed at 0x4F800000: ABOVE the snes9x blob (0x4F000000, ~2-4 MB) and below the shared
 * 0x50000000 emulator arena, giving 8 MB to snes9x and 8 MB here. Only ONE core runs at a time
 * and each blob is reloaded per session, so the VMAs just avoid confusion. GPGX's large runtime
 * buffers (cart.rom up to 32 MB, work RAM, VRAM, sound) are malloc'd into the 0x50000000 arena
 * via -DUSE_DYNAMIC_ALLOC, so this slot holds only the image + ~3 MB BSS (total span ~4.3 MB).
 *
 * Header (image offset 0):
 *   u32 magic   = 0x31474553 "SEG1"
 *   u32 version = 1
 *   u32 entry   = offset of genesis_core_blob_init from the image start
 *   u32 load    = bytes present in the file (load these, then zero up to span)
 *   u32 span    = end of BSS offset (total DRAM footprint)
 */
ENTRY(genesis_core_blob_init)

BLOB_VMA = 0x4F800000;

SECTIONS
{
	. = BLOB_VMA;

	.header : {
		LONG(0x31474553);                        /* [0] magic "SEG1" */
		LONG(1);                                 /* [1] version */
		LONG(genesis_core_blob_init - BLOB_VMA); /* [2] entry offset */
		LONG(__blob_load_end - BLOB_VMA);        /* [3] load_size */
		LONG(__blob_end - BLOB_VMA);             /* [4] total_span */
	}

	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
	.data : { *(.data .data.*) }
	.data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }

	/* libgcc's ARM unwinder references these table bounds even though we never throw. Define
	 * them equal so the unwinder sees an EMPTY table. */
	. = ALIGN(4);
	__exidx_start = .;
	__exidx_end = .;

	. = ALIGN(4);
	__init_array_start = .;
	.init_array : {
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
	}
	__init_array_end = .;

	. = ALIGN(16);
	__blob_load_end = .;

	.bss (NOLOAD) : {
		__blob_bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__blob_bss_end = .;
	}
	__blob_end = .;

	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note.*) *(.fini_array*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
}
