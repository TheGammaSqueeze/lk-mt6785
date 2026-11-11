/*
 * snes_core_loader.c - LK side of the loadable snes9x SNES core (see snes_core_abi.h).
 *
 * Compiled INTO lk_a. snes_core_load() copies the core from the SD card (CORE_PATH_SNES9X,
 * core_file.h) into its fixed DRAM slot, calls the blob's entry with the imports table and
 * returns the export table the session drives the core through.
 */
#include "snes_core_abi.h"
#include "../gba/core_file.h"

/* imports the core calls back into LK for */
extern unsigned ayaneo_snes_pad_mask(void);   /* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits */
extern long gba_host_time(void);               /* wall-clock seconds for SRTC / BS-X carts */

static const struct snes_core_imports s_imports = {
	ayaneo_snes_pad_mask,
	gba_host_time,
};

#define CORE_SLOT_VMA  0x4F000000u
#define CORE_SLOT_END  0x4F800000u

/* diagnostics for `fastboot oem diag`: 0 ok, else the core_file.h CORE_FILE_* reason */
volatile unsigned g_snes_dbg_loaderr;

const struct snes_core_exports *snes_core_load(fat_vol *v)
{
	snes_core_blob_init_fn init;
	const struct snes_core_exports *ex;
	int err;

	init = (snes_core_blob_init_fn)core_file_load(v, CORE_PATH_SNES9X, CORE_SLOT_VMA,
						      CORE_SLOT_END - CORE_SLOT_VMA,
						      SNES_CORE_ABI_MAGIC, SNES_CORE_ABI_VERSION, SNES_CORE_ABI_VERSION, &err);
	g_snes_dbg_loaderr = (unsigned)err;
	if (!init)
		return 0;
	ex = init(&s_imports);
	if (!ex || ex->magic != SNES_CORE_ABI_MAGIC || ex->version != SNES_CORE_ABI_VERSION) {
		g_snes_dbg_loaderr = CORE_FILE_REJECTED;
		return 0;
	}
	return ex;
}
