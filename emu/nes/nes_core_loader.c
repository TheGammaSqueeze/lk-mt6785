/*
 * nes_core_loader.c - the LK side of the loadable armNES core (nes_core_abi.h).
 *
 * Compiled into lk_a. nes_core_load() copies the core from the card (CORE_PATH_NES, core_file.h)
 * into its DRAM slot, calls the blob's entry with the imports and returns the export table the
 * session drives the core through. The slot is the Genesis core's: one core runs at a time, and
 * each session loads its core afresh.
 */
#include "nes_core_abi.h"
#include "../gba/core_file.h"

/* what the core calls back into the LK for */
extern unsigned ayaneo_nes_pad_mask(unsigned port);   /* the pad as RETRO_DEVICE_ID_JOYPAD_* bits */
extern long gba_host_time(void);                       /* wall clock seconds */

static const struct nes_core_imports s_imports = {
	ayaneo_nes_pad_mask,
	gba_host_time,
};

#define CORE_SLOT_VMA  0x4F800000u
#define CORE_SLOT_END  0x50000000u

/* oem diag: 0 loaded, else the core_file.h CORE_FILE_* reason */
volatile unsigned g_nes_dbg_loaderr;

const struct nes_core_exports *nes_core_load(fat_vol *v)
{
	nes_core_blob_init_fn init;
	const struct nes_core_exports *ex;
	int err;

	init = (nes_core_blob_init_fn)core_file_load(v, CORE_PATH_NES, CORE_SLOT_VMA,
						     CORE_SLOT_END - CORE_SLOT_VMA,
						     NES_CORE_ABI_MAGIC, NES_CORE_ABI_VERSION, NES_CORE_ABI_V3, &err);
	g_nes_dbg_loaderr = (unsigned)err;
	if (!init)
		return 0;
	ex = init(&s_imports);
	if (!ex || ex->magic != NES_CORE_ABI_MAGIC || ex->version < NES_CORE_ABI_VERSION || ex->version > NES_CORE_ABI_V3) {
		g_nes_dbg_loaderr = CORE_FILE_REJECTED;
		return 0;
	}
	return ex;
}
