/*
 * gba_core_exports.c - the blob side of the LK<->core ABI (see gba_core_abi.h).
 *
 * Compiled ONLY into the loadable core blob (not into lk_a). It does two jobs:
 *
 *  1. Provides local forwarders for the handful of services the core calls out to LK
 *     for (audio sink, host time, the vblank yield). The core's calls resolve to these
 *     forwarders inside the blob; each hops through the imports table LK passes at init,
 *     so the real implementations stay in LK (gba_driver.c / ayaneo_audio.c). Keeping
 *     the forwarders in the blob means the core's own object files need no edits.
 *
 *  2. Exposes gba_core_blob_init(): LK calls it once after loading the blob into DRAM;
 *     it records the imports and returns the export table (pointers to the core's
 *     gba_core_* API and the three shared flags) that the LK driver then drives through.
 *
 * memory_map_read/write are core-internal (defined in cpu.c), not imports.
 */
#include "gba_core_abi.h"

/* ---- the core's API, defined in the core objects (gba_wrap.c / gba_shim.c / main.c) ---- */
extern int   gba_core_init(void *arena, unsigned size);
extern int   gba_core_start(unsigned romsz, const void *bios16k);
extern void  gba_core_enter_bios(void);
extern void  reset_gba(void);
extern void  gba_core_cpu_loop(void);
extern void  gba_core_pre_frame(void);
extern void  gba_core_post_frame(void);
extern void  gba_frame_boundary_finish(void);
extern unsigned int gba_core_cpu_ticks(void);
extern void  gba_core_set_keys(unsigned gba_mask);
extern const unsigned short *gba_core_screen(void);
extern void  gba_core_screen_fill(unsigned short v);
extern unsigned gba_core_state_size(void);
extern void  gba_core_state_save(void *buf);
extern void  gba_core_state_load(const void *buf);
extern void  gba_sound_ring_save(void *dst);
extern void  gba_sound_ring_load(const void *src);
extern unsigned char *gba_core_rom_ptr(void);
extern unsigned gba_core_rom_capacity(void);
extern unsigned char *gba_core_scratch_ptr(void);
extern unsigned gba_core_scratch_size(void);
extern void *gba_core_backup_ptr(void);
extern unsigned gba_core_backup_size(void);
extern int   dynarec_enable;
extern volatile int g_gba_audio_suppress;
extern int   g_gba_load_light;

/* ---- imports: filled by gba_core_blob_init, hopped through by the forwarders ---- */
static const struct gba_core_imports *s_imp;

/* The core calls these three by name; here in the blob they forward to LK. */
void ayaneo_gba_audio_submit(const short *interleaved, unsigned frames)
{
	if (s_imp && s_imp->audio_submit)
		s_imp->audio_submit(interleaved, frames);
}
long gba_host_time(void)
{
	return (s_imp && s_imp->host_time) ? s_imp->host_time() : 0;
}
void gba_yield_to_main(void)
{
	if (s_imp && s_imp->yield_to_main)
		s_imp->yield_to_main();
}

/* ---- the single blob entry point ---- */
const struct gba_core_exports *gba_core_blob_init(const struct gba_core_imports *imp)
{
	static const struct gba_core_exports exports = {
		GBA_CORE_ABI_MAGIC, GBA_CORE_ABI_VERSION,
		gba_core_init, gba_core_start, gba_core_enter_bios, reset_gba,
		gba_core_cpu_loop, gba_core_pre_frame, gba_core_post_frame,
		gba_frame_boundary_finish, gba_core_cpu_ticks, gba_core_set_keys,
		gba_core_screen, gba_core_screen_fill,
		gba_core_state_size, gba_core_state_save, gba_core_state_load,
		gba_sound_ring_save, gba_sound_ring_load,
		gba_core_rom_ptr, gba_core_rom_capacity,
		gba_core_scratch_ptr, gba_core_scratch_size,
		gba_core_backup_ptr, gba_core_backup_size,
		&dynarec_enable, &g_gba_audio_suppress, &g_gba_load_light,
	};
	s_imp = imp;
	return &exports;
}
