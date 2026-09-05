/*
 * genesis_sd_run.c - run a Sega Genesis/MD, Master System, Game Gear or SG-1000 ROM from the SD
 * card through the loadable Genesis-Plus-GX core blob. Mirrors emu/gbc/gbc_sd_run.c: a dedicated
 * emulation thread (emu_thread's 64 KB stack overflows on a full core), the shared 0x50000000
 * arena, ROM/SRAM/state from the card. BASIC bring-up first (display + input + audio + SRAM +
 * suspend-resume); the parity extras (adaptive fast-forward + HUD, rewind, run-ahead, aspect
 * Fit/Stretch, Pico menu, GPGX core options) are layered on afterwards - see GENESIS_PORT_PROGRESS.md.
 *
 * Only one core runs at a time, so we reuse the gpSP/gambatte/snes9x arena at 0x50000000.
 */
#include "genesis_core_abi.h"
#include "../gba/sd_fat.h"
#include "../gba/gba_sd_save.h"
#include "../ayaneo_rewind.h"
#include <kernel/thread.h>
#include <kernel/event.h>

/* ---- LK / driver primitives (externs; no LK headers here) ---- */
extern const struct genesis_core_exports *genesis_core_load(void);   /* genesis_core_loader.c */
extern void     ayaneo_genesis_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern void     ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz);
extern void     ayaneo_menu_audio_silence(void);
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
#define GPIO_AYA       86      /* return to the selector */
#define GPIO_LB        92
#define GPIO_RB        81
#define GPIO_SELECT    90
#define GPIO_START     91
#define GPIO_B         82      /* held at launch = start fresh (skip resume) */
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#define GPIO_A         83
#define GPIO_X         85
#define GPIO_Y         84

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

/* Arena (0x50000000, 64 MB, shared - only one core runs at a time). GPGX with -DUSE_DYNAMIC_ALLOC
 * malloc()s cart.rom (up to 32 MB) + work RAM + VRAM + the render bitmap + sound state out of the
 * heap, so the heap must be large. Layout below stays inside the pre-mapped [0x50000000,0x54000000)
 * GBA arena (no custom MMU map, unlike snes9x - so no unmapped-tail savestate gotcha). */
#define GEN_HEAP_BASE  0x50000000u
#define GEN_HEAP_SZ    0x02C00000u        /* 44 MB */
#define GEN_ROM_BUF    0x52C00000u        /* SD ROM read here, handed to load() (GPGX copies it) */
#define GEN_ROM_CAP    0x00800000u        /* 8 MB (largest MD carts ~8 MB) */
#define GEN_STATE_BUF  0x53400000u        /* save/suspend state scratch */
#define GEN_STATE_CAP  0x00400000u        /* 4 MB (GPGX state ~0.5-1 MB) */
#define GEN_AHEAD_BUF  0x53800000u        /* run-ahead save/restore scratch (separate from state) */
#define GEN_REWIND_MAX_SPD 1536           /* max rewind speed in 256ths (1536 = 6x); floor 256 = 1x */

/* run-ahead CPU clock by depth: pf frames run pf+1 emulations/display + 1 save + 1 load, so
 * escalate the clock with depth (mirrors the snes s_snes_ra_opp). ayaneo_set_cpu_mhz is PLL-only. */
/* Gameplay CPU clock per run-ahead tier (index = pf: 0 Off, 1 Balanced, 2 Responsive, 3 Max). Set per
 * the user's chosen ladder - each tier steps up ~200 MHz for the extra pf+1 emulations run-ahead does.
 * ayaneo_set_cpu_mhz programs the PLL directly (no grid snapping), so these exact values are applied. */
static const unsigned s_gen_ra_opp[4] = { 999, 1199, 1399, 1599 };

volatile int g_genesis_menu_open;         /* gates game input + FF/rewind while the Pico menu is up */
volatile int g_genesis_aspect;            /* display aspect: 0=Pixel 1=Fit 2=Stretch (ayaneo_genesis_show_frame) */
volatile unsigned g_genesis_aspect_x1000; /* core display aspect * 1000 (Fit target); 0 -> 4:3 */
volatile int g_genesis_filter;            /* LCD filter: 0=off 1=scanlines 2/3=grid */

/* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits. GPGX maps retro -> Genesis internally:
 * Genesis B=retro B, C=retro A, A=retro Y, Start=retro Start; 6-button X=retro L, Y=retro X,
 * Z=retro R, Mode=retro Select. So the device face buttons land on sensible Genesis buttons. */
unsigned ayaneo_genesis_pad_mask(unsigned port)
{
	unsigned m = 0, d;
	if (port != 0 || g_genesis_menu_open) return 0;
	if (PRESSED(GPIO_B))      m |= 1u << RJ_B;
	if (PRESSED(GPIO_A))      m |= 1u << RJ_A;
	if (PRESSED(GPIO_Y))      m |= 1u << RJ_Y;
	if (PRESSED(GPIO_X))      m |= 1u << RJ_X;
	if (PRESSED(GPIO_LB))     m |= 1u << RJ_L;
	if (PRESSED(GPIO_RB))     m |= 1u << RJ_R;
	if (PRESSED(GPIO_START))  m |= 1u << RJ_START;
	if (PRESSED(GPIO_SELECT)) m |= 1u << RJ_SELECT;
	if (PRESSED(GPIO_UP))     m |= 1u << RJ_UP;
	if (PRESSED(GPIO_DOWN))   m |= 1u << RJ_DOWN;
	if (PRESSED(GPIO_LEFT))   m |= 1u << RJ_LEFT;
	if (PRESSED(GPIO_RIGHT))  m |= 1u << RJ_RIGHT;
	d = ayaneo_joypad_dpad();   /* left analog stick -> D-pad */
	if (d & 0x01u) m |= 1u << RJ_UP;
	if (d & 0x02u) m |= 1u << RJ_DOWN;
	if (d & 0x04u) m |= 1u << RJ_LEFT;
	if (d & 0x08u) m |= 1u << RJ_RIGHT;
	return m;
}

static int rom_type_to_system(unsigned char type)
{
	switch (type) {
	case GBA_CONSOLE_SMS: return GEN_SYS_SMS;
	case GBA_CONSOLE_GG:  return GEN_SYS_GG;
	case GBA_CONSOLE_SG:  return GEN_SYS_SG;
	default:              return GEN_SYS_MD;
	}
}

/* debug counters for `oem gen-*` */
volatile unsigned g_gen_dbg_frames;
volatile unsigned g_gen_dbg_w, g_gen_dbg_h, g_gen_dbg_sr;
volatile int      g_gen_dbg_loadrc;
/* rewind cost breakdown (updated while rewinding; read via `oem gen-rw`): the per-step state_load
 * (portable retro_unserialize + system_reset) cost vs the c->run re-emulate cost, the smoothed per-step
 * total, and the achieved reverse speed x10. Answers "is the state restore or the re-emulation the wall?" */
volatile unsigned g_gen_dbg_rw_load_us, g_gen_dbg_rw_run_us, g_gen_dbg_rw_step_us, g_gen_dbg_rw_eff_x10;
volatile unsigned g_gen_dbg_rw_mhz;   /* CPU clock at the time of the measurement */
/* headless benchmark results (oem gen-bench): uncapped forward throughput + state costs at a fixed
 * safe clock, so autonomous optimization iterations have a stable core-speed metric. */
volatile unsigned g_gen_dbg_bench_fps, g_gen_dbg_bench_us, g_gen_dbg_bench_save_us, g_gen_dbg_bench_load_us;
volatile unsigned g_gen_dbg_bench_mhz, g_gen_dbg_bench_rwx10;   /* clock; implied rewind speed x10 */
volatile unsigned g_gen_dbg_bench_loadfast_us;   /* state_load with the buffer-clear memsets skipped */
volatile unsigned g_gen_dbg_bench_norender_us;   /* per-frame with the VDP pixel render skipped (CPU+timing) */
volatile unsigned g_gen_dbg_bench_rw6_us, g_gen_dbg_bench_rwok;   /* headless 6x-rewind present cost + valid-frame flag */
volatile unsigned g_gen_dbg_bench_crc;   /* FNV-1a of the rendered frame (render-change byte-identity check) */

/* ---- in-game Pico menu (hardware OVL0 L0 overlay over the running game; mirrors the snes menu).
 * g_genesis_menu_open (declared above) gates game input + FF/rewind; the game keeps running so
 * settings preview live. Scoped to the settings that need no display-path change yet (Brightness,
 * Volume, CPU Clock, Save-state slots, Reset, Exit); aspect/filter/core-options land with the RSZ
 * display path. ---- */
volatile int g_genesis_menu_exit;   /* "Exit Game" -> the run loop breaks back to the selector */
static int   s_msel;
static char  s_mstat[48];
static int   s_save_slot;            /* manual save-state slot 0..2 */
static const struct genesis_core_exports *s_menu_c;   /* session context for the menu actions */
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static unsigned             s_menu_rw_payload;

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
extern int  ayaneo_get_gen_aspect(void); extern void ayaneo_set_gen_aspect(int v);   /* persisted per-core settings */
extern int  ayaneo_get_gen_filter(void); extern void ayaneo_set_gen_filter(int v);
extern int  ayaneo_get_gen_region(void); extern void ayaneo_set_gen_region(int v);
extern int  ayaneo_get_gen_slot(void);   extern void ayaneo_set_gen_slot(int v);

/* Deferred settings persist (mirrors the snes core): a hardware-rocker OR Pico-menu volume/brightness
 * change marks dirty; the eMMC+SD write is debounced ~0.5 s so holding a key never spams flash. Before
 * this, Genesis persisted NO settings - brightness/volume were lost on the next launch. */
static int s_gen_settings_dirty;
#define GEN_SETTINGS_DEBOUNCE 30
static void genesis_settings_touch(void) { s_gen_settings_dirty = GEN_SETTINGS_DEBOUNCE; }
static void genesis_settings_tick(void)  { if (s_gen_settings_dirty > 0 && --s_gen_settings_dirty == 0) ayaneo_menu_settings_persist(); }
static void genesis_settings_flush(void) { if (s_gen_settings_dirty > 0) { s_gen_settings_dirty = 0; ayaneo_menu_settings_persist(); } }

/* manual CPU-clock grid (ayaneo_set_cpu_mhz reprograms the PLL only, so >boot-Vproc is the user's call) */
static const unsigned s_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static int s_cpu_idx = -1;
static void genesis_cpu_step(int dir)
{
	int n = (int)(sizeof s_cpu_opp / sizeof s_cpu_opp[0]), i;
	if (s_cpu_idx < 0) {
		unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u; int best = 0;
		for (i = 0; i < n; i++) { unsigned d = s_cpu_opp[i] > cur ? s_cpu_opp[i] - cur : cur - s_cpu_opp[i]; if (d < bd) { bd = d; best = i; } }
		s_cpu_idx = best;
	}
	s_cpu_idx += dir; if (s_cpu_idx < 0) s_cpu_idx = 0; if (s_cpu_idx >= n) s_cpu_idx = n - 1;
	ayaneo_set_cpu_mhz(s_cpu_opp[s_cpu_idx]);
}

extern int  ayaneo_get_preempt_frames(void);
extern void ayaneo_set_preempt_frames(int v);

enum { GM_BRIGHT, GM_VOLUME, GM_ASPECT, GM_FILTER, GM_REGION, GM_RUNAHEAD, GM_CPU, GM_REFRESH, GM_SLOT, GM_SAVE, GM_LOAD, GM_RESET, GM_EXIT, GM_COUNT };
static const char *gen_aspect_name(int a) { return a == 2 ? "Stretch" : a == 1 ? "Fit" : "Pixel"; }
static const char *gen_filter_name(int f) { return f == 3 ? "Grid+" : f == 2 ? "Grid" : f == 1 ? "Scanlines" : "Off"; }
static const char *gen_ra_name(int pf) { return pf == 3 ? "Max" : pf == 2 ? "Responsive" : pf == 1 ? "Balanced" : "Off"; }

/* Region override -> GPGX genesis_plus_gx_region_detect. The core applies it live via check_variables
 * (reinits framerate/audio timing + I/O region reg); some games only honour it on reset. */
static int s_gen_region;   /* 0 Auto, 1 USA (ntsc-u), 2 Europe (pal), 3 Japan (ntsc-j) */
static volatile int s_gen_refresh_retune;   /* >0: re-read core fps + retune panel vfp after the next committed frame(s) (real-time region switch) */
static const char *gen_region_name(int r) { return r == 3 ? "Japan" : r == 2 ? "Europe" : r == 1 ? "USA" : "Auto"; }
static const char *gen_region_opt(int r)  { return r == 3 ? "ntsc-j" : r == 2 ? "pal" : r == 1 ? "ntsc-u" : "auto"; }
extern unsigned int ayaneo_dsi_refresh_milli(void);   /* panel refresh in milli-Hz (ties into LCM work) */
extern void ayaneo_dsi_set_vfp(unsigned int vfp);     /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);
extern unsigned int ayaneo_dsi_set_fps_milli(unsigned int fps_milli);  /* set panel refresh to target fps (milli-Hz), returns vfp (ddp_dsi.c) */
/* Panel vertical-front-porch per refresh rate (vtotal = 976 + vfp; refresh ~= 59.667 kHz / vtotal on
 * the shipped AYANEO_GBA build). Genesis NTSC is 59.92 Hz -> vfp 20 -> vtotal 996 matches the core's
 * native rate (the vsync-locked present then paces emulation to it = no judder), like SNES uses vfp 17.
 * PAL is ~49.70 Hz -> vfp 224 -> vtotal 1200 (needs ayaneo_dsi_set_vfp's clamp raised past 200 - see
 * the DSI mechanism change). DEFAULT_VFP 23 (59.749 Hz) is restored for the menu / other cores on exit. */
#define GEN_VFP        20u    /* NTSC fallback if the core reports no fps */
#define GEN_DEFAULT_VFP 23u
volatile unsigned g_gen_dbg_vfp;   /* live DSI vfp during the session (validates the switch) */

/* Map a core frame rate (milli-Hz) to the panel vfp that makes the vsync-locked present pace exactly
 * that rate. vtotal = 976 + vfp and refresh_milli = 59667000 / vtotal (shipped-build line const,
 * matches ayaneo_dsi_refresh_milli), so vfp = 59667000/fps_milli - 976, rounded. Guards a zero/insane
 * fps by falling back to the NTSC default. The DSI helper clamps the final vfp to its safe porch range. */
static unsigned genesis_vfp_for_fps(unsigned fps_milli)
{
	unsigned vtotal;
	if (fps_milli < 40000u || fps_milli > 65000u) return GEN_VFP;   /* implausible -> NTSC */
	vtotal = (59667000u + fps_milli / 2u) / fps_milli;             /* rounded 59667000/fps_milli */
	if (vtotal <= 976u) return 4u;                                 /* faster than the porch floor */
	return vtotal - 976u;
}

static char *mput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mputu(char *p, unsigned v) { char t[12]; int n = 0; if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; } while (n) *p++ = t[--n]; return p; }

static const char *gm_label(int i) { switch (i) {
	case GM_BRIGHT: return "Brightness"; case GM_VOLUME: return "Volume";
	case GM_ASPECT: return "Aspect Ratio"; case GM_FILTER: return "LCD Filter";
	case GM_REGION: return "Region"; case GM_REFRESH: return "Refresh Rate";
	case GM_RUNAHEAD: return "Run-Ahead"; case GM_CPU: return "CPU Clock";
	case GM_SLOT: return "Save Slot"; case GM_SAVE: return "Save State"; case GM_LOAD: return "Load State";
	case GM_RESET: return "Reset Game"; case GM_EXIT: return "Exit Game"; } return ""; }

static const char *gm_value(int i, char *buf) { char *p = buf;
	switch (i) {
	case GM_BRIGHT: p = mputu(p, (unsigned)ayaneo_brightness_pct()); p = mput(p, "%"); break;
	case GM_VOLUME: p = mputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mput(p, "%"); break;
	case GM_ASPECT: p = mput(p, gen_aspect_name(g_genesis_aspect)); break;
	case GM_FILTER: p = mput(p, gen_filter_name(g_genesis_filter)); break;
	case GM_REGION: p = mput(p, gen_region_name(s_gen_region)); break;
	case GM_REFRESH: { unsigned mhz = ayaneo_dsi_refresh_milli();   /* read-only: XX.X Hz */
		p = mputu(p, mhz / 1000u); *p++ = '.'; *p++ = (char)('0' + (mhz % 1000u) / 100u); p = mput(p, " Hz"); } break;
	case GM_RUNAHEAD: p = mput(p, gen_ra_name(ayaneo_get_preempt_frames())); break;
	case GM_CPU:    p = mputu(p, ayaneo_get_cpu_mhz()); p = mput(p, " MHz"); break;
	case GM_SLOT:   p = mputu(p, (unsigned)s_save_slot); break;
	case GM_SAVE: case GM_LOAD: case GM_RESET: case GM_EXIT: p = mput(p, "[A]"); break;
	} *p = 0; return buf; }

static void genesis_slot_ext(char *e) { e[0] = 's'; e[1] = 't'; e[2] = (char)('0' + s_save_slot); e[3] = 0; }

/* returns 1 to close the menu (Reset/Exit) */
static int gm_change(int i, int dir, int act)
{
	s_mstat[0] = 0;
	switch (i) {
	case GM_BRIGHT: if (dir) { ayaneo_brightness_step(dir); genesis_settings_touch(); } break;
	case GM_VOLUME: if (dir) { ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); genesis_settings_touch(); } break;
	case GM_ASPECT: if (dir) { g_genesis_aspect = (g_genesis_aspect + dir + 3) % 3;   /* live preview */
		ayaneo_set_gen_aspect(g_genesis_aspect); genesis_settings_touch(); } break;
	case GM_FILTER: if (dir) { g_genesis_filter = (g_genesis_filter + dir + 4) % 4;
		ayaneo_set_gen_filter(g_genesis_filter); genesis_settings_touch(); } break;
	case GM_REGION: if (dir) { s_gen_region = (s_gen_region + dir + 4) % 4;
		if (s_menu_c && s_menu_c->set_option)
			s_menu_c->set_option("genesis_plus_gx_region_detect", gen_region_opt(s_gen_region));
		ayaneo_set_gen_region(s_gen_region); genesis_settings_touch();
		/* Real-time refresh switch: the core option is now queued (s_opt_dirty), but check_variables -
		 * which recomputes system_clock/vdp_pal/lines_per_frame, hence fps - only runs inside the next
		 * retro_run(). The game keeps running underneath the Pico menu, so the very next c->run() in the
		 * session loop applies the region and updates fps. Arm a deferred re-tune that the session loop
		 * fires after that frame; do NOT poll fps here (it would still read the OLD rate). */
		s_gen_refresh_retune = 2;   /* re-read fps + retune vfp for the next 2 committed frames */
		mput(s_mstat, "Region set (some games need Reset)"); } break;
	case GM_REFRESH: break;   /* read-only display (panel Hz); no adjust */
	case GM_RUNAHEAD: if (dir) { int pf = (ayaneo_get_preempt_frames() + dir + 4) % 4;
		ayaneo_set_preempt_frames(pf); ayaneo_set_cpu_mhz(s_gen_ra_opp[pf]); s_cpu_idx = -1;
		genesis_settings_touch(); } break;   /* persist run-ahead (shared preempt-frames blob) */
	case GM_CPU:    if (dir) genesis_cpu_step(dir); break;
	case GM_SLOT:   if (dir) { s_save_slot = (s_save_slot + dir + 3) % 3;
		ayaneo_set_gen_slot(s_save_slot); genesis_settings_touch(); } break;
	case GM_SAVE: if (act) {
		unsigned char *st = (unsigned char *)GEN_STATE_BUF; unsigned ssz = s_menu_c->state_size();
		char ext[4]; int ok; genesis_slot_ext(ext);
		ok = (ssz && ssz <= GEN_STATE_CAP && s_menu_c->state_save(st, ssz) == 0 &&
		      gba_sd_write_named(s_menu_vol, "/states/genesis", s_menu_rom->name, ext, st, ssz) == 0);
		mput(s_mstat, ok ? "State saved" : "Save failed"); } break;
	case GM_LOAD: if (act) {
		unsigned char *st = (unsigned char *)GEN_STATE_BUF; char ext[4]; unsigned n; int ok;
		genesis_slot_ext(ext);
		n = gba_sd_read_named(s_menu_vol, "/states/genesis", s_menu_rom->name, ext, st, GEN_STATE_CAP);
		ok = (n && s_menu_c->state_load(st, n) == 0);
		if (ok && s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload);   /* loaded state breaks the rewind timeline */
		mput(s_mstat, ok ? "State loaded" : "No save state"); } break;
	case GM_RESET: if (act) { s_menu_c->reset(); if (s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload); return 1; } break;
	case GM_EXIT:  if (act) { g_genesis_menu_exit = 1; return 1; } break;
	}
	return 0;
}

int genesis_menu_open(void) { return g_genesis_menu_open; }

void genesis_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int rowH = 38, panelW = 520, panelH = 84 + GM_COUNT * rowH + 42;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2, x = px + 28, y = py + 84, i;
	char val[48];
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "GammaOS Pico");
	for (i = 0; i < GM_COUNT; i++, y += rowH) {
		unsigned int fg = (i == s_msel) ? 0xFF101018u : 0xFFC8D0E0u; int vw;
		if (i == s_msel) ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, y, 2, fg, gm_label(i));
		gm_value(i, val); for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	if (s_mstat[0]) ayaneo_text(buf, pitch, x, py + panelH - 40, 2, 0xFF80E080u, s_mstat);
	ayaneo_text(buf, pitch, x, py + panelH - 16, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

/* Hardware volume rocker (mirrors snes_poll_volume / gba poll_volume so every core behaves the same):
 * VolUp = mtk_detect_key(0x11), VolDown = 0x00, edge-detected (one press = one step). Holding SELECT
 * turns the rocker into a brightness control. Runs whether or not the Pico menu is open. Debounced to
 * disk via genesis_settings_touch. Genesis previously had NO in-game volume/brightness shortcut. */
static void genesis_poll_volume(void)
{
	static int vu_p, vd_p;
	int vu = mtk_detect_key(0x11), vd = mtk_detect_key(0x00);
	int sel = PRESSED(GPIO_SELECT);
	int dir = 0;
	if (vu && !vu_p) dir = +1; else if (vd && !vd_p) dir = -1;
	vu_p = vu; vd_p = vd;
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
	genesis_settings_touch();
}

/* Power key during gameplay (mirrors gba check_power/save_and_poweroff so power behaves the same on
 * every core): arm on release, fire on the next press. On press: silence audio, write the cartridge
 * battery + a suspend state so the next launch resumes, flush pending settings, then power off. Uses
 * the session context stashed in s_menu_* by genesis_session_body. */
static void genesis_power_check(void)
{
	static int armed;
	int p = pmic_detect_powerkey();
	if (!p) { armed = 1; return; }
	if (!armed) return;
	armed = 0;
	ayaneo_menu_audio_silence();
	if (s_menu_c && s_menu_vol && s_menu_rom) {
		const struct genesis_core_exports *c = s_menu_c;
		unsigned ssz;
		if (c->sram_ptr() && c->sram_size())
			gba_sd_write_sav(s_menu_vol, s_menu_rom->name, c->sram_ptr(), c->sram_size());
		ssz = c->state_size();
		if (ssz && ssz <= GEN_STATE_CAP && c->state_save((void *)GEN_STATE_BUF, ssz) == 0)
			gba_sd_write_named(s_menu_vol, "/states/genesis", s_menu_rom->name, "sus",
					   (const unsigned char *)GEN_STATE_BUF, ssz);
	}
	genesis_settings_flush();
	mt_power_off();   /* no return */
}

static void genesis_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct genesis_core_exports *c = genesis_core_load();
	unsigned romsz, sr = 44100, ssz, rw_payload, saved_mhz;
	int aya_prev = 0, aya_hold = 0, rw_acc = 0, reset_hold = 0;
	int up_h = 0, dn_h = 0, lt_h = 0, rt_h = 0;             /* menu nav auto-repeat hold counters */
	int up_p = 0, dn_p = 0, lt_p = 0, rt_p = 0, a_p = 0, b_p = 0;
	struct genesis_frame fr;

	if (!c) return;   /* blob load failed (g_gen_dbg_loaderr set by the loader) */

	c->heap_init((void *)GEN_HEAP_BASE, GEN_HEAP_SZ);
	c->init();

	romsz = gba_sd_load_rom(vol, rom, (unsigned char *)GEN_ROM_BUF, GEN_ROM_CAP);
	g_gen_dbg_loadrc = -2;
	if (!romsz) { ayaneo_menu_audio_silence(); return; }
	if (c->load((const void *)GEN_ROM_BUF, romsz, rom_type_to_system(rom->type)) != 0) {
		g_gen_dbg_loadrc = -1; ayaneo_menu_audio_silence(); return;
	}
	g_gen_dbg_loadrc = 0;

	/* cartridge battery (SRAM/EEPROM) from /saves */
	if (c->sram_ptr() && c->sram_size())
		gba_sd_load_sav(vol, rom->name, (unsigned char *)c->sram_ptr(), c->sram_size());

	/* suspend-resume: reload the auto-saved state unless B is held at launch (fresh start) */
	if (!PRESSED(GPIO_B)) {
		ssz = c->state_size();
		if (ssz && ssz <= GEN_STATE_CAP) {
			unsigned n = gba_sd_read_named(vol, "/states/genesis", rom->name, "sus",
						       (unsigned char *)GEN_STATE_BUF, GEN_STATE_CAP);
			if (n) c->state_load((const void *)GEN_STATE_BUF, n);
		}
	}

	c->av_info(0, 0, 0, 0, &sr);
	if (!sr) sr = 44100;
	g_gen_dbg_sr = sr;
	if (c->aspect_x1000) g_genesis_aspect_x1000 = c->aspect_x1000();   /* Fit target (~4:3) */

	/* Arm the rewind ring: capture the FULL serialized state each frame (GPGX has no raw fast
	 * snapshot, so unlike snes we use state_save/state_load = retro_serialize). The high-DRAM delta
	 * ring XOR+RLEs consecutive same-size states, so a frame of change compresses well. Disabled if
	 * the state is absurdly large or the region is unavailable. */
	rw_payload = c->state_size();
	if (rw_payload && rw_payload <= 0x00400000u) ayaneo_rewind_reset(rw_payload);
	else rw_payload = 0;

	/* Raise the CPU clock for gameplay, escalated by the run-ahead depth (pf frames run pf+1
	 * emulations + a save + a load per display). The menu holds a lower idle clock; restored on
	 * exit. ayaneo_set_cpu_mhz reprograms only the PLL (boot Vproc), like the SNES gameplay tiers. */
	saved_mhz = ayaneo_get_cpu_mhz();
	{ int pf0 = ayaneo_get_preempt_frames(); ayaneo_set_cpu_mhz(s_gen_ra_opp[(pf0 >= 0 && pf0 < 4) ? pf0 : 0]); }

	/* menu context (actions reference the session core + save target) */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom; s_menu_rw_payload = rw_payload;
	g_genesis_menu_open = 0; g_genesis_menu_exit = 0; s_msel = 0; s_mstat[0] = 0; s_cpu_idx = -1;

	/* Restore persisted per-core settings (aspect/filter/region/slot) so a launch honours the player's
	 * last picks instead of defaults. Run-ahead uses the shared preempt-frames; volume/brightness the
	 * shared blob. Region is pushed to the core here (before the launch frames run). */
	g_genesis_aspect = ayaneo_get_gen_aspect();
	g_genesis_filter = ayaneo_get_gen_filter();
	s_gen_region     = ayaneo_get_gen_region();
	s_save_slot      = ayaneo_get_gen_slot();
	if (s_gen_region && c->set_option)
		c->set_option("genesis_plus_gx_region_detect", gen_region_opt(s_gen_region));

	/* Switch the panel to the CORE's native rate (NTSC ~59.92 Hz, PAL ~49.70 Hz) so the vsync-locked
	 * present paces emulation to it (no periodic judder / no speed error). Read fps live from the core:
	 * the region option pushed above has NOT been applied yet (check_variables runs inside retro_run,
	 * and no frame has run), so run one discarded frame first if a region was forced, then read.
	 * Restored to the stock 59.749 Hz for the menu on exit. */
	if (s_gen_region && c->set_option) {
		struct genesis_frame pf0; pf0.video = 0; pf0.width = 0; pf0.height = 0;
		c->run(&pf0);   /* let check_variables apply the forced region so fps_milli reflects it */
	}
	{
		unsigned fps = (c->fps_milli) ? c->fps_milli() : 0u;
		ayaneo_dsi_set_vfp(fps ? genesis_vfp_for_fps(fps) : GEN_VFP);
	}
	g_gen_dbg_vfp = ayaneo_dsi_get_vfp();

	/* Launch punch-hole (matches snes/gba/gbc): the menu handed off with the frozen carousel still on
	 * screen (GBA_PUNCH_SNAP_PA = 0x54000000) and gba_punch_ready set. Run a few frames so real
	 * gameplay is on screen (not a black/boot frame), pre-render it full-screen with the live aspect
	 * geometry, then grow a circle from 0 to the panel diagonal over the frozen menu - the carousel is
	 * visibly eaten by the expanding gameplay instead of a hard cut. Skipped for the oem gen-launch. */
	{
		extern int  gba_punch_ready;
		extern void ayaneo_genesis_punch_prerender(const unsigned short *, unsigned, unsigned, unsigned);
		extern void ayaneo_gba_punch_frame_pre(const unsigned int *, int);
		if (gba_punch_ready) {
			int i, w;
			struct genesis_frame pf; pf.video = 0; pf.width = 0; pf.height = 0;
			gba_punch_ready = 0;
			for (w = 0; w < 20; w++) { c->run(&pf); if (pf.video && pf.width && w >= 3) break; }
			if (c->aspect_x1000) g_genesis_aspect_x1000 = c->aspect_x1000();
			if (pf.video && pf.width && pf.height) {
				ayaneo_genesis_punch_prerender((const unsigned short *)pf.video, pf.width, pf.height,
							       pf.pitch / 2u);
				for (i = 1; i <= 20; i++) {
					int r = 820 * i / 20; if (r < 1) r = 1;
					ayaneo_gba_punch_frame_pre((const unsigned int *)0x54000000u, r);
					mtk_wdt_restart();
				}
			}
		}
	}

	for (;;) {
		extern int ayaneo_joypad_ff_level(void);
		extern int ayaneo_joypad_rewind_level(void);
		extern void ayaneo_audio_reverse_flip(void);
		extern volatile unsigned int g_dbg_blit_us;
		static short s_gen_audrev[2048 * 2];   /* reversed+decimated rewind audio scratch */
		unsigned int em0, fe;
		int ff, rw;

		ayaneo_joypad_poll();
		genesis_poll_volume();     /* hardware volume rocker + SELECT-brightness (parity with all cores) */
		genesis_settings_tick();   /* debounced volume/brightness persist */
		genesis_power_check();      /* power key -> save cartridge + suspend, then power off */

		/* Refresh the Pico menu as a hardware overlay (OVL0 L0) over the running game, or disable it
		 * when closed - the game keeps running underneath so settings preview live (mirrors snes). */
		ayaneo_menu_overlay(genesis_menu_paint, g_genesis_menu_open);

		/* Rewind (left trigger): walk the delta ring backward and re-render instead of advancing.
		 * Press depth sets a smooth 1x..6x speed via a fractional accumulator; each stepped-back
		 * frame's audio is reversed + decimated by `steps` and submitted (game plays backwards). On
		 * release the ring commits the rewound point as the new head. Mirrors the snes rewind block.
		 * Gated off while the menu is open. */
		rw = g_genesis_menu_open ? 0 : ayaneo_joypad_rewind_level();
		if (rw > 0 && ayaneo_rewind_ready() && ayaneo_rewind_count() > 0) {
			extern int priamry_display_wait_for_vsync(void);
			int spd = 256 + (rw * (GEN_REWIND_MAX_SPD - 256)) / 255;   /* 256(1x)..MAX_SPD(6x) */
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
				/* FAST load: reset_do_not_clear_buffers skips the ~2.1ms of VDP/render buffer-clear memsets
				 * (measured load 2682us -> 557us). Safe for a same-session load (vram/cram/etc. are reloaded
				 * from the state; bitmap/linebuf/pattern-cache are scratch regenerated by run() - run-ahead
				 * uses the exact same skip). sound_rebase below overrides the fast path's blip-latch restore
				 * with a clean baseline, so the reverse audio stays correct. */
				if (c->set_ra_fast) c->set_ra_fast(1);
				c->state_load(st, sz);                    /* one retro_unserialize for the target (buffers kept) */
				if (c->set_ra_fast) c->set_ra_fast(0);
				tb = gpt4_get_current_tick();
				if (c->sound_rebase) c->sound_rebase();   /* clean audio baseline (state_load can't carry blip phase) */
				c->run(&fr);                              /* render the target rewound frame ONCE */
				tc = gpt4_get_current_tick();
				t_load = (tb - ta) / 13u; t_run = (tc - tb) / 13u;
				g_gen_dbg_frames++;
				/* Reverse the target frame's audio: one frame's worth ~= one present's consumption, so the
				 * AFE ring neither underruns nor overruns. Normal-pitch reverse (no per-speed blend) - the
				 * intermediate frames are no longer emulated, and it is smoother this way. */
				if (fr.audio && fr.frames) {
					const short *a = fr.audio; unsigned int f = fr.frames, i;
					if (f > 2048u) f = 2048u;
					for (i = 0; i < f; i++) {
						s_gen_audrev[i * 2]     = a[(f - 1u - i) * 2];
						s_gen_audrev[i * 2 + 1] = a[(f - 1u - i) * 2 + 1];
					}
					ayaneo_snes_audio_submit(s_gen_audrev, f, sr);
				}
			}
			/* publish the split + achieved speed for `oem gen-rw` */
			g_gen_dbg_rw_load_us = t_load; g_gen_dbg_rw_run_us = t_run; g_gen_dbg_rw_step_us = t_load + t_run;
			g_gen_dbg_rw_eff_x10 = (unsigned)(spd * 10 / 256); g_gen_dbg_rw_mhz = ayaneo_get_cpu_mhz();
			if (fr.video && fr.width && fr.height)
				ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height, fr.pitch / 2u);
			else
				priamry_display_wait_for_vsync();   /* pace a video-less rewound frame (matches snes) */
			mtk_wdt_restart();
			continue;   /* rewind present done; skip the forward path */
		}
		if (ayaneo_rewind_active()) { ayaneo_rewind_end(); ayaneo_audio_reverse_flip(); }

		em0 = gpt4_get_current_tick();
		c->run(&fr);                                 /* committed frame */
		fe = (gpt4_get_current_tick() - em0) / 13u;  /* per-frame emu cost (us) for the FF cap */
		g_gen_dbg_frames++;
		/* Real-time panel refresh follow: a GM_REGION change armed s_gen_refresh_retune; the region
		 * option was applied by the c->run() just above (check_variables -> get_region recomputes fps),
		 * so read the (now new) fps and retune the panel vfp. Retry a couple of frames in case the
		 * variable-update flag was consumed a frame late. No extra wait_for_vsync here (double-wait = 30fps). */
		if (s_gen_refresh_retune > 0 && c->fps_milli) {
			unsigned fps = c->fps_milli();
			if (fps) { ayaneo_dsi_set_vfp(genesis_vfp_for_fps(fps)); g_gen_dbg_vfp = ayaneo_dsi_get_vfp(); }
			s_gen_refresh_retune--;
		}
		if (c->aspect_x1000 && (g_gen_dbg_frames & 15u) == 0)   /* refresh Fit target (H32/H40 switch) */
			g_genesis_aspect_x1000 = c->aspect_x1000();
		if (rw_payload && ayaneo_rewind_ready()) {   /* capture committed frame into the rewind ring */
			void *p = ayaneo_rewind_capture_begin();
			if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
		}
		if (fr.audio && fr.frames)
			ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);

		/* Variable fast-forward (right trigger): after the committed frame, run extra frames so the
		 * game advances 2..10x per displayed frame, capped adaptively to what fits one vsync so the
		 * panel stays a smooth 60fps (mirrors the GBA/GBC/SNES adaptive cap). GPGX renders every FF
		 * frame, so the committed-frame cost is the per-frame cost; reserve the present blit; 2x floor.
		 * Audio of every frame is submitted (so the sound speeds up). Only the LAST frame is presented. */
		ff = g_genesis_menu_open ? 0 : ayaneo_joypad_ff_level();
		if (ff > 0) {
			int raw = 2 + (ff * (10 - 2)) / 255;
			unsigned int rmilli = ayaneo_dsi_refresh_milli();
			unsigned int full_us = rmilli ? (1000000000u / rmilli) : 16666u;   /* 1e9/milliHz = 1e6/panel_fps; PAL ~20000us, NTSC ~16666us */
			unsigned int budget_us = full_us > 1200u ? full_us - 1200u : full_us; /* ~1.2ms present-margin the old 15500-vs-16666 baked in */
			unsigned int blit = g_dbg_blit_us < budget_us ? g_dbg_blit_us : 0u;
			unsigned int fef = fe ? fe : 2500u;
			int cap = (int)((budget_us - blit) / fef);
			int mult = raw < cap ? raw : cap, k;
			if (mult < 2) mult = 2;
			if (mult > 10) mult = 10;
			ayaneo_hud_set(1, mult * 10);
			for (k = 1; k < mult; k++) {
				/* skip the VDP render (the heavy part) on the thrown-away frames; render only
				 * the LAST one (the frame we present). Audio kept on all so the sound speeds up. */
				if (c->set_av_skip) c->set_av_skip(k < mult - 1 ? 1 : 0, 0);
				c->run(&fr);
				g_gen_dbg_frames++;
				if (rw_payload && ayaneo_rewind_ready()) {   /* capture each FF frame too */
					void *p = ayaneo_rewind_capture_begin();
					if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
				}
				if (fr.audio && fr.frames)
					ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);
			}
			if (c->set_av_skip) c->set_av_skip(0, 0);
		} else {
			ayaneo_hud_set(0, 0);
		}

		/* Run-ahead: advance the DISPLAY pf frames into the future with the current input, present
		 * that future frame, then restore the committed state so real emulation still advances one
		 * frame per loop - hiding pf frames of the game's internal input lag. Gated off during FF /
		 * menu. GPGX has no raw snapshot, so state_save/load (fast + deterministic per the host
		 * test); the look-ahead frames skip render (all but the last) and audio (all) via set_av_skip. */
		{
			int pf = (ff || g_genesis_menu_open || !rw_payload) ? 0 : ayaneo_get_preempt_frames();
			int did_ra = 0, i;
			if (pf > 3) pf = 3;
			/* Only run the look-ahead if the committed state was actually saved. Pass the EXACT
			 * state size: GPGX retro_serialize is `if (size != STATE_SIZE) return FALSE`, so a wrong
			 * size (e.g. the buffer capacity) silently fails the save; then the restore has nothing
			 * to rewind to and the look-ahead frames STICK, running the game pf+1x - the
			 * fast-forward-with-frameskip bug. Guarding on the save return makes that impossible. */
			if (pf > 0) {
				/* Preserve the audio-synthesis phase across the run-ahead save/load pair: with
				 * FAST_SAVESTATES on, the state_save latches the committed blip-buffer + FM phase and
				 * the state_load restores it (and suppresses the blip_clear the load would otherwise
				 * do), so the next committed frame's audio is continuous. Without this the look-ahead
				 * frames leave the blip pf frames ahead and every committed frame audibly steps =
				 * the run-ahead crackle. Scoped to THIS pair only (the sound buffer is a single latch). */
				if (c->set_ra_fast) c->set_ra_fast(1);
				if (c->state_save((void *)GEN_AHEAD_BUF, rw_payload) == 0) {
					did_ra = 1;
					for (i = 0; i < pf; i++) {
						if (c->set_av_skip) c->set_av_skip(i == pf - 1 ? 0 : 1, 1);
						c->run(&fr);
					}
					if (c->set_av_skip) c->set_av_skip(0, 0);
				}
			}
			if (fr.video && fr.width && fr.height) {   /* present the future (or committed) frame */
				g_gen_dbg_w = fr.width; g_gen_dbg_h = fr.height;
				ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height,
							  fr.pitch / 2u);
			}
			if (did_ra) c->state_load((const void *)GEN_AHEAD_BUF, rw_payload);   /* rewind to committed */
			if (pf > 0 && c->set_ra_fast) c->set_ra_fast(0);   /* close the audio-preserving window */
		}
		mtk_wdt_restart();

		/* AYA taps toggle the Pico menu; holding AYA ~1.5 s force-exits to the selector. */
		{
			int aya = PRESSED(GPIO_AYA);
			if (aya && !aya_prev) { g_genesis_menu_open = !g_genesis_menu_open; s_mstat[0] = 0;
				ayaneo_menu_overlay_mark_dirty(); }
			aya_prev = aya;
			if (aya) { if (++aya_hold >= 90) break; } else aya_hold = 0;
		}

		/* menu navigation (Up/Down move, Left/Right change, A select, B/AYA close), with press-edge
		 * + auto-repeat. The game keeps running underneath (pad mask returns 0 while open). */
		if (g_genesis_menu_open) {
			int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
			int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
			int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
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
			if (up && FIRE(up_h)) s_msel = (s_msel + GM_COUNT - 1) % GM_COUNT;
			if (dn && FIRE(dn_h)) s_msel = (s_msel + 1) % GM_COUNT;
			if (lt && FIRE(lt_h)) gm_change(s_msel, -1, 0);
			if (rt && FIRE(rt_h)) gm_change(s_msel, +1, 0);
			#undef NAV_DELAY
			#undef NAV_REP
			#undef FIRE
			if (a && !a_p) { if (gm_change(s_msel, 0, 1)) { g_genesis_menu_open = 0; genesis_settings_flush(); } }
			if (b && !b_p) { g_genesis_menu_open = 0; genesis_settings_flush(); }
			up_p = up; dn_p = dn; lt_p = lt; rt_p = rt; a_p = a; b_p = b;
			reset_hold = 0;
		} else {
			/* Soft reset: SELECT+START+L+R held ~0.5 s (parity with snes/gba/gbc). */
			if (PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) && PRESSED(GPIO_LB) && PRESSED(GPIO_RB)) {
				if (++reset_hold >= 30) { c->reset(); if (rw_payload) ayaneo_rewind_reset(rw_payload); reset_hold = 0; }
			} else reset_hold = 0;
		}
		if (g_genesis_menu_exit) break;   /* Pico "Exit Game" selected */
	}
	/* Arm the exit reverse-punch: the carousel re-entry shrinks this last game frame back into the
	 * selector (matches snes/gba/gbc) instead of a hard cut to black. fr still holds the last rendered
	 * frame (the core framebuffer is untouched until c->unload below). */
	{
		extern void genesis_menu_arm_reverse(const unsigned short *, unsigned, unsigned, unsigned);
		if (fr.video && fr.width && fr.height)
			genesis_menu_arm_reverse((const unsigned short *)fr.video, fr.width, fr.height, fr.pitch / 2u);
	}
	ayaneo_dsi_set_vfp(GEN_DEFAULT_VFP);   /* panel back to 59.749 Hz for the menu / other cores */
	{ extern void ayaneo_snes_rsz_restore(void); ayaneo_snes_rsz_restore(); }   /* RSZ back to 1:1 (else Close hangs / carousel shears) */
	ayaneo_menu_overlay(0, 0);   /* disable the overlay BEFORE returning to the carousel (else the
				      * stale Pico panel composites over the selector - see CORE_PORTING_NOTES) */
	ayaneo_hud_set(0, 0);   /* clear the FF/RW badge on exit */
	ayaneo_set_cpu_mhz(saved_mhz);   /* restore the menu's idle clock */
	genesis_settings_flush();   /* write any pending volume/brightness change before leaving */

	/* persist SRAM + a suspend state so the next launch resumes */
	if (c->sram_ptr() && c->sram_size())
		gba_sd_write_sav(vol, rom->name, c->sram_ptr(), c->sram_size());
	ssz = c->state_size();
	if (ssz && ssz <= GEN_STATE_CAP && c->state_save((void *)GEN_STATE_BUF, ssz) == 0)
		gba_sd_write_named(vol, "/states/genesis", rom->name, "sus",
				   (const unsigned char *)GEN_STATE_BUF, ssz);
	c->unload();
	ayaneo_menu_audio_silence();
}

/* Headless core benchmark: load the ROM, run `frames` UNCAPPED at a fixed safe clock (1400 MHz, like the
 * SNES uncapped bench - sustained 100% duty at 1800+ browns the core out), and record the per-frame emu
 * cost + fps + a state save/load round-trip. Publishes an IMPLIED rewind speed (16.7ms / (load+run)) so an
 * autonomous optimization loop has one stable number to push. No display/audio - pure core throughput. */
static void genesis_bench_body(fat_vol *vol, const gba_rom_entry *rom, int frames)
{
	const struct genesis_core_exports *c = genesis_core_load();
	unsigned romsz, ssz, rwp, saved_mhz; int i;
	unsigned int t0, t1;
	struct genesis_frame fr;
	g_gen_dbg_bench_fps = 0;
	if (!c) return;
	saved_mhz = ayaneo_get_cpu_mhz();
	ayaneo_set_cpu_mhz(1400);                     /* guaranteed-stable uncapped clock */
	c->heap_init((void *)GEN_HEAP_BASE, GEN_HEAP_SZ);
	c->init();
	romsz = gba_sd_load_rom(vol, rom, (unsigned char *)GEN_ROM_BUF, GEN_ROM_CAP);
	if (!romsz || c->load((const void *)GEN_ROM_BUF, romsz, rom_type_to_system(rom->type)) != 0) {
		c->unload(); ayaneo_set_cpu_mhz(saved_mhz); return;
	}
	for (i = 0; i < 90; i++) c->run(&fr);          /* warm up past the BIOS/intro */
	t0 = gpt4_get_current_tick();
	for (i = 0; i < frames; i++) c->run(&fr);
	t1 = gpt4_get_current_tick();
	{
		unsigned int per = ((t1 - t0) / 13u) / (unsigned)(frames > 0 ? frames : 1);
		g_gen_dbg_bench_us  = per;
		g_gen_dbg_bench_fps = per ? (1000000u / per) : 0;
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
		g_gen_dbg_bench_crc = h;
	}
	/* Profile split: run frames with the VDP pixel render SKIPPED (set_av_skip novideo=1). frame - norender
	 * = the VDP render cost; norender ~= 68k+Z80+sound+VDP-timing. Tells us whether the pixel render or the
	 * CPU is the wall for the frame-emulate. */
	if (c->set_av_skip) {
		c->set_av_skip(1, 0);
		t0 = gpt4_get_current_tick();
		for (i = 0; i < frames; i++) c->run(&fr);
		t1 = gpt4_get_current_tick();
		c->set_av_skip(0, 0);
		g_gen_dbg_bench_norender_us = ((t1 - t0) / 13u) / (unsigned)(frames > 0 ? frames : 1);
	}
	ssz = c->state_size(); rwp = (ssz && ssz <= 0x00400000u) ? ssz : 0;
	if (rwp) {
		t0 = gpt4_get_current_tick(); c->state_save((void *)GEN_STATE_BUF, rwp); t1 = gpt4_get_current_tick();
		g_gen_dbg_bench_save_us = (t1 - t0) / 13u;
		t0 = gpt4_get_current_tick(); c->state_load((const void *)GEN_STATE_BUF, rwp); t1 = gpt4_get_current_tick();
		g_gen_dbg_bench_load_us = (t1 - t0) / 13u;
		/* FAST load (set_ra_fast -> reset_do_not_clear_buffers skips the render/vdp buffer memsets):
		 * measures how much of the load is those clears vs the actual chip re-init. If much cheaper, use
		 * the fast path for rewind + run-ahead. */
		if (c->set_ra_fast) {
			c->set_ra_fast(1);
			t0 = gpt4_get_current_tick(); c->state_load((const void *)GEN_STATE_BUF, rwp); t1 = gpt4_get_current_tick();
			c->set_ra_fast(0);
			g_gen_dbg_bench_loadfast_us = (t1 - t0) / 13u;
		}
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
					if (c->set_ra_fast) c->set_ra_fast(1);
					c->state_load(st2, sz2);
					if (c->set_ra_fast) c->set_ra_fast(0);
					if (c->sound_rebase) c->sound_rebase();
					c->run(&fr);
					ok = (fr.video && fr.width >= 160u && fr.height >= 100u);
				}
				t1 = gpt4_get_current_tick();
				ayaneo_rewind_end();
				g_gen_dbg_bench_rw6_us = (t1 - t0) / 13u;   /* real 6x rewind present cost @1400 */
				g_gen_dbg_bench_rwok   = ok ? 1u : 0u;
			}
		}
	}
	/* implied rewind x10: one present (~15500us usable) / (per-step = state_load + one re-emulate) */
	{
		unsigned int step = g_gen_dbg_bench_load_us + g_gen_dbg_bench_us;
		g_gen_dbg_bench_rwx10 = step ? (155000u / step) : 0;
	}
	g_gen_dbg_bench_mhz = ayaneo_get_cpu_mhz();
	c->unload();
	ayaneo_set_cpu_mhz(saved_mhz);
}

/* ---- dedicated emulation thread (see gbc_sd_run.c rationale: emu_thread's 64 KB overflows) ---- */
static thread_t *s_gen_thread;
static event_t   s_gen_kick, s_gen_done;
static fat_vol  *s_gen_vol;
static const gba_rom_entry *s_gen_rom;
static volatile int s_gen_bench_n;   /* >0: run the headless bench for N frames instead of a session */

static int genesis_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_gen_kick);
		if (s_gen_bench_n > 0) { genesis_bench_body(s_gen_vol, s_gen_rom, s_gen_bench_n); s_gen_bench_n = 0; }
		else genesis_session_body(s_gen_vol, s_gen_rom);
		event_signal(&s_gen_done, false);
	}
	return 0;
}

/* Called from emu_thread's ROM-select dispatch. Runs the game on the genesis_emu thread and blocks
 * here until it returns to the selector (AYA). */
void genesis_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	if (!s_gen_thread) {
		event_init(&s_gen_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_gen_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_gen_thread = thread_create("genesis_emu", &genesis_thread_fn, NULL,
					     DEFAULT_PRIORITY, 262144);
		if (!s_gen_thread) { genesis_session_body(vol, rom); return; }   /* fallback: inline */
		thread_resume(s_gen_thread);
	}
	s_gen_bench_n = 0;
	s_gen_vol = vol;
	s_gen_rom = rom;
	event_signal(&s_gen_kick, false);
	event_wait(&s_gen_done);
}

/* Headless benchmark of `rom` for `frames` uncapped frames on the genesis_emu thread (stack-safe).
 * Blocks until done; results land in the g_gen_dbg_bench_* globals (read via `oem gen-bench`). */
void genesis_sd_bench(fat_vol *vol, const gba_rom_entry *rom, int frames)
{
	if (!s_gen_thread) {
		event_init(&s_gen_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_gen_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_gen_thread = thread_create("genesis_emu", &genesis_thread_fn, NULL,
					     DEFAULT_PRIORITY, 262144);
		if (!s_gen_thread) { s_gen_bench_n = frames; genesis_bench_body(vol, rom, frames); s_gen_bench_n = 0; return; }
		thread_resume(s_gen_thread);
	}
	s_gen_bench_n = frames > 0 ? frames : 300;
	s_gen_vol = vol;
	s_gen_rom = rom;
	event_signal(&s_gen_kick, false);
	event_wait(&s_gen_done);
}
