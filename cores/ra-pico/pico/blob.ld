/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze */
/* ra-pico: link script for the RetroAchievements blob (see ra_core_abi.h).
 *
 * The LK copies the flat image to the start of the RA slot (RA_SLOT_PA, 4 MiB after the Wi-Fi
 * memory), zeroes [load_size, total_span), syncs the caches and calls the entry found through the
 * header with its imports. The rest of the slot, from total_span on, is the heap it hands to
 * heap_init: rcheevos' sets, their logic and the answers it reads.
 *
 * Header (image offset 0):
 *   u32 magic      0x31414152 "RAA1"
 *   u32 version    1 (RA_CORE_ABI_VERSION: change both together, the link script cannot include
 *                     the header)
 *   u32 entry      offset of ra_core_blob_init
 *   u32 load_size  bytes present in the file
 *   u32 total_span bytes to reserve, BSS included
 */
ENTRY(ra_core_blob_init)

BLOB_VMA = 0xBE600000;
BLOB_MAX = 0xBE700000;   /* the image and its BSS in the first MiB; three for the heap */

SECTIONS
{
	. = BLOB_VMA;
	.header : {
		LONG(0x31414152);
		LONG(1);
		LONG(ra_core_blob_init - BLOB_VMA);
		LONG(__blob_load_end - BLOB_VMA);
		LONG(__blob_end - BLOB_VMA);
	}
	.text : { KEEP(*(.text.ra_core_blob_init)) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.init_array : { __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .; }
	/* the padding to 16 bytes sits inside .data, the last section in the file, so the flat image
	 * always carries it and its size is exactly the load size in the header */
	.data : { *(.data .data.*) . = ALIGN(16); }
	__blob_load_end = .;
	.bss (NOLOAD) : {
		__bss_start__ = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end__ = .;
	}
	__blob_end = .;
	end = .;
	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note.*) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(__blob_end <= BLOB_MAX, "ra-pico: the image and its BSS outgrew the first MiB of the RA slot")
