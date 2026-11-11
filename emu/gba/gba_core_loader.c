/*
 * gba_core_loader.c - LK side of the loadable GBA cores (see gba_core_abi.h).
 *
 * Compiled INTO lk_a (not the blob). gba_core_load_file() copies a GBA core from the SD card
 * (/cores/armgba-pico.blob or /cores/gpsp-pico.blob, core_file.h) into the GBA core slot, calls
 * its entry with the imports table and returns the export table the driver drives the core
 * through. Both cores use the same slot, so loading one replaces the other.
 */
#include "gba_core_abi.h"
#include "core_file.h"

/* imports the blob calls back into LK for */
extern void ayaneo_gba_audio_submit(const short *interleaved, unsigned frames);
extern long gba_host_time(void);
extern void gba_yield_to_main(void);

/* The GBA core slot: the VMA both cores are linked to, up to the GB/GBC core slot. It sits
 * within ARM BL's +-32 MB reach of the JIT caches at 0x4E000000 (gpSP's dynarec calls into the
 * blob's helpers directly). */
#define GBA_CORE_BLOB_VMA  0x4E400000u
#define GBA_CORE_SLOT_END  0x4E800000u

/* a monotonic millisecond count for the core (the black box keeps it wrap safe) */
extern unsigned int ayaneo_bb_uptime_ms(void);
static unsigned int core_host_ms(void) { return ayaneo_bb_uptime_ms(); }

static const struct gba_core_imports s_imports = {
	ayaneo_gba_audio_submit,
	gba_host_time,
	gba_yield_to_main,
	core_host_ms,
};

static int core_version_ok(unsigned v) { return v >= GBA_CORE_ABI_VERSION && v <= GBA_CORE_ABI_MAX; }

/* the reason the last load failed (core_file.h CORE_FILE_*), for the menu and oem diag */
volatile int g_gba_core_err;

const struct gba_core_exports *gba_core_load_file(fat_vol *v, const char *path)
{
	gba_core_blob_init_fn init;
	const struct gba_core_exports *ex;
	int err;

	init = (gba_core_blob_init_fn)core_file_load(v, path, GBA_CORE_BLOB_VMA,
						      GBA_CORE_SLOT_END - GBA_CORE_BLOB_VMA,
						      GBA_CORE_ABI_MAGIC, GBA_CORE_ABI_VERSION, GBA_CORE_ABI_MAX, &err);
	g_gba_core_err = err;
	if (!init)
		return 0;
	ex = init(&s_imports);
	if (!ex || ex->magic != GBA_CORE_ABI_MAGIC || !core_version_ok(ex->version)) {
		g_gba_core_err = CORE_FILE_REJECTED;
		return 0;
	}
	return ex;
}
