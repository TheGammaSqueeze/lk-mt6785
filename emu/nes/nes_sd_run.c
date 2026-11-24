/*
 * nes_sd_run.c - run an NES, Famicom or Famicom Disk System game from the SD card through the
 * loadable armNES core blob (nes_core_abi.h). Built on the Genesis session (genesis_sd_run.c): a
 * dedicated emulation thread, the shared 0x50000000 arena, the game, its battery save and its states
 * on the card, the in-game menu, run ahead, rewind, fast forward, and the panel at the core's own
 * refresh (60.0988 Hz NTSC, 50.007 Hz PAL). A Disk System game gets the BIOS from /bios/disksys.rom;
 * its battery save is the disk itself (what the game wrote to it).
 *
 * The picture goes through the Genesis display path (ayaneo_genesis_show_frame): its aspect modes
 * and LCD filters serve the NES as they are, so the session sets that path's settings from its own.
 */
#include <string.h>
#include "nes_core_abi.h"
#include <platform/ayaneo_volume_input.h>
#include "../gba/sd_fat.h"
#include "../gba/pico_igm.h"
#include "../gba/pico_cheats.h"   /* the game's cheats, the Cheats page */
#include "../gba/pico_input.h"    /* the player's button map, the hotkeys */
#include "../gba/pico_coreopt.h"
#include "../gba/card_writer.h"   /* in-game saves go to the card off the game's thread */
#include "../gba/pico_i18n.h"   /* tr_igm: the in-game menu's words in the language in use */
#include "../gba/gba_sd_save.h"
#include "../gba/pico_state_shot.h"   /* each state's picture and time, for the Suspend Point List */
#include "../ayaneo_rewind.h"
#include <kernel/thread.h>
#include <kernel/event.h>
#include "../gba/emu_stack.h"
#include "../gba/rom_archive.h"   /* RA_ERR_START */

/* ---- LK / driver primitives (externs; no LK headers here) ---- */
extern const struct nes_core_exports *nes_core_load(fat_vol *v);   /* nes_core_loader.c: from the card */
extern void     ayaneo_genesis_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern void     ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_gbc_audio_pause(int on);
extern int      gba_launch_state(char *ext);   /* the game opening carries on from a state, and which (gba_snes_menu.c) */
extern void     ayaneo_display_prepare(void);
extern unsigned gpt4_get_current_tick(void);
extern void     mtk_wdt_restart(void);
extern int      mt_get_gpio_in(unsigned pin);
extern void     ayaneo_joypad_poll(void);
extern unsigned int ayaneo_joypad_dpad(void);
extern void     ayaneo_hud_set(int mode, int speed_x10);   /* mt_disp_drv.c: FF/RW speed badge */
extern void     ayaneo_set_cpu_mhz(unsigned int mhz);      /* reprograms ARM-PLL PCW (not voltage) */
extern unsigned int ayaneo_get_cpu_mhz(void);

/* pad GPIOs (match gba_driver.c / gbc_sd_run.c). Active-low. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#if defined(KONKR_BLOCK)
/* Konkr Pocket Block: every button is a GPIO, active low (the board's gamepad and gpio-keys
 * nodes): A 4, B 5, X 3, Y 6, L1 14, R1 10, L2 15, R2 9, START 7, SELECT 24, the d-pad 25/27/28/26
 * (up/down/left/right), MODE 17; the menu key is the side key on GPIO 22 (the gpio-keys node's aya_a_key) */
#define GPIO_AYA       22      /* return to the selector */
#define GPIO_LB        14
#define GPIO_RB        10
#define GPIO_SELECT    24
#define GPIO_START     7
#define GPIO_B         5       /* held at launch = start fresh (skip resume) */
#define GPIO_A         4
#define GPIO_X         3
#define GPIO_Y         6
#define GPIO_UP        25
#define GPIO_DOWN      27
#define GPIO_LEFT      28
#define GPIO_RIGHT     26
#else
#define GPIO_AYA       86      /* return to the selector */
#define GPIO_LB        92
#define GPIO_RB        81
#if defined(KONKR_KPA)
/* Konkr: the device tree labels match the shell (A 82, B 83, X 84, Y 85, START 90, SELECT 91) */
#define GPIO_SELECT    91
#define GPIO_START     90
#define GPIO_B         83      /* held at launch = start fresh (skip resume) */
#define GPIO_A         82
#define GPIO_X         84
#define GPIO_Y         85
#else
#define GPIO_SELECT    90
#define GPIO_START     91
#define GPIO_B         82      /* held at launch = start fresh (skip resume) */
#define GPIO_A         83
#define GPIO_X         85
#define GPIO_Y         84
#endif
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#endif

/* retro joypad bit positions (RETRO_DEVICE_ID_JOYPAD_*) */
#define RJ_B 0
#define RJ_Y 1
#define RJ_SELECT 2
#define RJ_START 3
#define RJ_UP 4
#define RJ_DOWN 5
#define RJ_LEFT 6
#define RJ_RIGHT 7
#define RJ_A 8
#define RJ_X 9
#define RJ_L 10
#define RJ_R 11

/* The arena (0x50000000, shared: one core runs at a time), inside the mapped [0x50000000,0x54000000).
 * armNES allocates its cartridge (PRG, CHR, work RAM, a disk's sides) and its own buffers from the heap
 * the LK hands it; NES games are small (a few MB at most), so the room is generous. */
#define NES_HEAP_BASE  0x50000000u
#define NES_HEAP_SZ    0x02C00000u        /* 44 MB */
#define NES_ROM_BUF    0x52C00000u        /* the game read from the card here, handed to load() */
#define NES_ROM_CAP    0x00800000u        /* 8 MB, far above the largest NES and multicart images */
#define NES_STATE_BUF  0x53400000u        /* save/suspend state scratch */
#define NES_STATE_CAP  0x00400000u        /* 4 MB (an armNES state is tens of KB) */
#define NES_AHEAD_BUF  0x53800000u        /* run-ahead save/restore scratch (separate from state) */
#define NES_REWIND_MAX_SPD 1536           /* max rewind speed in 256ths (1536 = 6x); floor 256 = 1x */

/* run-ahead CPU clock by depth: pf frames run pf+1 emulations/display + 1 save + 1 load, so
 * escalate the clock with depth (mirrors the snes s_snes_ra_opp). ayaneo_set_cpu_mhz is PLL-only. */
/* Gameplay CPU clock per run-ahead tier (index = pf: 0 Off, 1 Balanced, 2 Responsive, 3 Max). Set per
 * the user's chosen ladder - each tier steps up ~200 MHz for the extra pf+1 emulations run-ahead does.
 * ayaneo_set_cpu_mhz programs the PLL directly (no grid snapping), so these exact values are applied. */
/* RetroAchievements after a frame the game really ran (pico_ra.c; absent on consoles without it) */
/* a RetroAchievements card wants the theme's layer while no menu is open (pico_current.c) */
static int ra_card_wanted(void)
{
	extern int pico_igm_card_wanted(void) __attribute__((weak));
	return pico_igm_card_wanted && pico_igm_card_wanted();
}
/* RetroAchievements' hardcore limits how often the menu may pause the game (pico_ra.c) */
static int ra_may_pause(void)
{
	extern int pico_ra_may_pause(void) __attribute__((weak));
	return !pico_ra_may_pause || pico_ra_may_pause();
}
static void nes_ra_reset_check(void);
static void nes_ra_frame(void)
{
	extern void pico_ra_frame(void) __attribute__((weak));
	if (pico_ra_frame) { pico_ra_frame(); nes_ra_reset_check(); }
}

static const unsigned s_nes_ra_opp[4] = { 999, 1199, 1399, 1599 };

volatile int g_nes_menu_open;         /* gates game input + FF/rewind while the Pico menu is up */
/* the display path's settings (genesis_sd_run.c), set from the NES's own at the session's start */
extern volatile int g_genesis_aspect;
extern volatile unsigned g_genesis_aspect_x1000;
extern volatile int g_genesis_filter;

/* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits. armNES takes A and B as the pad's, X as turbo A and
 * Y as turbo B (by the caps: X next to A, Y next to B), and L3 as the Disk System's disk swap, which
 * the in-game menu presses (s_disk_swap_frames). */
static volatile int s_disk_swap_frames;
static int s_nes_np;   /* this session is a netplay one (pico_netplay.c): both players' pads from it */
static int s_nes_np_guest;   /* the guest plays the host's game: it keeps no save of it */
static unsigned nes_local_pad(void);
unsigned ayaneo_nes_pad_mask(unsigned port)
{
	extern unsigned int pico_np_pad(unsigned int port);
	if (s_nes_np) return pico_np_pad(port);   /* player one and two, the same on both consoles */
	return port == 0 ? nes_local_pad() : 0u;
}

/* this console's pad, nothing while the in-game menu is open: the player's map (pico_input.c, the
 * buttons as they always were unless moved), and the menu's disk swap held a few frames */
static unsigned nes_local_pad(void)
{
	unsigned m;
	if (g_nes_menu_open) return 0;
	m = pico_input_pad();
	if (s_disk_swap_frames > 0) m |= 1u << 14;   /* RETRO_DEVICE_ID_JOYPAD_L3 */
	return m;
}

/* a Famicom Disk System image: the fwNES header ("FDS" 0x1A), or a bare disk side starting with
 * its info block (0x01 "*NINTENDO-HVC*"); the file's name does not tell inside an archive */
static int image_is_disk(const unsigned char *d, unsigned n)
{
	static const char hvc[] = "*NINTENDO-HVC*";
	unsigned i;
	if (n >= 4 && d[0] == 'F' && d[1] == 'D' && d[2] == 'S' && d[3] == 0x1A) return 1;
	if (n < 15 || d[0] != 0x01) return 0;
	for (i = 0; i < 14; i++) if (d[1 + i] != (unsigned char)hvc[i]) return 0;
	return 1;
}

/* the Disk System BIOS (8 KiB) from /bios/disksys.rom, or /disksys.rom; its size, 0 when missing */
#define NES_BIOS_BUF   0x53F00000u
static unsigned nes_load_bios(fat_vol *v)
{
	fat_file f;
	if (fat_open(v, "/bios/disksys.rom", &f) != 0 && fat_open(v, "/disksys.rom", &f) != 0) return 0;
	if (f.size != 8192u) return 0;
	return fat_read(&f, 0, (void *)NES_BIOS_BUF, 8192u) == 8192u ? 8192u : 0;
}

/* debug counters (oem diag) */
volatile unsigned g_nes_dbg_frames;
volatile unsigned g_nes_dbg_w, g_nes_dbg_h, g_nes_dbg_sr;
volatile int      g_nes_dbg_loadrc;
volatile int      g_nes_dbg_disk;   /* bit 0 a disk image, bit 1 the BIOS loaded */
/* rewind cost breakdown (updated while rewinding): the per-step state_load
 * (portable retro_unserialize + system_reset) cost vs the c->run re-emulate cost, the smoothed per-step
 * total, and the achieved reverse speed x10. Answers "is the state restore or the re-emulation the wall?" */
volatile unsigned g_nes_dbg_rw_load_us, g_nes_dbg_rw_run_us, g_nes_dbg_rw_step_us, g_nes_dbg_rw_eff_x10;
volatile unsigned g_nes_dbg_rw_mhz;   /* CPU clock at the time of the measurement */
/* headless benchmark results: uncapped forward throughput + state costs at a fixed
 * safe clock, so autonomous optimization iterations have a stable core-speed metric. */
volatile unsigned g_nes_dbg_bench_fps, g_nes_dbg_bench_us, g_nes_dbg_bench_save_us, g_nes_dbg_bench_load_us;
volatile unsigned g_nes_dbg_bench_mhz, g_nes_dbg_bench_rwx10;   /* clock; implied rewind speed x10 */
volatile unsigned g_nes_dbg_bench_loadfast_us;   /* state_load with the buffer-clear memsets skipped */
volatile unsigned g_nes_dbg_bench_norender_us;   /* per frame with the picture skipped */
volatile unsigned g_nes_dbg_bench_rw6_us, g_nes_dbg_bench_rwok;   /* headless 6x-rewind present cost + valid-frame flag */
volatile int g_nes_commit_unshown = 1;   /* the run-ahead committed frame (never shown) skips its picture */
volatile unsigned g_nes_dbg_bench_crc;   /* FNV-1a of the rendered frame (render-change byte-identity check) */

/* ---- in-game Pico menu (hardware OVL0 L0 overlay over the running game; mirrors the snes menu).
 * g_nes_menu_open (declared above) gates game input + FF/rewind; the game keeps running so
 * settings preview live. Scoped to the settings that need no display-path change yet (Brightness,
 * Volume, CPU Clock, Save-state slots, Reset, Exit); aspect/filter/core-options land with the RSZ
 * display path. ---- */
volatile int g_nes_menu_exit;   /* "Exit Game" -> the run loop breaks back to the selector */
static int   s_msel;
static char  s_mstat[48];
static int   s_save_slot;            /* manual save-state slot 0..2 */
static const struct nes_core_exports *s_menu_c;   /* session context for the menu actions */
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static unsigned             s_menu_rw_payload;

/* hardcore came on during the game: it starts again from its power on, as RetroAchievements asks
 * (pico_ra.c), and rcheevos hears of it */
static void nes_ra_reset_check(void)
{
	extern int pico_ra_reset_wanted(void) __attribute__((weak));
	extern void pico_ra_game_reset(void) __attribute__((weak));
	if (!pico_ra_reset_wanted || !pico_ra_reset_wanted() || !s_menu_c) return;
	s_menu_c->reset();
	if (s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload);
	if (pico_ra_game_reset) pico_ra_game_reset();
}

extern void ayaneo_fill(unsigned int *buf, unsigned int pitch, int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch, int x, int y, int scale, unsigned int argb, const char *s);
extern void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open);
extern void ayaneo_menu_overlay_mark_dirty(void);
extern int  ayaneo_brightness_pct(void);
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  mtk_detect_key(unsigned short hwkey);       /* MTK keypad matrix (hardware volume rocker) */
extern void ayaneo_gbc_osd_show(int kind, int pct);     /* transient OSD bar: 1=volume 2=brightness */
extern void ayaneo_menu_settings_persist(void);         /* persist brightness/volume/... to eMMC+SD */
extern int  pmic_detect_powerkey(void);
extern void mt_power_off(void);
/* rewind ring (platform/mt6785/ayaneo_rewind.c) - declared once so the pointer/unsigned returns are correct
 * (no implicit-int) for both the session loop and the headless self-test. */
extern unsigned int ayaneo_rewind_reset(unsigned int max_payload);
extern int          ayaneo_rewind_ready(void);
extern int          ayaneo_rewind_active(void);
extern unsigned int ayaneo_rewind_count(void);
extern void        *ayaneo_rewind_capture_begin(void);
extern void         ayaneo_rewind_capture_commit(unsigned int size);
extern int          ayaneo_rewind_begin(void);
extern int          ayaneo_rewind_step(void);
extern const void  *ayaneo_rewind_cur(unsigned int *size_out);
extern void         ayaneo_rewind_end(void);
extern int  ayaneo_get_nes_aspect(void); extern void ayaneo_set_nes_aspect(int v);   /* persisted per-core settings */
extern int  ayaneo_get_nes_filter(void); extern void ayaneo_set_nes_filter(int v);
extern int  ayaneo_get_nes_region(void); extern void ayaneo_set_nes_region(int v);
extern int  ayaneo_get_strobe(void);
extern void ayaneo_set_strobe(int v);
extern const char *ayaneo_strobe_name(int b);
extern int  ayaneo_get_overdrive(void); extern void ayaneo_set_overdrive_level(int k);   /* software motion-trail overdrive (shared) */
extern int  ayaneo_get_nes_slot(void);   extern void ayaneo_set_nes_slot(int v);

/* Deferred settings persist (mirrors the snes core): a hardware-rocker OR Pico-menu volume/brightness
 * change marks dirty; the eMMC+SD write is debounced ~0.5 s so holding a key never spams flash. Before
 * this ensures brightness and volume changed in a game are kept. */
static int s_nes_settings_dirty;
#define NES_SETTINGS_DEBOUNCE 30
static void nes_settings_touch(void) { s_nes_settings_dirty = NES_SETTINGS_DEBOUNCE; }
static void nes_settings_tick(void)  { if (s_nes_settings_dirty > 0 && --s_nes_settings_dirty == 0) ayaneo_menu_settings_persist(); }
static void nes_settings_flush(void) { if (s_nes_settings_dirty > 0) { s_nes_settings_dirty = 0; ayaneo_menu_settings_persist(); } }

/* manual CPU-clock grid (ayaneo_set_cpu_mhz reprograms the PLL only, so >boot-Vproc is the user's call) */
static const unsigned s_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static int s_cpu_idx = -1;
static void nes_cpu_step(int dir)
{
	int n = (int)(sizeof s_cpu_opp / sizeof s_cpu_opp[0]), i;
	if (s_cpu_idx < 0) {
		unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u; int best = 0;
		for (i = 0; i < n; i++) { unsigned d = s_cpu_opp[i] > cur ? s_cpu_opp[i] - cur : cur - s_cpu_opp[i]; if (d < bd) { bd = d; best = i; } }
		s_cpu_idx = best;
	}
	s_cpu_idx += dir; if (s_cpu_idx < 0) s_cpu_idx = 0; if (s_cpu_idx >= n) s_cpu_idx = n - 1;
	ayaneo_set_cpu_mhz(s_cpu_opp[s_cpu_idx]);
	{ extern void ayaneo_cpu_manual_set(int core, unsigned mhz); ayaneo_cpu_manual_set(4, s_cpu_opp[s_cpu_idx]); }
}
static void nes_apply_clock(void)
{
	extern unsigned ayaneo_core_clock(int core, unsigned tier_mhz, unsigned tier0_mhz);
	extern int ayaneo_get_preempt_frames(void);
	int pf = ayaneo_get_preempt_frames();
	ayaneo_set_cpu_mhz(ayaneo_core_clock(4, s_nes_ra_opp[(pf >= 0 && pf < 4) ? pf : 0], s_nes_ra_opp[0]));
}

extern int  ayaneo_get_preempt_frames(void);
extern void ayaneo_set_preempt_frames(int v);

enum { GM_BRIGHT, GM_VOLUME, GM_ASPECT, GM_FILTER, GM_REGION, GM_RUNAHEAD, GM_REWIND, GM_CPU, GM_REFRESH, GM_STROBE, GM_SHOWFPS, GM_CORE, GM_BATTERY, GM_RESUME, GM_DISK, GM_SLOT, GM_SAVE, GM_LOAD, GM_RESET, GM_EXIT, GM_FFCAP, GM_COUNT };
/* Overdrive is On/Off: On applies the tuned halo-free profile (ayaneo_set_overdrive_level). The exact
 * kup/kdn/thr/cap are still pokeable live via `oem od:` for tuning. */
#define OD_MENU_MAX 1
static const char *gen_aspect_name(int a)
{
	switch (a) { case 1: return "Core"; case 2: return "Stretch"; case 3: return "4:3"; case 4: return "Square"; case 5: return "16:9"; default: return "Pixel"; }
}
/* the menu walks the modes in this order (the stored value is the mode number) */
static const unsigned char gen_aspect_order[] = { 0, 3, 1, 4, 5, 2 };
static int gen_aspect_step(int a, int dir)
{
	int i, n = (int)sizeof gen_aspect_order;
	for (i = 0; i < n; i++) if (gen_aspect_order[i] == a) break;
	if (i >= n) i = 0;
	i = (i + dir + n) % n;
	return gen_aspect_order[i];
}
static const char *gen_filter_name(int f) { return f == 5 ? "LCD3x" : f == 4 ? "LCD1x" : f == 3 ? "Grid+" : f == 2 ? "Grid" : f == 1 ? "Scanlines" : "Off"; }
static const char *gen_ra_name(int pf) { return pf == 3 ? "Max" : pf == 2 ? "Responsive" : pf == 1 ? "Balanced" : "Off"; }

/* Region -> armnes_region, which armNES takes when the game loads: the session sets it first, and a
 * change in the menu applies from the next game start. */
static int s_nes_region;   /* 0 Auto, 1 NTSC, 2 PAL, 3 Dendy */
static const char *gen_region_name(int r) { return r == 3 ? "Dendy" : r == 2 ? "PAL" : r == 1 ? "NTSC" : "Auto"; }
static const char *gen_region_opt(int r)  { return r == 3 ? "Dendy" : r == 2 ? "PAL" : r == 1 ? "NTSC" : "Auto"; }
extern unsigned int ayaneo_dsi_refresh_milli(void);
extern unsigned int ayaneo_dsi_core_refresh_milli(void);   /* panel refresh in milli-Hz (ties into LCM work) */
extern void ayaneo_dsi_set_vfp(unsigned int vfp);     /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);
extern unsigned int ayaneo_dsi_set_fps_milli(unsigned int fps_milli);  /* set panel refresh to target fps (milli-Hz), returns vfp (ddp_dsi.c) */
/* The panel at the core's own rate (ayaneo_dsi_vfp_for_fps, the board's timing): NTSC 60.0988 Hz, PAL
 * and Dendy 50.007 Hz, so the vsync locked present paces the game exactly; the menu's rate comes back on
 * exit. */
extern unsigned int ayaneo_dsi_vfp_for_fps(unsigned int fps_milli);   /* ddp_dsi.c: board timing */
extern unsigned int ayaneo_dsi_default_vfp(void);
#define NES_VFP         ayaneo_dsi_vfp_for_fps(60099u)   /* NTSC, if the core reports no rate */
#define NES_DEFAULT_VFP ayaneo_dsi_default_vfp()
volatile unsigned g_nes_dbg_vfp;   /* live DSI vfp during the session (validates the switch) */

/* Map a core frame rate (milli-Hz) to the panel vfp that makes the vsync-locked present pace exactly
 * that rate. vtotal = 976 + vfp and refresh_milli = 59667000 / vtotal (shipped-build line const,
 * matches ayaneo_dsi_refresh_milli), so vfp = 59667000/fps_milli - 976, rounded. Guards a zero/insane
 * fps by falling back to the NTSC default. The DSI helper clamps the final vfp to its safe porch range. */
static unsigned nes_vfp_for_fps(unsigned fps_milli)
{
	if (fps_milli < 40000u || fps_milli > 65000u) return NES_VFP;   /* implausible -> NTSC */
	return ayaneo_dsi_vfp_for_fps(fps_milli);
}

static char *mput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mputu(char *p, unsigned v) { char t[12]; int n = 0; if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; } while (n) *p++ = t[--n]; return p; }

static const char *gm_label(int i) { switch (i) {
	case GM_BRIGHT: return "Brightness"; case GM_VOLUME: return "Volume";
	case GM_ASPECT: return "Aspect Ratio"; case GM_FILTER: return "LCD Filter";
	case GM_REGION: return "Region"; case GM_REFRESH: return "Refresh Rate";
	case GM_RUNAHEAD: return "Run-Ahead"; case GM_REWIND: return "Rewind"; case GM_FFCAP: return "Fast Forward Speed"; case GM_CPU: return "CPU Clock";
	case GM_STROBE: return "BFI"; case GM_RESUME: return "Resume on Power On";
	case GM_SHOWFPS: return "Show FPS"; case GM_BATTERY: return "Battery";
	case GM_CORE: return "Core Settings";
	case GM_SLOT: return "Save Slot"; case GM_SAVE: return "Save State"; case GM_LOAD: return "Load State";
	case GM_DISK: return "Swap Disk Side";
	case GM_RESET: return "Reset Game"; case GM_EXIT: return "Exit Game"; } return ""; }

static const char *gm_value(int i, char *buf) { char *p = buf;
	switch (i) {
	case GM_BRIGHT: p = mputu(p, (unsigned)ayaneo_brightness_pct()); p = mput(p, "%"); break;
	case GM_VOLUME: p = mputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mput(p, "%"); break;
	case GM_ASPECT: p = mput(p, gen_aspect_name(g_genesis_aspect)); break;
	case GM_FILTER: p = mput(p, gen_filter_name(g_genesis_filter)); break;
	case GM_REGION: p = mput(p, gen_region_name(s_nes_region)); break;
	case GM_REFRESH: { unsigned mhz = ayaneo_dsi_core_refresh_milli();   /* read-only: XX.X Hz per frame */
		p = mputu(p, mhz / 1000u); *p++ = '.'; *p++ = (char)('0' + (mhz % 1000u) / 100u); p = mput(p, " Hz"); } break;
	case GM_STROBE: { int b = ayaneo_get_strobe(); p = mput(p, ayaneo_strobe_name(b)); } break;
	case GM_REWIND: { extern int ayaneo_get_rewind(void); p = mput(p, ayaneo_get_rewind() ? "On" : "Off"); } break;
	case GM_FFCAP:    { extern const char *ayaneo_ff_cap_name(char *buf); char t[8]; p = mput(p, ayaneo_ff_cap_name(t)); } break;
	case GM_SHOWFPS: { extern int ayaneo_get_show_fps(void); p = mput(p, ayaneo_get_show_fps() ? "On" : "Off"); } break;
	case GM_BATTERY: { extern int ayaneo_gauge_pct(void); extern int ayaneo_gauge_time_str(char *out, int n); extern int upmu_is_chr_det(void);
		char ts[64]; p = mputu(p, (unsigned)ayaneo_gauge_pct()); p = mput(p, "%");
		if (upmu_is_chr_det()) { p = mput(p, " "); p = mput(p, tr_igm("Charging")); }
		if (ayaneo_gauge_time_str(ts, sizeof ts)) { p = mput(p, " "); p = mput(p, ts); } } break;
	case GM_RESUME: { extern int ayaneo_get_resume_on_boot(void); p = mput(p, ayaneo_get_resume_on_boot() ? "On" : "Off"); } break;
	case GM_RUNAHEAD: p = mput(p, gen_ra_name(ayaneo_get_preempt_frames())); break;
	case GM_CPU:    p = mputu(p, ayaneo_get_cpu_mhz()); p = mput(p, " MHz"); break;
	case GM_SLOT:   p = mputu(p, (unsigned)s_save_slot); break;
	case GM_CORE:   p = mput(p, pico_coreopt_n() ? ">" : "None"); break;
	case GM_DISK: p = mput(p, (g_nes_dbg_disk & 1) ? "[A]" : "-"); break;
	case GM_SAVE: case GM_LOAD: case GM_RESET: case GM_EXIT: p = mput(p, "[A]"); break;
	} *p = 0; return buf; }

static void nes_slot_ext(char *e) { e[0] = 's'; e[1] = 't'; e[2] = (char)('0' + s_save_slot); e[3] = 0; }

/* returns 1 to close the menu (Reset/Exit) */
static int gm_change(int i, int dir, int act)
{
	s_mstat[0] = 0;
	/* netplay: loading a state or resetting would change the game on this console only and the two
	 * would part; the disk swap is fine (it goes to both consoles as a pad press) */
	if (s_nes_np && act && i == GM_RESET) {	/* the host's, on both consoles at the same frame */
		extern int pico_np_is_host(void);
		extern void pico_np_ask_reset(void);
		memset(s_mstat, 0, sizeof s_mstat);
		if (!pico_np_is_host()) { mput(s_mstat, "Only the host can reset the game"); return 0; }
		pico_np_ask_reset();
		return 1;
	}
	if (s_nes_np && act && i == GM_LOAD) {
		memset(s_mstat, 0, sizeof s_mstat);
		mput(s_mstat, "Not during netplay");
		return 0;
	}
	switch (i) {
	case GM_BRIGHT: if (dir) { ayaneo_brightness_step(dir); nes_settings_touch(); } break;
	case GM_VOLUME: if (dir) { ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); nes_settings_touch(); } break;
	case GM_ASPECT: if (dir) { g_genesis_aspect = gen_aspect_step(g_genesis_aspect, dir);   /* live preview */
		ayaneo_set_nes_aspect(g_genesis_aspect); nes_settings_touch(); } break;
	case GM_FILTER: if (dir) { g_genesis_filter = (g_genesis_filter + dir + 6) % 6;
		ayaneo_set_nes_filter(g_genesis_filter); nes_apply_clock(); nes_settings_touch(); } break;
	case GM_REGION: if (dir) { s_nes_region = (s_nes_region + dir + 4) % 4;
		if (s_menu_c && s_menu_c->set_option)
			s_menu_c->set_option("armnes_region", gen_region_opt(s_nes_region));
		ayaneo_set_nes_region(s_nes_region); nes_settings_touch();
		/* armNES takes the region when a game loads (its timing and frame rate are set then) */
		mput(s_mstat, "Takes effect at the next game start"); } break;
	case GM_REFRESH: break;   /* read-only display (panel Hz); no adjust */
	case GM_STROBE: if (dir) { ayaneo_set_strobe((ayaneo_get_strobe() + dir + 4) % 4); nes_settings_touch(); } break;
	case GM_REWIND: if (dir || act) { extern int ayaneo_get_rewind(void); extern void ayaneo_set_rewind(int v);
			ayaneo_set_rewind(!ayaneo_get_rewind()); nes_settings_touch(); } break;
	case GM_FFCAP:  if (dir || act) { extern void ayaneo_step_ff_cap(int dir);
			ayaneo_step_ff_cap(dir ? dir : 1); nes_settings_touch(); } break;
	case GM_SHOWFPS: if (dir || act) { extern int ayaneo_get_show_fps(void); extern void ayaneo_set_show_fps(int v);
			ayaneo_set_show_fps(!ayaneo_get_show_fps()); nes_settings_touch(); } break;
	case GM_BATTERY: break;   /* read-only */
	case GM_CORE: if (act || dir > 0) pico_coreopt_page_open(); break;   /* the core's own options */
	case GM_RESUME: if (dir) { extern int ayaneo_get_resume_on_boot(void); extern void ayaneo_set_resume_on_boot(int v);
			ayaneo_set_resume_on_boot(!ayaneo_get_resume_on_boot()); nes_settings_touch(); } break;
	case GM_RUNAHEAD: if (dir) { int pf = (ayaneo_get_preempt_frames() + dir + 4) % 4;
		ayaneo_set_preempt_frames(pf); nes_apply_clock(); s_cpu_idx = -1;
		nes_settings_touch(); } break;   /* persist run-ahead (shared preempt-frames blob) */
	case GM_CPU:    if (dir) { nes_cpu_step(dir); nes_settings_touch(); } break;   /* persisted per core */
	case GM_SLOT:   if (dir) { s_save_slot = (s_save_slot + dir + 3) % 3;
		ayaneo_set_nes_slot(s_save_slot); nes_settings_touch(); } break;
	case GM_SAVE: if (act) {
		unsigned char *st = (unsigned char *)NES_STATE_BUF; unsigned ssz = s_menu_c->state_size();
		char ext[4]; int ok; nes_slot_ext(ext);
		ok = ssz && ssz <= NES_STATE_CAP && s_menu_c->state_save(st, ssz) == 0;
		/* on the card from the writer's thread: the game and its sound go on (the note once it is there) */
		if (ok) {
			char nm[288];
			gba_sd_state_name(s_menu_rom->name, ext, nm, sizeof nm);
			card_writer_put(s_menu_vol, "/states/nes", nm, st, ssz, s_mstat, sizeof s_mstat, "State saved", "Save failed");
			pico_shot_save(s_menu_vol, "/states/nes", s_menu_rom->name, ext, 1);
			{ extern void pico_ra_state_saved(fat_vol *v, const char *d, const char *r, const char *e) __attribute__((weak));
				if (pico_ra_state_saved) pico_ra_state_saved(s_menu_vol, "/states/nes", s_menu_rom->name, ext); }   /* its RetroAchievements progress beside it */
		} else mput(s_mstat, "Save failed"); } break;
	case GM_LOAD: if (act) { extern int pico_ra_hardcore(void) __attribute__((weak)); if (pico_ra_hardcore && pico_ra_hardcore()) { mput(s_mstat, "Load state is off in hardcore"); break; } }
		if (act) {
		unsigned char *st = (unsigned char *)NES_STATE_BUF; char ext[4]; unsigned n; int ok;
		nes_slot_ext(ext);
		n = gba_sd_read_named(s_menu_vol, "/states/nes", s_menu_rom->name, ext, st, NES_STATE_CAP);
		ok = (n && s_menu_c->state_load(st, n) == 0);
		if (ok && s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload);   /* loaded state breaks the rewind timeline */
		if (ok) { extern void pico_ra_state_loaded_from(fat_vol *v, const char *d, const char *r, const char *e) __attribute__((weak));
				if (pico_ra_state_loaded_from) pico_ra_state_loaded_from(s_menu_vol, "/states/nes", s_menu_rom->name, ext); }   /* and RetroAchievements' progress */
		mput(s_mstat, ok ? "State loaded" : "No save state"); } break;
	case GM_DISK: if (act && (g_nes_dbg_disk & 1)) {
		/* armNES takes L3 as the drive's eject: the disk out, then the next side in; held for a
		 * few frames once the menu has closed, so the game sees it */
		s_disk_swap_frames = 20; mput(s_mstat, "Disk side changed"); return 1; } break;
	case GM_RESET: if (act) { s_menu_c->reset(); if (s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload); { extern void pico_ra_game_reset(void) __attribute__((weak)); if (pico_ra_game_reset) pico_ra_game_reset(); } return 1; } break;
	case GM_EXIT:  if (act) { g_nes_menu_exit = 1; return 1; } break;
	}
	return 0;
}

int nes_menu_open(void) { return g_nes_menu_open; }

/* the Current theme's in-game menu (pico_igm.h): the rows in its pages, the handlers above */
static const struct igm_row k_nes_igm_rows[] = {
	{ GM_SAVE, IGM_TOP, IGM_ACTION }, { GM_LOAD, IGM_TOP, IGM_ACTION },
	{ IGM_DISPLAY, IGM_TOP, IGM_LINK }, { IGM_SOUND, IGM_TOP, IGM_LINK },
	{ IGM_PLAY, IGM_TOP, IGM_LINK }, { IGM_SYSTEM, IGM_TOP, IGM_LINK },
	{ GM_RESET, IGM_TOP, IGM_ACTION }, { GM_EXIT, IGM_TOP, IGM_ACTION },
	{ GM_BRIGHT, IGM_DISPLAY, IGM_VALUE }, { GM_ASPECT, IGM_DISPLAY, IGM_VALUE },
	{ GM_FILTER, IGM_DISPLAY, IGM_VALUE }, { GM_STROBE, IGM_DISPLAY, IGM_VALUE },
	{ GM_SHOWFPS, IGM_DISPLAY, IGM_VALUE },
	{ GM_VOLUME, IGM_SOUND, IGM_VALUE },
	{ GM_CPU, IGM_PLAY, IGM_VALUE }, { GM_RUNAHEAD, IGM_PLAY, IGM_VALUE }, { GM_REWIND, IGM_PLAY, IGM_VALUE },
	{ GM_FFCAP, IGM_PLAY, IGM_VALUE },
	{ GM_REGION, IGM_PLAY, IGM_VALUE }, { GM_REFRESH, IGM_PLAY, IGM_INFO }, { IGM_CORE, IGM_PLAY, IGM_LINK },
	{ GM_RESUME, IGM_SYSTEM, IGM_VALUE }, { GM_BATTERY, IGM_SYSTEM, IGM_INFO },
	{ GM_DISK, IGM_TOP, IGM_ACTION },   /* last: left off for a cartridge (the session sets nrows) */
};
static struct igm_core s_nes_igm = {
	k_nes_igm_rows, (int)(sizeof k_nes_igm_rows / sizeof k_nes_igm_rows[0]),
	GM_SLOT, GM_SAVE, GM_LOAD, GM_RESET, GM_EXIT,
	gm_label, gm_value, gm_change, s_mstat,
	pico_coreopt_n, pico_coreopt_label, pico_coreopt_value, pico_coreopt_change,	/* the core's own */
	GBA_CONSOLE_NES, 0,
};
static int s_nes_igm_on;   /* this game's menu is the Current theme's */

extern int ayaneo_pico_layout(int count, int H, int sel, int *top, int *rowH, int *panelH);
extern void ayaneo_pico_bar(unsigned int *buf, unsigned int pitch, int x, int w, int y, int rowH);
extern void ayaneo_pico_more(unsigned int *buf, unsigned int pitch, int px, int py, int panelW, int panelH, int up, int down);
void nes_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int vis, top;
	int rowH = 38, panelW = 520, panelH = 60 + GM_COUNT * rowH + 34;
	int px, py, x, y, i;
	if (pico_coreopt_page_is_open()) { pico_coreopt_page_paint(buf, pitch, W, H); return; }
	/* Fit the panel to the screen (ayaneo_pico_layout): 38 px rows when everything fits, down to
	 * 32 px on the Konkr's 640 px height, and the list scrolls beyond that. */
	{ static int s_top; vis = ayaneo_pico_layout(GM_COUNT, (int)H, s_msel, &s_top, &rowH, &panelH); top = s_top; }
	px = ((int)W - panelW) / 2; py = ((int)H - panelH) / 2; x = px + 28; y = py + 60;
	char val[48];
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xD010141Cu);   /* translucent: the game shows through (hardware blend) */
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 20, 3, 0xFFFFFFFFu, "GammaOS Pico");
	for (i = top; i < GM_COUNT && i < top + vis; i++, y += rowH) {
		unsigned int fg = (i == s_msel) ? 0xFF101018u : 0xFFC8D0E0u; int vw;
		if (i == s_msel) ayaneo_pico_bar(buf, pitch, px + 10, panelW - 20, y, rowH);
		ayaneo_text(buf, pitch, x, y, 2, fg, gm_label(i));
		gm_value(i, val); for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	ayaneo_pico_more(buf, pitch, px, py, panelW, panelH, top > 0, top + vis < GM_COUNT);
	if (s_mstat[0]) ayaneo_text(buf, pitch, x, py + panelH - 32, 2, 0xFF80E080u, s_mstat);
	ayaneo_text(buf, pitch, x, py + panelH - 14, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

/* Hardware volume rocker (mirrors snes_poll_volume / gba poll_volume so every core behaves the same):
 * VolUp = mtk_detect_key(0x11), VolDown = 0x00, edge-detected (one press = one step). Holding SELECT
 * turns the rocker into a brightness control. Runs whether or not the Pico menu is open. Debounced to
 * disk via nes_settings_touch. */
static void nes_poll_volume(void)
{
	int sel = PRESSED(GPIO_SELECT);
	int dir = ayaneo_volume_dir();
	if (!dir) return;
	if (sel) {
		ayaneo_brightness_step(dir);
		ayaneo_gbc_osd_show(2, ayaneo_brightness_pct());   /* 2 = brightness */
	} else {
		int v = ayaneo_gbc_audio_get_volume() + dir * 5;
		if (v < 0) v = 0; if (v > 100) v = 100;
		ayaneo_gbc_audio_set_volume(v);
		ayaneo_gbc_osd_show(1, v);                         /* 1 = volume */
	}
	nes_settings_touch();
}

/* Power key during gameplay (mirrors gba check_power/save_and_poweroff so power behaves the same on
 * every core): arm on release, fire on the next press. On press: silence audio, write the cartridge
 * battery + a suspend state so the next launch resumes, flush pending settings, then power off. Uses
 * the session context stashed in s_menu_* by nes_session_body. */
static void nes_power_check(void)
{
	extern int ayaneo_power_hold(int menu_open, int use_overlay);
	if (!ayaneo_power_hold(g_nes_menu_open, 1)) return;
	ayaneo_gbc_audio_pause(1);   /* silent through the card writes and the power off */
	{ extern void pico_np_stop(void); if (s_nes_np) pico_np_stop(); }   /* tell the other console */
	if (s_menu_c && s_menu_vol && s_menu_rom && !s_nes_np_guest) {
		const struct nes_core_exports *c = s_menu_c;
		unsigned ssz;
		if (c->sram_ptr() && c->sram_size())
			gba_sd_write_sav(s_menu_vol, s_menu_rom->name, c->sram_ptr(), c->sram_size());
		ssz = c->state_size();
		if (ssz && ssz <= NES_STATE_CAP && c->state_save((void *)NES_STATE_BUF, ssz) == 0) {
			gba_sd_write_named(s_menu_vol, "/states/nes", s_menu_rom->name, "sus",
					   (const unsigned char *)NES_STATE_BUF, ssz);
			pico_shot_save(s_menu_vol, "/states/nes", s_menu_rom->name, "sus", 0);
		}
	}
	{ extern void ayaneo_resume_mark(const char *name); ayaneo_resume_mark(s_menu_rom->name); s_nes_settings_dirty = 1; }
	nes_settings_flush();
	{ extern void ayaneo_settings_persist_flush(void); ayaneo_settings_persist_flush(); }
	{ extern void ayaneo_power_off_now(void); ayaneo_power_off_now(); }   /* no return */
}

/* Run ahead only as deep as the frame leaves room for: the committed frame, the look-ahead (its
 * save, frames, picture and load) and the present together must fit a refresh, or the panel is
 * shown every other one (a heavy game at a deep setting). The cap drops a level when a frame went
 * over, and comes back up when a whole committed frame more would still fit, as the GBA core's
 * preempt_adapt does. */
static int s_nes_pf_cap = 3;
volatile int g_nes_dbg_pf_cap = 3;
volatile unsigned g_nes_dbg_pf_us[3];   /* oem nes: what the cap was decided on, committed / ahead / blit us */
static void nes_pf_adapt(unsigned int committed_us, unsigned int ahead_us)
{
	extern volatile unsigned int g_dbg_blit_us;
	unsigned int busy = committed_us + ahead_us + g_dbg_blit_us;
	g_nes_dbg_pf_us[0] = committed_us; g_nes_dbg_pf_us[1] = ahead_us; g_nes_dbg_pf_us[2] = g_dbg_blit_us;
	unsigned int step = committed_us ? committed_us : 2500u;
	if (busy > 15500u) {
		if (s_nes_pf_cap > 0) s_nes_pf_cap--;
	} else if (busy + step < 15500u) {
		if (s_nes_pf_cap < 3) s_nes_pf_cap++;
	}
	g_nes_dbg_pf_cap = s_nes_pf_cap;
}

static void nes_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct nes_core_exports *c = nes_core_load(vol);
	unsigned romsz, sr = 44100, ssz, rw_payload, saved_mhz;
	int aya_prev = 0, aya_hold = 0, rw_acc = 0, reset_hold = 0;
	int up_h = 0, dn_h = 0, lt_h = 0, rt_h = 0;             /* menu nav auto-repeat hold counters */
	int up_p = 0, dn_p = 0, lt_p = 0, rt_p = 0, a_p = 0, b_p = 0;
	struct nes_frame fr;

	if (!c) { gba_rom_set_error(RA_ERR_START); return; }   /* blob load failed (g_nes_dbg_loaderr set by the loader) */

	c->heap_init((void *)NES_HEAP_BASE, NES_HEAP_SZ);
	c->init();
	/* the core's own options (it declared them in init): the choices saved for this system, read
	 * by the core while the game loads */
	if (c->version >= 2 && c->opt_count && c->opt_info && c->set_option) {
		static struct pico_coreopt_core copt;
		copt.count = c->opt_count; copt.info = c->opt_info; copt.set = c->set_option;
		pico_coreopt_begin(vol, "nes", &copt);
	} else
		pico_coreopt_begin(vol, "nes", 0);

	romsz = gba_sd_load_rom(vol, rom, (unsigned char *)NES_ROM_BUF, NES_ROM_CAP);
	g_nes_dbg_loadrc = -2;
	if (!romsz) { ayaneo_menu_audio_silence(); return; }
	s_nes_region = ayaneo_get_nes_region();   /* before the load: armNES sets its timing from it then */
	if (c->set_option)
		c->set_option("armnes_region", gen_region_opt(s_nes_region));
	{	/* a Disk System game needs the BIOS; a cartridge takes none */
		int disk = image_is_disk((const unsigned char *)NES_ROM_BUF, romsz);
		unsigned bsz = disk ? nes_load_bios(vol) : 0u;
		g_nes_dbg_disk = disk | (bsz ? 2 : 0);
		if (disk && !bsz) {
			g_nes_dbg_loadrc = -3; gba_rom_set_error(RA_ERR_NOBIOS); ayaneo_menu_audio_silence(); return;
		}
		if (c->load((const void *)NES_ROM_BUF, romsz, bsz ? (const void *)NES_BIOS_BUF : 0, bsz, disk) != 0) {
			g_nes_dbg_loadrc = -1; gba_rom_set_error(RA_ERR_START); ayaneo_menu_audio_silence(); return;
		}
	}
	g_nes_dbg_loadrc = 0;

	/* cartridge battery (SRAM/EEPROM) from /saves */
	if (c->sram_ptr() && c->sram_size())
		gba_sd_load_sav(vol, rom->name, (unsigned char *)c->sram_ptr(), c->sram_size());

	/* suspend-resume: reload the auto-saved state unless B is held at launch (fresh start) or
	 * Resume on Game Open is off */
	char open_ext[4];   /* the state picked in the Suspend Point List, or the suspend point */
	if (!PRESSED(GPIO_B) && gba_launch_state(open_ext)) {
		ssz = c->state_size();
		if (ssz && ssz <= NES_STATE_CAP) {
			unsigned n = gba_sd_read_named(vol, "/states/nes", rom->name, open_ext,
						       (unsigned char *)NES_STATE_BUF, NES_STATE_CAP);
			if (n) c->state_load((const void *)NES_STATE_BUF, n);
		}
	}

	/* A netplay session: both consoles start from the host's state (its battery RAM and any resume
	 * with it), the guest's replaced by it; then the first frame runs on both together. */
	s_nes_np = s_nes_np_guest = 0;
	{
		extern int pico_np_armed(unsigned int *delay);
		extern int pico_np_begin(int host, unsigned int delay, void *state, unsigned int size,
					 unsigned int cap, unsigned int *got);
		unsigned int delay = 2, got = 0;
		int host = pico_np_armed(&delay);
		if (host >= 0) {
			ssz = c->state_size();
			if (ssz && ssz <= NES_STATE_CAP && (!host || c->state_save((void *)NES_STATE_BUF, ssz) == 0) &&
			    pico_np_begin(host, delay, (void *)NES_STATE_BUF, ssz, NES_STATE_CAP, &got) == 0 &&
			    (host || c->state_load((const void *)NES_STATE_BUF, got) == 0)) {
				s_nes_np = 1;
				s_nes_np_guest = !host;
			} else {
				g_nes_dbg_loadrc = -4;   /* the other console did not join: no game */
				gba_rom_set_error(RA_ERR_START);
				ayaneo_menu_audio_silence();
				return;
			}
		}
	}

	/* cheats: the game's, those on put in force (none in netplay: the other console has none) */
	{
		struct pico_cheat_core cc = { 0, 0 };
		if (!s_nes_np && c->version >= NES_CORE_ABI_V4) { cc.reset = c->cheat_reset; cc.add = c->cheat_add; }
		pico_cheats_game(vol, rom->type, rom->name, (const void *)NES_ROM_BUF, romsz, &cc);
		/* the buttons: the game's map, the menu's save, load and restart for the hotkeys, and its
		 * disk swap for a Disk System game */
		pico_input_game(vol, rom->type, rom->name, &s_nes_igm, (g_nes_dbg_disk & 1) ? GM_DISK : -1);
		pico_input_netplay(s_nes_np);
	}

	/* RetroAchievements: the game's set, with its memory (not in netplay) */
	{
		extern void pico_ra_core_ready(const void *rom, unsigned size, const struct pico_core_memory *mem)
			__attribute__((weak));
		if (pico_ra_core_ready && !s_nes_np && c->version >= NES_CORE_ABI_V3 && c->get_memory) {
			struct pico_core_memory mem;
			c->get_memory(&mem);
			pico_ra_core_ready((const void *)NES_ROM_BUF, romsz, &mem);
		}
	}

	c->av_info(0, 0, 0, 0, &sr);
	if (!sr) sr = 44100;
	g_nes_dbg_sr = sr;
	if (c->aspect_x1000) g_genesis_aspect_x1000 = c->aspect_x1000();   /* Fit target (~4:3) */

	/* Arm the rewind ring: the whole serialized state each frame (retro_serialize); the high DRAM delta
	 * ring keeps only what changed from one to the next, so a frame of change is small. Off if the state
	 * is absurdly large or the ring's memory is not there. */
	rw_payload = c->state_size();
	if (rw_payload && rw_payload <= 0x00400000u) ayaneo_rewind_reset(rw_payload);
	else rw_payload = 0;

	/* Raise the CPU clock for gameplay, escalated by the run-ahead depth (pf frames run pf+1
	 * emulations + a save + a load per display). The menu holds a lower idle clock; restored on
	 * exit. ayaneo_set_cpu_mhz reprograms only the PLL (boot Vproc), like the SNES gameplay tiers. */
	saved_mhz = ayaneo_get_cpu_mhz();
	nes_apply_clock();   /* ladder by run-ahead tier, manual choice, filter bump */

	/* menu context (actions reference the session core + save target) */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom; s_menu_rw_payload = rw_payload;
	s_nes_igm.nrows = (int)(sizeof k_nes_igm_rows / sizeof k_nes_igm_rows[0]) - ((g_nes_dbg_disk & 1) ? 0 : 1);
	s_disk_swap_frames = 0;
	s_nes_pf_cap = 3;
	g_nes_menu_open = 0; g_nes_menu_exit = 0; s_msel = 0; s_mstat[0] = 0; s_cpu_idx = -1; s_nes_igm_on = 0;

	/* Restore persisted per-core settings (aspect, filter, slot; the region went in before the load)
	 * so a launch honours the player's last picks. Run-ahead uses the shared preempt-frames;
	 * volume and brightness the shared blob. */
	g_genesis_aspect = ayaneo_get_nes_aspect();
	g_genesis_filter = ayaneo_get_nes_filter();
	s_save_slot      = ayaneo_get_nes_slot();

	/* Switch the panel to the core's own rate (NTSC 60.0988 Hz, PAL and Dendy 50.007 Hz), read from
	 * the core after the load, so the vsync locked present paces the game exactly; the menu's rate
	 * comes back on exit. */
	{
		unsigned fps = (c->fps_milli) ? c->fps_milli() : 0u;
		ayaneo_dsi_set_vfp(fps ? nes_vfp_for_fps(fps) : NES_VFP);
	}
	g_nes_dbg_vfp = ayaneo_dsi_get_vfp();

	/* Launch punch-hole (matches snes/gba/gbc): the menu handed off with the frozen carousel still on
	 * screen (GBA_PUNCH_SNAP_PA = 0x54000000) and gba_punch_ready set. Run a few frames so real
	 * gameplay is on screen (not a black/boot frame), pre-render it full-screen with the live aspect
	 * geometry, then grow a circle from 0 to the panel diagonal over the frozen menu - the carousel is
	 * visibly eaten by the expanding gameplay instead of a hard cut.  */
	{
		extern int  gba_punch_ready;
		extern int  ayaneo_punch_steps(void);
		extern void ayaneo_genesis_punch_prerender(const unsigned short *, unsigned, unsigned, unsigned);
		extern void ayaneo_gba_punch_frame_pre(const unsigned int *, int);
		if (gba_punch_ready) {
			int i, w;
			struct nes_frame pf; pf.video = 0; pf.width = 0; pf.height = 0;
			gba_punch_ready = 0;
			for (w = 0; w < 20; w++) { c->run(&pf); if (pf.video && pf.width && w >= 3) break; }
			if (c->aspect_x1000) g_genesis_aspect_x1000 = c->aspect_x1000();
			if (pf.video && pf.width && pf.height) {
				ayaneo_genesis_punch_prerender((const unsigned short *)pf.video, pf.width, pf.height,
							       pf.pitch / 2u);
				int steps = ayaneo_punch_steps();
				for (i = 1; i <= steps; i++) {
					int r = 820 * i / steps; if (r < 1) r = 1;
					ayaneo_gba_punch_frame_pre((const unsigned int *)0x54000000u, r);
					mtk_wdt_restart();
				}
			} else ayaneo_gbc_audio_pause(0);   /* no opening: the menu's hold on the sound ends here */
		}
	}

	int ff_prev = 0;   /* last frame ran a fast forward batch */
	for (;;) {
		extern int ayaneo_joypad_ff_level(void);
		extern int ayaneo_joypad_rewind_level(void);
		extern void ayaneo_audio_reverse_flip(void);
		extern volatile unsigned int g_dbg_blit_us;
		static short s_nes_audrev[2048 * 2];   /* reversed+decimated rewind audio scratch */
		unsigned int em0, fe;
		int ff, rw;

		{ extern void ayaneo_frame_delay(void); ayaneo_frame_delay(); }   /* read the buttons as late as the frame can still make its refresh */
		ayaneo_joypad_poll();
		if (s_disk_swap_frames > 0 && !g_nes_menu_open) s_disk_swap_frames--;   /* the menu's disk swap, held a few frames */
		nes_poll_volume();     /* hardware volume rocker + SELECT-brightness (parity with all cores) */
		nes_settings_tick();   /* debounced volume/brightness persist */
		nes_power_check();      /* power key -> save cartridge + suspend, then power off */

		/* Refresh the Pico menu as a hardware overlay (OVL0 L0) over the running game, or disable it
		 * when closed - the game keeps running underneath so settings preview live (mirrors snes). */
		if (s_nes_igm_on || (!g_nes_menu_open && ra_card_wanted())) pico_igm_overlay(g_nes_menu_open);	/* or a RetroAchievements card on its layer */
		else ayaneo_menu_overlay(nes_menu_paint, g_nes_menu_open);

		/* Rewind (left trigger): walk the delta ring backward and re-render instead of advancing.
		 * Press depth sets a smooth 1x..6x speed via a fractional accumulator; each stepped-back
		 * frame's audio is reversed + decimated by `steps` and submitted (game plays backwards). On
		 * release the ring commits the rewound point as the new head. Mirrors the snes rewind block.
		 * Gated off while the menu is open. */
		rw = (g_nes_menu_open || s_nes_np) ? 0 : ayaneo_joypad_rewind_level();   /* netplay: no rewind */
		if (rw > 0 && ayaneo_rewind_ready() && ayaneo_rewind_count() > 0) {
			extern int priamry_display_wait_for_vsync(void);
			int spd = 256 + (rw * (NES_REWIND_MAX_SPD - 256)) / 255;   /* 256(1x)..MAX_SPD(6x) */
			unsigned int sz; const void *st = 0; int k, steps, walked = 0;
			unsigned int t_load = 0, t_run = 0, ta, tb, tc;
			if (!ayaneo_rewind_active()) { ayaneo_rewind_begin(); rw_acc = 0; ayaneo_audio_reverse_flip(); }
			rw_acc += spd; steps = rw_acc >> 8; rw_acc &= 255;
			if (steps < 1) steps = 1;
			ayaneo_hud_set(2, spd * 10 / 256);   /* achieved == requested: cost is now speed-independent */

			/* KEY optimization: walk the delta ring back `steps` records FIRST - each ayaneo_rewind_step
			 * is just an XOR-RLE reconstruction of the target state in the ring's scratch buffer, NO core
			 * work. Then render ONLY the target frame with a SINGLE state_load + run. The old loop did a
			 * full state_load + frame re-emulate for EVERY stepped-back frame even though only the last is
			 * shown (the intermediates existed solely for a blended reverse-audio), which is why a heavy
			 * core capped rewind near 2x. One render per present makes the per-present cost SPEED-
			 * INDEPENDENT (~1 state_load + 1 re-emulate), so 6x (and beyond) fits one vsync at ANY clock. */
			for (k = 0; k < steps; k++) { if (ayaneo_rewind_step() != 0) break; walked++; }
			if (walked > 0 && (st = ayaneo_rewind_cur(&sz)) != 0 && sz) {
				if (c->set_av_skip) c->set_av_skip(0, 0);   /* never skip the rewound frame's render/audio */
				ta = gpt4_get_current_tick();
				c->state_load(st, sz);                    /* one retro_unserialize for the target (buffers kept) */
				tb = gpt4_get_current_tick();
				c->run(&fr);                              /* render the target rewound frame ONCE */
				tc = gpt4_get_current_tick();
				t_load = (tb - ta) / 13u; t_run = (tc - tb) / 13u;
				g_nes_dbg_frames++; { extern void ayaneo_fps_tick(void); ayaneo_fps_tick(); }
				/* Reverse the target frame's audio: one frame's worth ~= one present's consumption, so the
				 * AFE ring neither underruns nor overruns. Normal-pitch reverse (no per-speed blend) - the
				 * intermediate frames are no longer emulated, and it is smoother this way. */
				if (fr.audio && fr.frames) {
					const short *a = fr.audio; unsigned int f = fr.frames, i;
					if (f > 2048u) f = 2048u;
					for (i = 0; i < f; i++) {
						s_nes_audrev[i * 2]     = a[(f - 1u - i) * 2];
						s_nes_audrev[i * 2 + 1] = a[(f - 1u - i) * 2 + 1];
					}
					ayaneo_snes_audio_submit(s_nes_audrev, f, sr);
				}
			}
			/* publish the split and the achieved speed */
			g_nes_dbg_rw_load_us = t_load; g_nes_dbg_rw_run_us = t_run; g_nes_dbg_rw_step_us = t_load + t_run;
			g_nes_dbg_rw_eff_x10 = (unsigned)(spd * 10 / 256); g_nes_dbg_rw_mhz = ayaneo_get_cpu_mhz();
			if (fr.video && fr.width && fr.height)
				ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height, fr.pitch / 2u);
			else
				priamry_display_wait_for_vsync();   /* pace a video-less rewound frame (matches snes) */
			mtk_wdt_restart();
			continue;   /* rewind present done; skip the forward path */
		}
		if (ayaneo_rewind_active()) { ayaneo_rewind_end(); ayaneo_audio_reverse_flip(); }

		/* With run ahead the picture shown is the last look-ahead frame's, never the committed
		 * frame's, so that one skips its picture (what the game reads back of the PPU still happens).
		 * Decided before the frame from the menu and from last frame's fast forward: a fast forward
		 * starting now ends in a drawn frame of its own. */
		{
			int unshown = g_nes_commit_unshown && !g_nes_menu_open && !ff_prev && rw_payload && !s_nes_np &&
			              ayaneo_get_preempt_frames() > 0 && s_nes_pf_cap > 0;   /* a look-ahead will be shown */
			if (s_nes_np) {	/* netplay: this frame's pads for both players, from both consoles */
				extern int pico_np_frame(unsigned int local, unsigned int *p1, unsigned int *p2);
				extern void pico_np_set_pads(unsigned int p1, unsigned int p2);
				unsigned int p1 = 0, p2 = 0;
				extern int pico_np_take_reset(unsigned int *p1);
				if (pico_np_frame(nes_local_pad(), &p1, &p2) != 0)
					break;   /* the other console left or the link is gone: back to the menu */
				if (pico_np_take_reset(&p1)) c->reset();   /* the host's reset, here at the same frame */
				pico_np_set_pads(p1, p2);
				{	/* every 120 frames: this console's whole state, hashed before the frame,
					 * compared with the other's (run ahead is off, its buffer is free) */
					extern unsigned int pico_np_frame_no(void);
					extern unsigned int pico_np_crc(const void *p, unsigned int n);
					extern void pico_np_hash(unsigned int frame, unsigned int crc);
					unsigned int fno = pico_np_frame_no() - 1u;   /* the frame these pads are for */
					if (fno && fno % 120u == 0 && rw_payload &&
					    (memset((void *)NES_AHEAD_BUF, 0, rw_payload), 1) &&
					    c->state_save((void *)NES_AHEAD_BUF, rw_payload) == 0)
						pico_np_hash(fno, pico_np_crc((const void *)NES_AHEAD_BUF, rw_payload));
				}
			}
			if (unshown && c->set_av_skip) c->set_av_skip(1, 0);
			em0 = gpt4_get_current_tick();
			c->run(&fr);                                 /* committed frame */
			fe = (gpt4_get_current_tick() - em0) / 13u;  /* per-frame emu cost (us) for the FF cap */
			if (unshown && c->set_av_skip) c->set_av_skip(0, 0);
		}
		g_nes_dbg_frames++; { extern void ayaneo_fps_tick(void); ayaneo_fps_tick(); }
		if (!s_nes_np && !g_nes_menu_open) nes_ra_frame();	/* RetroAchievements: the committed frame */
		if (c->aspect_x1000 && (g_nes_dbg_frames & 15u) == 0)   /* refresh the Fit target (a crop change) */
			g_genesis_aspect_x1000 = c->aspect_x1000();
		if (rw_payload && ayaneo_rewind_ready()) {   /* capture committed frame into the rewind ring */
			void *p = ayaneo_rewind_capture_begin();
			if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
		}
		if (fr.audio && fr.frames)
			ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);

		/* Variable fast-forward (right trigger): after the committed frame, run extra frames so the
		 * game advances 2..10x per displayed frame, capped adaptively to what fits one vsync so the
		 * panel stays a smooth 60fps (as the other cores cap it); the frames not shown skip their picture,
		 * and the present blit is kept in reserve; 2x floor.
		 * Audio of every frame is submitted (so the sound speeds up). Only the LAST frame is presented. */
		ff = (g_nes_menu_open || s_nes_np) ? 0 : ayaneo_joypad_ff_level();   /* netplay: no fast forward */
		ff_prev = ff > 0;
		if (ff > 0) {
			/* no multiplier and no cap: render-skipped frames while one still fits before this
			 * refresh's present (ayaneo_ff_fits), then one rendered frame, the picture shown */
			extern int ayaneo_ff_fits(unsigned int t0, unsigned int blit_us, unsigned int next_us, unsigned int rend_us); extern int ayaneo_ff_cap_frames(void);
			static unsigned int s_nes_skip_us, s_nes_rend_us;
			unsigned int blit = g_dbg_blit_us < 15500u ? g_dbg_blit_us : 0u;
			unsigned int rend = s_nes_rend_us ? s_nes_rend_us : (fe ? fe : 2500u);
			unsigned int skipc = s_nes_skip_us ? s_nes_skip_us : rend;
			int n = 1, last = 0;
			while (!last) {
				unsigned int _t = gpt4_get_current_tick(), us;
				last = !(n < ayaneo_ff_cap_frames() - 1 && ayaneo_ff_fits(em0, blit, skipc, rend));
				/* the frames between two presents skip their picture; what the game reads back of
				 * the PPU (sprite 0 hit, overflow) still happens (nes_core_abi.h, set_av_skip) */
				if (c->set_av_skip) c->set_av_skip(last ? 0 : 1, 0);
				c->run(&fr);
				us = (gpt4_get_current_tick() - _t) / 13u;
				if (last) {
					s_nes_rend_us += ((int)us - (int)s_nes_rend_us) / 4;
					/* the skipped frame cost only moves when one runs: capped at the rendered
					 * cost (measured every batch) so one slow frame cannot keep fast forward at 2x */
					if (s_nes_skip_us > s_nes_rend_us) s_nes_skip_us = s_nes_rend_us;
				} else { s_nes_skip_us += ((int)us - (int)s_nes_skip_us) / 4; skipc = s_nes_skip_us; }
				g_nes_dbg_frames++; { extern void ayaneo_fps_tick(void); ayaneo_fps_tick(); }
				nes_ra_frame();				/* and each fast forward frame */
				if (last && rw_payload && ayaneo_rewind_ready()) {   /* one snapshot per shown frame */
					void *p = ayaneo_rewind_capture_begin();
					if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
				}
				if (fr.audio && fr.frames)
					ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);
				n++;
			}
			if (c->set_av_skip) c->set_av_skip(0, 0);
			ayaneo_hud_set(1, n * 10);
		} else {
			ayaneo_hud_set(0, 0);
		}

		/* Run-ahead: advance the DISPLAY pf frames into the future with the current input, present
		 * that future frame, then restore the committed state so real emulation still advances one
		 * frame per loop - hiding pf frames of the game's internal input lag. Gated off during FF /
		 * menu. A state save and load either side (deterministic); the look-ahead frames skip their
		 * picture (all but the last) and sound (all) through set_av_skip. */
		{
			int pf = (ff || g_nes_menu_open || !rw_payload || s_nes_np) ? 0 : ayaneo_get_preempt_frames();
			int did_ra = 0, i;
			unsigned int ra0 = gpt4_get_current_tick();
			if (pf > 3) pf = 3;
			if (pf > s_nes_pf_cap) pf = s_nes_pf_cap;   /* what this game leaves room for (nes_pf_adapt) */
			/* Only run the look-ahead if the committed state was actually saved (with its exact size):
			 * a failed save leaves nothing to go back to, and the look-ahead frames would stay, the game
			 * running pf+1 times as fast. */
			if (pf > 0) {
				/* The look-ahead frames make no sound (set_av_skip), and the load that undoes them
				 * puts the sound chip back too, so the committed frames' sound runs on unbroken. */
#if defined(KONKR_BLOCK)
				extern unsigned int gpt4_get_current_tick(void);
				extern void block_ra_note(int core, unsigned int frames, unsigned int t0_tick);
				unsigned int ra_t0 = gpt4_get_current_tick();
#endif
				if (c->state_save((void *)NES_AHEAD_BUF, rw_payload) == 0) {
					did_ra = 1;
					for (i = 0; i < pf; i++) {
						if (c->set_av_skip) c->set_av_skip(i == pf - 1 ? 0 : 1, 1);
						c->run(&fr);
					}
					if (c->set_av_skip) c->set_av_skip(0, 0);
#if defined(KONKR_BLOCK)
					block_ra_note(4, (unsigned int)pf, ra_t0);
#endif
				}
			}
			/* present the future (or committed) frame. (Were the run-ahead save ever to fail with the
			 * committed frame undrawn, the previous picture is shown again for this one refresh.) */
			unsigned int show_ticks = gpt4_get_current_tick();
			if (fr.video && fr.width && fr.height) {
				g_nes_dbg_w = fr.width; g_nes_dbg_h = fr.height;
				ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height,
							  fr.pitch / 2u);
			}
			show_ticks = gpt4_get_current_tick() - show_ticks;
			if (did_ra) c->state_load((const void *)NES_AHEAD_BUF, rw_payload);   /* rewind to committed */
			/* the look-ahead's own work: the present is left out, since on a board where it waits for
			 * the refresh that wait would count as run ahead's (the Block's cap then never rose) */
			if (!ff && !g_nes_menu_open)
				nes_pf_adapt(fe, (gpt4_get_current_tick() - ra0 - show_ticks) / 13u);
		}
		mtk_wdt_restart();

		/* AYA taps toggle the Pico menu; holding AYA ~1.5 s force-exits to the selector. */
		{
			int aya;
			pico_input_frame(g_nes_menu_open);	/* the hotkeys: states, restart, the disk, the menu (a tap of its key) */
			aya = PRESSED(GPIO_AYA);
			{ extern volatile int g_dbg_igm_toggle;   /* oem igm: a tap of the menu key */
			  if (g_dbg_igm_toggle) { g_dbg_igm_toggle = 0; aya_prev = 0; aya = 1; } }
			if (aya && !aya_prev) {
				if (g_nes_menu_open && s_nes_igm_on) pico_igm_dismiss();   /* closes animated */
				else if (!g_nes_menu_open && !ra_may_pause()) { }	/* hardcore: too soon after the last pause */
				else if (!g_nes_menu_open && pico_igm_on()) {
					s_nes_igm_on = 1; s_mstat[0] = 0; g_nes_menu_open = 1;
					s_nes_igm.game = s_menu_rom->uname[0] ? s_menu_rom->uname : s_menu_rom->name; s_nes_igm.system = s_menu_rom->type;
					pico_igm_open(&s_nes_igm);
				} else { g_nes_menu_open = !g_nes_menu_open; s_mstat[0] = 0;
					ayaneo_menu_overlay_mark_dirty(); }
			}
			aya_prev = aya;
			if (aya) { if (++aya_hold >= 90) break; } else aya_hold = 0;
		}

		/* menu navigation (Up/Down move, Left/Right change, A select, B/AYA close), with press-edge
		 * + auto-repeat. The game keeps running underneath (pad mask returns 0 while open). */
		if (g_nes_menu_open && s_nes_igm_on) {
			unsigned int k = 0, jd = ayaneo_joypad_dpad();
			int r;
			if (PRESSED(GPIO_UP) || (jd & 0x01u)) k |= IGM_K_UP;
			if (PRESSED(GPIO_DOWN) || (jd & 0x02u)) k |= IGM_K_DOWN;
			if (PRESSED(GPIO_LEFT) || (jd & 0x04u)) k |= IGM_K_LEFT;
			if (PRESSED(GPIO_RIGHT) || (jd & 0x08u)) k |= IGM_K_RIGHT;
			if (PRESSED(GPIO_A)) k |= IGM_K_A;
			if (PRESSED(GPIO_B)) k |= IGM_K_B;
			if (PRESSED(GPIO_LB)) k |= IGM_K_L;
			if (PRESSED(GPIO_RB)) k |= IGM_K_R;
			r = pico_igm_frame(k);
			if (r >= 0) { g_nes_menu_open = 0; pico_input_hold_back(); nes_settings_flush(); }
			reset_hold = 0;
		} else if (g_nes_menu_open) {
			int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
			int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
			int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
			{	/* a key oem nav injects (testing the panel over USB) */
				extern unsigned pico_dbg_take_key(void);
				unsigned dk = pico_dbg_take_key();
				lt |= !!(dk & 1u); rt |= !!(dk & 2u); up |= !!(dk & 4u); dn |= !!(dk & 8u);
				a |= !!(dk & 16u); b |= !!(dk & 32u);
			}
			unsigned int jd = ayaneo_joypad_dpad();   /* left analog stick also drives nav */
			up |= !!(jd & 0x01u); dn |= !!(jd & 0x02u); lt |= !!(jd & 0x04u); rt |= !!(jd & 0x08u);
			#define NAV_DELAY 22
			#define NAV_REP   5
			#define FIRE(h)   ((h) == 1 || ((h) > NAV_DELAY && (((h) - NAV_DELAY) % NAV_REP) == 0))
			up_h = up ? up_h + 1 : 0; dn_h = dn ? dn_h + 1 : 0;
			lt_h = lt ? lt_h + 1 : 0; rt_h = rt ? rt_h + 1 : 0;
			if ((up && FIRE(up_h)) || (dn && FIRE(dn_h)) || (lt && FIRE(lt_h)) || (rt && FIRE(rt_h)) ||
			    (a && !a_p) || (b && !b_p))
				ayaneo_menu_overlay_mark_dirty();
			if (pico_coreopt_page_is_open()) {	/* the core's own options: B back to the list */
				unsigned ck = 0;
				if (up && FIRE(up_h)) ck |= PICO_COREOPT_K_UP;
				if (dn && FIRE(dn_h)) ck |= PICO_COREOPT_K_DOWN;
				if (lt && FIRE(lt_h)) ck |= PICO_COREOPT_K_LEFT;
				if (rt && FIRE(rt_h)) ck |= PICO_COREOPT_K_RIGHT;
				if (b && !b_p) ck |= PICO_COREOPT_K_B;
				pico_coreopt_page_input(ck);
			} else {
				if (up && FIRE(up_h)) s_msel = (s_msel + GM_COUNT - 1) % GM_COUNT;
				if (dn && FIRE(dn_h)) s_msel = (s_msel + 1) % GM_COUNT;
				if (lt && FIRE(lt_h)) gm_change(s_msel, -1, 0);
				if (rt && FIRE(rt_h)) gm_change(s_msel, +1, 0);
				if (a && !a_p) { if (gm_change(s_msel, 0, 1)) { g_nes_menu_open = 0; pico_input_hold_back(); nes_settings_flush(); } }
				if (b && !b_p) { g_nes_menu_open = 0; pico_input_hold_back(); nes_settings_flush(); }
			}
			#undef NAV_DELAY
			#undef NAV_REP
			#undef FIRE
			up_p = up; dn_p = dn; lt_p = lt; rt_p = rt; a_p = a; b_p = b;
			reset_hold = 0;
		} else {
			/* Soft reset: SELECT+START+L+R held ~0.5 s (parity with snes/gba/gbc). */
			if (PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) && PRESSED(GPIO_LB) && PRESSED(GPIO_RB)) {
				if (++reset_hold >= 30) { c->reset(); if (rw_payload) ayaneo_rewind_reset(rw_payload); reset_hold = 0; }
			} else reset_hold = 0;
		}
		{ extern volatile int g_dbg_force_close;   /* oem close: the menu's Exit Game, over USB */
		  if (g_dbg_force_close) { g_dbg_force_close = 0; g_nes_menu_exit = 1; } }
		if (g_nes_menu_exit) break;   /* Pico "Exit Game" selected */
	}
	ayaneo_menu_audio_silence();   /* the ring would loop the last sound through the card writes below */
	/* Arm the exit reverse-punch: the carousel re-entry shrinks this last game frame back into the
	 * selector (matches snes/gba/gbc) instead of a hard cut to black. fr still holds the last rendered
	 * frame (the core framebuffer is untouched until c->unload below). */
	{
		extern void genesis_menu_arm_reverse(const unsigned short *, unsigned, unsigned, unsigned);
		if (fr.video && fr.width && fr.height)
			genesis_menu_arm_reverse((const unsigned short *)fr.video, fr.width, fr.height, fr.pitch / 2u);
	}
	ayaneo_dsi_set_vfp(NES_DEFAULT_VFP);   /* panel back to 59.749 Hz for the menu / other cores */
	{ extern void ayaneo_snes_rsz_restore(void); ayaneo_snes_rsz_restore(); }   /* RSZ back to 1:1 (else Close hangs / carousel shears) */
	ayaneo_menu_overlay(0, 0);   /* disable the overlay BEFORE returning to the carousel (else the
				      * stale Pico panel composites over the selector - see CORE_PORTING_NOTES) */
	ayaneo_hud_set(0, 0);   /* clear the FF/RW badge on exit */
	ayaneo_set_cpu_mhz(saved_mhz);   /* restore the menu's idle clock */
	nes_settings_flush();   /* write any pending volume/brightness change before leaving */

	{ extern void pico_np_stop(void); if (s_nes_np) pico_np_stop(); }   /* tell the other console */
	/* persist SRAM + a suspend state so the next launch resumes; a netplay guest played the
	 * host's game and keeps its own saves as they were */
	if (!s_nes_np_guest) {
		if (c->sram_ptr() && c->sram_size())
			gba_sd_write_sav(vol, rom->name, c->sram_ptr(), c->sram_size());
		ssz = c->state_size();
		if (ssz && ssz <= NES_STATE_CAP && c->state_save((void *)NES_STATE_BUF, ssz) == 0) {
			gba_sd_write_named(vol, "/states/nes", rom->name, "sus",
					   (const unsigned char *)NES_STATE_BUF, ssz);
			pico_shot_save(vol, "/states/nes", rom->name, "sus", 0);
		}
	}
	s_nes_np = s_nes_np_guest = 0;
	pico_coreopt_end();
	c->unload();
	ayaneo_menu_audio_silence();
}

/* Headless core benchmark: load the ROM, run `frames` UNCAPPED at a fixed safe clock (1400 MHz, like the
 * SNES uncapped bench - sustained 100% duty at 1800+ browns the core out), and record the per-frame emu
 * cost + fps + a state save/load round-trip. Publishes an IMPLIED rewind speed (16.7ms / (load+run)) so an
 * autonomous optimization loop has one stable number to push. No display/audio - pure core throughput. */
static void nes_bench_body(fat_vol *vol, const gba_rom_entry *rom, int frames)
{
	const struct nes_core_exports *c = nes_core_load(vol);
	unsigned romsz, ssz, rwp, saved_mhz; int i;
	unsigned int t0, t1;
	struct nes_frame fr;
	g_nes_dbg_bench_fps = 0;
	if (!c) return;
	saved_mhz = ayaneo_get_cpu_mhz();
	ayaneo_set_cpu_mhz(1400);                     /* guaranteed-stable uncapped clock */
	c->heap_init((void *)NES_HEAP_BASE, NES_HEAP_SZ);
	c->init();
	romsz = gba_sd_load_rom(vol, rom, (unsigned char *)NES_ROM_BUF, NES_ROM_CAP);
	if (!romsz || c->load((const void *)NES_ROM_BUF, romsz, 0, 0, image_is_disk((const unsigned char *)NES_ROM_BUF, romsz)) != 0) {
		c->unload(); ayaneo_set_cpu_mhz(saved_mhz); return;
	}
	for (i = 0; i < 90; i++) c->run(&fr);          /* warm up past the BIOS/intro */
	t0 = gpt4_get_current_tick();
	for (i = 0; i < frames; i++) c->run(&fr);
	t1 = gpt4_get_current_tick();
	{
		unsigned int per = ((t1 - t0) / 13u) / (unsigned)(frames > 0 ? frames : 1);
		g_nes_dbg_bench_us  = per;
		g_nes_dbg_bench_fps = per ? (1000000u / per) : 0;
	}
	/* Frame CRC (FNV-1a over the rendered frame after the deterministic run): a RENDER-path change that is
	 * byte-identical keeps this value, so a render optimization can be proven correct headlessly (no live
	 * pixel check needed). Emulation/timing changes may legitimately alter it. */
	if (fr.video && fr.width && fr.height) {
		const unsigned short *pv = (const unsigned short *)fr.video;
		unsigned int pitch_px = fr.pitch / 2u, y, x, h = 0x811c9dc5u;
		for (y = 0; y < fr.height; y++) {
			const unsigned short *row = pv + (unsigned int)y * pitch_px;
			for (x = 0; x < fr.width; x++) { h ^= row[x]; h *= 0x01000193u; }
		}
		g_nes_dbg_bench_crc = h;
	}
	/* Profile split: frames with the picture skipped (set_av_skip 1): a frame less that is the drawing's
	 * cost, the rest the CPU, APU and the PPU's timing. */
	if (c->set_av_skip) {
		c->set_av_skip(1, 0);
		t0 = gpt4_get_current_tick();
		for (i = 0; i < frames; i++) c->run(&fr);
		t1 = gpt4_get_current_tick();
		c->set_av_skip(0, 0);
		g_nes_dbg_bench_norender_us = ((t1 - t0) / 13u) / (unsigned)(frames > 0 ? frames : 1);
	}
	ssz = c->state_size(); rwp = (ssz && ssz <= 0x00400000u) ? ssz : 0;
	if (rwp) {
		t0 = gpt4_get_current_tick(); c->state_save((void *)NES_STATE_BUF, rwp); t1 = gpt4_get_current_tick();
		g_nes_dbg_bench_save_us = (t1 - t0) / 13u;
		t0 = gpt4_get_current_tick(); c->state_load((const void *)NES_STATE_BUF, rwp); t1 = gpt4_get_current_tick();
		g_nes_dbg_bench_load_us = (t1 - t0) / 13u;
		/* Headless 6x-rewind self-test: capture a run of states into the ring, then do the EXACT decoupled
		 * rewind (walk 6 records + one fast state_load + one render) and measure the real per-present cost +
		 * confirm a valid frame. Validates the primary campaign goal (6x rewind) without a live trigger. */
		{
			ayaneo_rewind_reset(rwp);
			for (i = 0; i < 200; i++) {
				c->run(&fr);
				if (ayaneo_rewind_ready()) {
					void *p = ayaneo_rewind_capture_begin();
					if (p && c->state_save(p, rwp) == 0) ayaneo_rewind_capture_commit(rwp);
				}
			}
			if (ayaneo_rewind_count() > 6u) {
				int k, ok = 0; unsigned int sz2; const void *st2;
				ayaneo_rewind_begin();
				t0 = gpt4_get_current_tick();
				for (k = 0; k < 6; k++) if (ayaneo_rewind_step() != 0) break;
				st2 = ayaneo_rewind_cur(&sz2);
				if (st2 && sz2) {
					c->state_load(st2, sz2);
					c->run(&fr);
					ok = (fr.video && fr.width >= 160u && fr.height >= 100u);
				}
				t1 = gpt4_get_current_tick();
				ayaneo_rewind_end();
				g_nes_dbg_bench_rw6_us = (t1 - t0) / 13u;   /* real 6x rewind present cost @1400 */
				g_nes_dbg_bench_rwok   = ok ? 1u : 0u;
			}
		}
	}
	/* implied rewind x10: one present (~15500us usable) / (per-step = state_load + one re-emulate) */
	{
		unsigned int step = g_nes_dbg_bench_load_us + g_nes_dbg_bench_us;
		g_nes_dbg_bench_rwx10 = step ? (155000u / step) : 0;
	}
	g_nes_dbg_bench_mhz = ayaneo_get_cpu_mhz();
	c->unload();
	ayaneo_set_cpu_mhz(saved_mhz);
}

/* ---- dedicated emulation thread (see gbc_sd_run.c rationale: emu_thread's 64 KB overflows) ---- */
static thread_t *s_nes_thread;
static event_t   s_nes_kick, s_nes_done;
static fat_vol  *s_nes_vol;
static const gba_rom_entry *s_nes_rom;
static volatile int s_nes_bench_n;   /* >0: run the headless bench for N frames instead of a session */

static int nes_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_nes_kick);
		if (s_nes_bench_n > 0) { nes_bench_body(s_nes_vol, s_nes_rom, s_nes_bench_n); s_nes_bench_n = 0; }
		else nes_session_body(s_nes_vol, s_nes_rom);
		event_signal(&s_nes_done, false);
	}
	return 0;
}

/* Called from emu_thread's ROM-select dispatch. Runs the game on the nes_emu thread and blocks
 * here until it returns to the selector (AYA). */
void nes_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	if (!s_nes_thread) {
		event_init(&s_nes_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_nes_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_nes_thread = emu_thread_create("nes_emu", &nes_thread_fn, NULL,
					     DEFAULT_PRIORITY, EMU_STACK_NES, 262144);
		if (!s_nes_thread) { nes_session_body(vol, rom); return; }   /* fallback: inline */
		thread_resume(s_nes_thread);
	}
	s_nes_bench_n = 0;
	s_nes_vol = vol;
	s_nes_rom = rom;
	event_signal(&s_nes_kick, false);
	event_wait(&s_nes_done);
}

/* Headless benchmark of `rom` for `frames` uncapped frames on the nes_emu thread (stack-safe).
 * Blocks until done; results land in the g_nes_dbg_bench_* globals. */
void nes_sd_bench(fat_vol *vol, const gba_rom_entry *rom, int frames)
{
	if (!s_nes_thread) {
		event_init(&s_nes_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_nes_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_nes_thread = emu_thread_create("nes_emu", &nes_thread_fn, NULL,
					     DEFAULT_PRIORITY, EMU_STACK_NES, 262144);
		if (!s_nes_thread) { s_nes_bench_n = frames; nes_bench_body(vol, rom, frames); s_nes_bench_n = 0; return; }
		thread_resume(s_nes_thread);
	}
	s_nes_bench_n = frames > 0 ? frames : 300;
	s_nes_vol = vol;
	s_nes_rom = rom;
	event_signal(&s_nes_kick, false);
	event_wait(&s_nes_done);
}
