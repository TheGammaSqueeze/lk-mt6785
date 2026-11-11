/*
 * genesis_core_loader.c - LK side of the loadable Genesis Plus GX (MD/SMS/GG/SG) core (see genesis_core_abi.h).
 *
 * Compiled INTO lk_a. genesis_core_load() copies the core from the SD card (CORE_PATH_GENESIS,
 * core_file.h) into its fixed DRAM slot, calls the blob's entry with the imports table and
 * returns the export table the session drives the core through.
 */
#include "genesis_core_abi.h"
#include "../gba/core_file.h"

/* imports the core calls back into LK for */
extern unsigned ayaneo_genesis_pad_mask(unsigned port);  /* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits */
extern long gba_host_time(void);                          /* wall-clock seconds (unused by GPGX) */

static const struct genesis_core_imports s_imports = {
	ayaneo_genesis_pad_mask,
	gba_host_time,
};

#define CORE_SLOT_VMA  0x4F800000u
#define CORE_SLOT_END  0x50000000u

/* diagnostics for `fastboot oem diag`: 0 ok, else the core_file.h CORE_FILE_* reason */
volatile unsigned g_gen_dbg_loaderr;

const struct genesis_core_exports *genesis_core_load(fat_vol *v)
{
	genesis_core_blob_init_fn init;
	const struct genesis_core_exports *ex;
	int err;

	init = (genesis_core_blob_init_fn)core_file_load(v, CORE_PATH_GENESIS, CORE_SLOT_VMA,
						      CORE_SLOT_END - CORE_SLOT_VMA,
						      GENESIS_CORE_ABI_MAGIC, GENESIS_CORE_ABI_VERSION, GENESIS_CORE_ABI_VERSION, &err);
	g_gen_dbg_loaderr = (unsigned)err;
	if (!init)
		return 0;
	ex = init(&s_imports);
	if (!ex || ex->magic != GENESIS_CORE_ABI_MAGIC || ex->version != GENESIS_CORE_ABI_VERSION) {
		g_gen_dbg_loaderr = CORE_FILE_REJECTED;
		return 0;
	}
	return ex;
}
