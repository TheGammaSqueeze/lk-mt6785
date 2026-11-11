/*
 * core_file.h - loading an emulator core from the SD card into its fixed DRAM slot.
 *
 * Every core is a flat image linked to run at one address, with a 20-byte header in front:
 *   u32 magic, u32 version, u32 entry (offset), u32 load_size (bytes in the file to copy),
 *   u32 total_span (load_size plus the BSS to zero).
 * The cores are built in their own repositories (armGBA-pico, gpsp-pico, gambatte-pico,
 * snes9x-pico, genesis-plus-gx-pico) and copied to /cores on the card.
 */
#ifndef CORE_FILE_H
#define CORE_FILE_H

#include "fat_ro.h"

#define CORE_PATH_ARMGBA   "/cores/armgba-pico.blob"
#define CORE_PATH_GPSP     "/cores/gpsp-pico.blob"
#define CORE_PATH_GAMBATTE "/cores/gambatte-pico.blob"
#define CORE_PATH_SNES9X   "/cores/snes9x-pico.blob"
#define CORE_PATH_GENESIS  "/cores/genesis-plus-gx-pico.blob"

enum {
	CORE_FILE_OK = 0,
	CORE_FILE_NO_CARD,	/* no mounted card */
	CORE_FILE_MISSING,	/* the file is not there */
	CORE_FILE_HEADER,	/* short file, or not this core's magic or version */
	CORE_FILE_SIZE,		/* the header's sizes do not fit the file or the slot */
	CORE_FILE_READ,		/* the card read failed */
	CORE_FILE_REJECTED,	/* the core's entry refused (its own magic or version) */
};

/* Copy the image at path into [vma, vma + slot_size), zero its BSS and make the code fetchable.
 * The header must carry magic and a version in [ver_lo, ver_hi]. Returns the entry address,
 * or 0 with *err set (err may be 0). */
void *core_file_load(fat_vol *v, const char *path, unsigned vma, unsigned slot_size,
		     unsigned magic, unsigned ver_lo, unsigned ver_hi, int *err);

/* A short reason for a CORE_FILE_* code, for the menu and oem diag. */
const char *core_file_error(int err);

#endif
