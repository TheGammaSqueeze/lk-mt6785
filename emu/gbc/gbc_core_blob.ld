/* Link script for the loadable gambatte GB/GBC core blob (see gbc_core_abi.h).
 *
 * Flat binary at a FIXED DRAM VMA in the k85v1_64 WB window [0x4E000000,0x56000000).
 * Placed at 0x4E800000: above the gpSP core blob's slot (0x4E400000, ~3 MB span) and
 * below the shared emulator arena at 0x50000000, clear of the menu/display buffers.
 * gambatte is a plain interpreter (no dynarec BL-reach constraint), so any WB address in
 * the window works; 0x4E800000 gives each core its own resident slot with ~24 MB of
 * headroom to the arena. LK loads the flat image here, zeroes [bss_start,bss_end],
 * flushes caches, then calls the entry found via the 20-byte header at the image start.
 *
 * Header (image offset 0):
 *   u32 magic   = 0x31434247 "GBC1"
 *   u32 version = 1
 *   u32 entry   = offset of gbc_core_blob_init from the image start
 *   u32 load    = bytes present in the file (load these, then zero up to span)
 *   u32 span    = end of BSS offset (total DRAM footprint)
 */
ENTRY(gbc_core_blob_init)

BLOB_VMA = 0x4E800000;

SECTIONS
{
	. = BLOB_VMA;

	.header : {
		LONG(0x31434247);                    /* [0] magic "GBC1" */
		LONG(1);                             /* [1] version */
		LONG(gbc_core_blob_init - BLOB_VMA); /* [2] entry offset */
		LONG(__blob_load_end - BLOB_VMA);    /* [3] load_size */
		LONG(__blob_end - BLOB_VMA);         /* [4] total_span */
	}

	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
	.data : { *(.data .data.*) }
	/* C++ vtables / init: no static ctors are run (gambatte builds with
	 * -fno-use-cxa-atexit and the core is constructed via operator new at runtime),
	 * but keep any linkonce data the templates emit. */
	.data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }

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

	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note.*) *(.init_array*) *(.fini_array*) }
}
