/* Link script for the loadable snes9x core blob (see snes_core_abi.h).
 *
 * Flat binary at a FIXED DRAM VMA in the k85v1_64 WB window [0x4E000000,0x56000000).
 * Placed at 0x4F000000: above the gpSP blob (0x4E400000) and the gambatte blob
 * (0x4E800000, ~140 KB) and below the shared 0x50000000 emulator arena. snes9x is bigger
 * (~2-4 MB code+data); its large runtime buffers (ROM/RAM/work) live in the 0x50000000
 * arena, reused since only one core runs at a time. That leaves this slot only needing to
 * hold the blob image + BSS, with ~16 MB of headroom to the arena.
 *
 * UNLIKE the gambatte blob, .init_array is KEPT (not discarded): snes9x uses
 * std::string/stringstream/std::map, so libstdc++ static constructors (ios_base::Init,
 * locale) must run. snes_shim.cpp's snes_run_init_array() walks [__init_array_start,
 * __init_array_end) at blob entry.
 *
 * Header (image offset 0):
 *   u32 magic   = 0x31534E53 "SNS1"
 *   u32 version = 2
 *   u32 entry   = offset of snes_core_blob_init from the image start
 *   u32 load    = bytes present in the file (load these, then zero up to span)
 *   u32 span    = end of BSS offset (total DRAM footprint)
 */
ENTRY(snes_core_blob_init)

BLOB_VMA = 0x4F000000;

SECTIONS
{
	. = BLOB_VMA;

	.header : {
		LONG(0x31534E53);                     /* [0] magic "SNS1" */
		LONG(2);                              /* [1] version - MUST match SNES_CORE_ABI_VERSION */
		LONG(snes_core_blob_init - BLOB_VMA); /* [2] entry offset */
		LONG(__blob_load_end - BLOB_VMA);     /* [3] load_size */
		LONG(__blob_end - BLOB_VMA);          /* [4] total_span */
	}

	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
	.data : { *(.data .data.*) }
	.data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }

	/* libgcc's ARM unwinder references these table bounds even though we never throw
	 * (exceptions compiled out). Both exidx AND extab are discarded below; define the
	 * bounds equal so the unwinder sees an EMPTY table (never consulted - no throws). */
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
