/*
 * Bridge between the LK-side driver (gba_driver.c) and the gpSP core.
 *
 * gpSP has no top-level "run one frame" entry: the CPU runs in its own thread
 * and yields to the frontend at each vblank via switch_to_main_thread(). We keep
 * that model but drive the two halves from LK: the driver runs gba_core_cpu_loop()
 * on a dedicated LK thread and implements the frame hand-off (gba_yield_to_main),
 * while switch_to_main_thread() (called by the core) bounces back into it.
 *
 * This file also owns the DRAM-arena layout for the core (32 MB ROM buffer, the
 * dynarec translation caches, the page map and the screen buffer), replaces the
 * globals the removed libretro.c used to define, and provides the audio + input
 * callbacks the core polls.
 *
 * Compiled with arm-none-eabi-gcc and the core include paths (see
 * build_core_gba.sh); it must NOT pull in LK kernel headers - all LK primitives
 * cross as plain extern C declarations, and the threading lives in the driver.
 */
#include "common.h"
#include "cpu.h"
#include "gba_memory.h"
#include "sound.h"
#include "video.h"
#include "input.h"
#include "main.h"
#include "libretro.h"

/* ---- globals the removed libretro.c used to define ---- */
int dynarec_enable = 1;
int use_libretro_save_method = 0;
u32 skip_next_frame = 0;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 iwram_stack_optimize = 1;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

/* ---- the core calls this at each vblank; the driver parks/resumes threads ---- */
extern void gba_yield_to_main(void);		/* gba_driver.c */
void switch_to_main_thread(void)
{
	gba_yield_to_main();
}

/* audio sink implemented on the LK side (ayaneo_audio.c): s16 stereo @ GBA rate */
extern void ayaneo_gba_audio_submit(const short *interleaved, unsigned frames);

/* ================= DRAM arena layout ================= */
/* Sized so the whole 32 MB max ROM lives resident (no paging) and the dynarec
 * caches are the "big" (non-PSP) gpSP sizes. All executable (dynarec) memory is
 * in this DRAM region, which LK maps RWX. */
#define ROM_BUF_SZ	(32u * 1024 * 1024)
#define SCREEN_PX	(GBA_SCREEN_PITCH * GBA_SCREEN_HEIGHT)	/* 240*160 */

static u8 *s_rom_buf;
static u8 *s_screen;
static u8 *s_scratch;		/* free arena tail: staging for ROM decompression */
static unsigned s_scratch_sz;

/* forward decls (definitions below) */
int16_t gba_input_cb(unsigned port, unsigned device, unsigned index, unsigned id);
size_t gba_audio_cb(const int16_t *data, size_t frames);

/* The dynarec translation caches MUST live within ARM BL's +-32 MB reach of the
 * gpSP helper functions in LK .text (generate_function_call emits a direct BL to
 * e.g. execute_load_u32). Those helpers sit at ~0x4C48xxxx; the nearest free RWX
 * DRAM is the base of the 128 MB SCRATCH region at 0x4E000000 (~27 MB away, in
 * range). The caches are small (GBA_LK_SMALL_CACHE in cpu.h, 3.25 MB total) so
 * their far end stays inside the +-32 MB window. The big ROM buffer and all data
 * are accessed via base-register LDR, not branches, so they stay in the caller's
 * far arena (0x50000000) with no range constraint. */
#define GBA_JIT_CACHE_PA	0x4E000000u

/* Lay the core buffers out over the caller's arena. Returns 0 on success, -1 if
 * the arena is too small. */
int gba_core_init(void *arena, unsigned size)
{
	u8 *p = (u8 *)arena;
	u8 *end = p + size;
	unsigned mmap_sz = gamepak_memory_map_size(ROM_BUF_SZ);
	u8 *jit = (u8 *)GBA_JIT_CACHE_PA;

	/* translation caches: fixed near address, within BL range of LK .text */
	rom_translation_cache  = jit;  jit += ROM_TRANSLATION_CACHE_SIZE;
	ram_translation_cache  = jit;  jit += RAM_TRANSLATION_CACHE_SIZE;
	bios_translation_cache = jit;  jit += BIOS_TRANSLATION_CACHE_SIZE;
	rom_translation_ptr  = rom_translation_cache;
	ram_translation_ptr  = ram_translation_cache;
	bios_translation_ptr = bios_translation_cache;

	/* 32 MB ROM buffer (data; distance irrelevant) */
	s_rom_buf = p;                 p += ROM_BUF_SZ;
	/* gamepak page map */
	{ void *mmap = p;              p += (mmap_sz + 4095u) & ~4095u;
	  init_gamepak_buffer_ext(s_rom_buf, ROM_BUF_SZ, mmap); }
	/* video output buffer (RGB565 240x160) */
	p = (u8 *)(((unsigned long)p + 63u) & ~63ul);
	s_screen = p;                  p += SCREEN_PX * sizeof(u16);
	gba_screen_pixels = (u16 *)s_screen;

	if (p > end)
		return -1;

	/* whatever is left of the arena is scratch the driver uses to stage the
	 * compressed ROM during decompression - kept ABOVE the 32 MB ROM buffer so a
	 * full-size (up to 32 MB) ROM can inflate into the buffer without the output
	 * ever overrunning the still-compressed input. */
	p = (u8 *)(((unsigned long)p + 4095u) & ~4095ul);
	s_scratch = p;
	s_scratch_sz = (p < end) ? (unsigned)(end - p) : 0;

	init_sound(1);
	retro_set_input_state(&gba_input_cb);
	retro_set_audio_sample_batch(&gba_audio_cb);
	return 0;
}

/* ROM buffer the driver decompresses the game image straight into. */
unsigned char *gba_core_rom_ptr(void) { return s_rom_buf; }
unsigned gba_core_rom_capacity(void)  { return ROM_BUF_SZ; }

/* Arena scratch (above the ROM buffer) for staging the compressed ROM. */
unsigned char *gba_core_scratch_ptr(void)  { return s_scratch; }
unsigned gba_core_scratch_size(void)       { return s_scratch_sz; }

/* Load BIOS + ROM (already resident in s_rom_buf) and reset. */
int gba_core_start(unsigned romsz, const void *bios16k)
{
	load_bios_mem((const u8 *)bios16k);
	load_gamepak_mem(romsz);
	reset_gba();
	return 0;
}

/* For the boot-logo intro: gpSP's init_cpu() HLE-skips the BIOS and starts at the
 * cart entry (0x08000000), so the Nintendo boot logo never plays. Point the CPU
 * at the BIOS reset vector (0x0) in Supervisor mode instead, so the real BIOS
 * runs its boot sequence - it animates the cart-header logo, then hands off to
 * the cart. Call right after gba_core_start(). */
void gba_core_enter_bios(void)
{
	set_cpu_mode(MODE_SUPERVISOR);
	reg[REG_CPSR] = 0x000000D3;	/* Supervisor, ARM state, IRQ+FIQ disabled */
	reg[REG_PC] = 0x00000000;
	reg[CHANGED_PC_STATUS] = 1;
}

/* The CPU thread body (run by the driver on its own LK thread). Never returns;
 * it yields once per frame through switch_to_main_thread(). */
void gba_core_cpu_loop(void)
{
#ifdef HAVE_DYNAREC
	if (dynarec_enable)
		execute_arm_translate(execute_cycles);
#endif
	execute_arm(execute_cycles);
}

/* frontend-side per-frame hooks (called by the driver around the hand-off) */
void gba_core_pre_frame(void)  { update_input(); }
void gba_core_post_frame(void) { render_audio(); }

const unsigned short *gba_core_screen(void) { return (const unsigned short *)s_screen; }

/* Wipe the core screen to a solid colour. Used before the launch punch-in warmup so a
 * stale previous-game frame (still in s_screen when switching ROMs) is cleared: the
 * warmup then runs the NEW game until it renders real content, instead of opening onto
 * the last game's screenshot. */
void gba_core_screen_fill(unsigned short v)
{
	unsigned short *p = (unsigned short *)s_screen;
	int i;
	if (!p) return;
	for (i = 0; i < SCREEN_PX; i++) p[i] = v;
}

/* ================= input ================= */
/* The driver hands us a ready-made GBA P1 mask (BUTTON_* bits). The core polls
 * per-button through the retro input callback, so translate id -> BUTTON bit. */
static volatile u32 s_keys;
void gba_core_set_keys(unsigned gba_mask) { s_keys = gba_mask & 0x3FF; }

int16_t gba_input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	u32 bit = 0;
	(void)port; (void)device; (void)index;
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_A:      bit = BUTTON_A; break;
	case RETRO_DEVICE_ID_JOYPAD_B:      bit = BUTTON_B; break;
	case RETRO_DEVICE_ID_JOYPAD_SELECT: bit = BUTTON_SELECT; break;
	case RETRO_DEVICE_ID_JOYPAD_START:  bit = BUTTON_START; break;
	case RETRO_DEVICE_ID_JOYPAD_UP:     bit = BUTTON_UP; break;
	case RETRO_DEVICE_ID_JOYPAD_DOWN:   bit = BUTTON_DOWN; break;
	case RETRO_DEVICE_ID_JOYPAD_LEFT:   bit = BUTTON_LEFT; break;
	case RETRO_DEVICE_ID_JOYPAD_RIGHT:  bit = BUTTON_RIGHT; break;
	case RETRO_DEVICE_ID_JOYPAD_L:      bit = BUTTON_L; break;
	case RETRO_DEVICE_ID_JOYPAD_R:      bit = BUTTON_R; break;
	default: return 0;
	}
	return (s_keys & bit) ? 1 : 0;
}

/* ================= audio ================= */
/* render_audio() drains gpSP's internal ring in 256-frame chunks through this
 * callback (s16 interleaved L,R at sound_frequency). Forward to the LK sink. */
size_t gba_audio_cb(const int16_t *data, size_t frames)
{
	ayaneo_gba_audio_submit((const short *)data, (unsigned)frames);
	return frames;
}

/* ================= save (.sav = gamepak backup) ================= */
void *gba_core_backup_ptr(void)   { return gamepak_backup; }
unsigned gba_core_backup_size(void) { return sizeof(gamepak_backup); }
int gba_core_backup_type(void)    { return (int)backup_type; }

/* ================= save states ================= */
unsigned gba_core_state_size(void) { return GBA_STATE_MEM_SIZE; }
void gba_core_state_save(void *dst) { gba_save_state(dst); }
void gba_core_state_load(const void *src) { gba_load_state(src); }
