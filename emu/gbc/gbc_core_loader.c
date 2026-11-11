/*
 * gbc_core_loader.c - LK side of the loadable gambatte GB/GBC core (see gbc_core_abi.h).
 *
 * Compiled INTO lk_a. gbc_core_load() copies the core from the SD card (CORE_PATH_GAMBATTE,
 * core_file.h) into its fixed DRAM slot, calls the blob's entry with the imports table and
 * returns the export table the session drives the core through.
 */
#include "gbc_core_abi.h"
#include "../gba/core_file.h"

/* imports the core calls back into LK for */
extern unsigned ayaneo_gbc_pad_mask(void);   /* physical pad -> gambatte button bits */
extern long gba_host_time(void);             /* wall-clock seconds for the cart RTC */

static const struct gbc_core_imports s_imports = {
	ayaneo_gbc_pad_mask,
	gba_host_time,
};

#define CORE_SLOT_VMA  0x4E800000u
#define CORE_SLOT_END  0x4F000000u

/* diagnostics for `fastboot oem diag`: 0 ok, else the core_file.h CORE_FILE_* reason */
volatile unsigned g_gbc_dbg_loaderr;

const struct gbc_core_exports *gbc_core_load(fat_vol *v)
{
	gbc_core_blob_init_fn init;
	const struct gbc_core_exports *ex;
	int err;

	init = (gbc_core_blob_init_fn)core_file_load(v, CORE_PATH_GAMBATTE, CORE_SLOT_VMA,
						      CORE_SLOT_END - CORE_SLOT_VMA,
						      GBC_CORE_ABI_MAGIC, GBC_CORE_ABI_VERSION, GBC_CORE_ABI_VERSION, &err);
	g_gbc_dbg_loaderr = (unsigned)err;
	if (!init)
		return 0;
	ex = init(&s_imports);
	if (!ex || ex->magic != GBC_CORE_ABI_MAGIC || ex->version != GBC_CORE_ABI_VERSION) {
		g_gbc_dbg_loaderr = CORE_FILE_REJECTED;
		return 0;
	}
	return ex;
}
