/*
 * snes_sd_run.c - run a SNES ROM from the SD card through the loadable snes9x core.
 *
 * The GBA-from-SD selector hands a chosen ROM to gba_driver.c; when it is a SNES title
 * (roms/snes) the driver calls snes_sd_session() here. We load the snes9x core blob
 * (snes_core_load, boot_b), give it a DRAM heap, load the ROM (+ its .srm) from the card,
 * then run the frame loop: each frame the core produces an RGB565 image (256/512 wide) +
 * audio; we blit it (ayaneo_snes_show_frame) paced off the 13 MHz counter. Holding AYA
 * returns to the selector. Runs on its OWN thread (deep C++ would overflow emu_thread's
 * stack - see emu/CORE_PORTING_NOTES.md item 2). Only one core runs at a time, so we reuse
 * the 0x50000000 arena.
 *
 * First bring-up: video + input. Audio and the in-game menu/run-ahead come next.
 */
#include "snes_core_abi.h"
#include "../gba/sd_fat.h"
#include "../gba/gba_sd_save.h"
#include <kernel/thread.h>
#include <kernel/event.h>

extern const struct snes_core_exports *snes_core_load(void);   /* snes_core_loader.c */
extern void     ayaneo_snes_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern unsigned int ayaneo_get_cpu_mhz(void);
extern void         ayaneo_set_cpu_mhz(unsigned int mhz);   /* ARM-PLL OPP (see gba_driver s_cpu_opp) */
extern void     ayaneo_gbc_audio_init(void);       /* 48 kHz AFE ring (shared with GB/GBC) */
extern void     ayaneo_snes_audio_reset(void);
extern void     ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_display_prepare(void);
extern void     ayaneo_dsi_set_vfp(unsigned int vfp);   /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);           /* read-back to validate the switch */
volatile unsigned g_snes_dbg_vfp;                       /* the live DSI vfp during the session */
extern int      priamry_display_wait_for_vsync(void);   /* primary_display.c (name has the typo) */
extern unsigned gpt4_get_current_tick(void);
extern int      ayaneo_get_lcd_filter(void);            /* 0 Off,1 Scanlines,2 Grid,3 Dot Matrix */
extern void     ayaneo_set_lcd_filter(int f);
extern int      ayaneo_get_preempt_frames(void);        /* run-ahead depth 0..3 (shared setting) */
extern void     ayaneo_set_preempt_frames(int v);
extern unsigned ayaneo_get_snes_opts(void);             /* packed aspect|overscan<<8|audio<<16|hires<<24 */
extern void     ayaneo_set_snes_opts(unsigned v);
extern int      ayaneo_get_snes_slot(void);             /* persisted manual save-state slot 0..9 */
extern void     ayaneo_set_snes_slot(int v);

/* Run-ahead: present pf frames into the future with the current input, then rewind to the
 * committed frame (mirrors GB/GBC/GBA "Preemptive Frames"). Runs pf+1 full emulations per
 * displayed frame, so escalate the ARM clock with the tier. Off while the menu is open. */
#define SNES_AHEAD_BUF 0x53000000u   /* run-ahead state (shares the mapped-arena scratch slot) */
#define SNES_AHEAD_CAP 0x00200000u   /* 2 MB room before the menu wallpaper cache (0x53200000) */
static const unsigned s_snes_ra_opp[4] = { 1400, 1600, 1800, 2000 };   /* clock per pf tier */
static const char *snes_ra_name(int pf)
{ return pf == 1 ? "Balanced" : pf == 2 ? "Responsive" : pf == 3 ? "Max" : "Off"; }

/* Benchmark (uncap): run the emulator with no vsync pacing and no audio, counting
 * emulated frames per second so CPU-clock changes are measurable (mirrors the GBC/GBA
 * "Benchmark (Uncap)" item). ayaneo_snes_show_frame skips its vsync wait when this is set. */
volatile int g_snes_benchmark;
static volatile int s_snes_fps;
int snes_benchmark_on(void) { return g_snes_benchmark; }

/* Measured SNES panel refresh (Hz*1000), an 8-frame average from the vsync-locked present;
 * shown read-only in the Pico menu and via oem diag. Same tick math as the menu's
 * g_dbg_hz1000 - comparing the two validates the per-core LCM vfp switch (menu vfp 23 ~=
 * 59.749 Hz vs SNES vfp 17 ~= 60.11 Hz). */
volatile unsigned g_snes_dbg_hz1000;
#define s_snes_hz1000 g_snes_dbg_hz1000

/* Manual CPU-clock OPP grid (matches gba_driver.c s_cpu_opp); the SNES session floors at
 * 1400 MHz but the player can raise it here. Not persisted. */
static const unsigned s_snes_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static void snes_cpu_step(int dir)
{
	int n = (int)(sizeof s_snes_cpu_opp / sizeof s_snes_cpu_opp[0]), i, best = 0;
	unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u;
	for (i = 0; i < n; i++) { unsigned d = s_snes_cpu_opp[i] > cur ? s_snes_cpu_opp[i] - cur : cur - s_snes_cpu_opp[i];
		if (d < bd) { bd = d; best = i; } }
	best += dir; if (best < 0) best = 0; if (best >= n) best = n - 1;
	ayaneo_set_cpu_mhz(s_snes_cpu_opp[best]);
}

/* Panel vertical-front-porch per refresh rate. Stock vfp 23 -> vtotal 999 -> 59.749 Hz
 * (GB/GBC/GBA/menu). SNES uses vfp 17 -> vtotal 993 -> ~60.11 Hz (0.02% off its native
 * 60.0988 Hz) so the vsync-locked present in ayaneo_snes_show_frame is smooth. */
#define SNES_VFP      17u
#define DEFAULT_VFP   23u
extern void     mtk_wdt_restart(void);
extern void     mtk_wdt_disable(void);
extern int      mt_get_gpio_in(unsigned pin);

/* pad GPIOs (match gba_driver.c). Active-low. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#define GPIO_AYA       86
#define GPIO_LB        92
#define GPIO_RB        81
#define GPIO_SELECT    90
#define GPIO_START     91
#define GPIO_B         82
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#define GPIO_A         83
#define GPIO_X         85      /* physical X (84/85 read swapped vs the caps) */
#define GPIO_Y         84      /* physical Y */
#define GPIO_R2        57      /* second-stage right trigger = fast-forward (matches GBA) */

/* RETRO_DEVICE_ID_JOYPAD_* bit positions (what the core's input_state_cb reads). */
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

/* ---- in-game overlay menu (GammaOS Pico), mirrors the GB/GBC one ---- */
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch, int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch, int x, int y, int scale, unsigned int argb, const char *s);
extern int  ayaneo_brightness_pct(void);
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern void ayaneo_menu_settings_persist(void);

volatile int g_snes_menu_open;   /* gates ayaneo_snes_pad_mask so the game ignores menu input */

/* diagnostics, read via `fastboot oem diag` after a launch (why did the session exit?) */
volatile unsigned g_snes_dbg_stage;   /* 1 blob,2 rom,3 heap,4 loading,5 running */
volatile unsigned g_snes_dbg_romsz;
volatile unsigned g_snes_dbg_loadrc;  /* c->load() return (0 = ok, 0xFF = not reached) */
volatile unsigned g_snes_dbg_frames;
volatile unsigned g_snes_dbg_w, g_snes_dbg_h;
volatile unsigned g_snes_dbg_exit;    /* 0 running,1 aya-hold,2 blob,3 rom,4 load */
volatile unsigned g_snes_dbg_pitch;   /* f.pitch of the last frame */
volatile unsigned g_snes_dbg_nz;      /* non-zero pixels in a full-frame sample (0 = black) */
volatile unsigned g_snes_dbg_changed; /* count of frames whose content hash changed (0 = frozen) */
static   unsigned s_snes_dbg_lasthash;
volatile unsigned g_snes_test_limit;  /* >0: run this many frames then exit (oem snes-launch) */
volatile unsigned g_snes_dbg_audframes; /* total audio sample-pairs submitted (0 = APU silent) */
/* headless save-state self-test (run at the end of an oem snes-launch frame-limited run):
 * ss_size = state_size bytes; ss_core = 1 if state_save then state_load both returned 0;
 * ss_sd = 1 if SD write -> read-back -> memcmp matched exactly (SD path integrity). */
volatile unsigned g_snes_dbg_ss_size, g_snes_dbg_ss_core, g_snes_dbg_ss_sd;
volatile int      g_snes_dbg_ra;        /* forced run-ahead depth in the headless test (oem snes-ra) */
volatile unsigned g_snes_dbg_heapused;  /* arena bytes in use at test end (run-ahead leak check) */

/* Physical pad -> SNES button bitmask (imports.read_buttons). Returns 0 while the in-game
 * menu is open so navigation keys do not leak into the game. */
unsigned ayaneo_snes_pad_mask(void)
{
	unsigned m = 0;
	if (g_snes_menu_open) return 0;
	if (PRESSED(GPIO_B))      m |= 1u << RJ_B;
	if (PRESSED(GPIO_Y))      m |= 1u << RJ_Y;
	if (PRESSED(GPIO_SELECT)) m |= 1u << RJ_SELECT;
	if (PRESSED(GPIO_START))  m |= 1u << RJ_START;
	if (PRESSED(GPIO_UP))     m |= 1u << RJ_UP;
	if (PRESSED(GPIO_DOWN))   m |= 1u << RJ_DOWN;
	if (PRESSED(GPIO_LEFT))   m |= 1u << RJ_LEFT;
	if (PRESSED(GPIO_RIGHT))  m |= 1u << RJ_RIGHT;
	if (PRESSED(GPIO_A))      m |= 1u << RJ_A;
	if (PRESSED(GPIO_X))      m |= 1u << RJ_X;
	if (PRESSED(GPIO_LB))     m |= 1u << RJ_L;
	if (PRESSED(GPIO_RB))     m |= 1u << RJ_R;
	return m;
}

/* Arena (0x50000000, 64 MB, shared - only one core runs at a time). */
#define SNES_ROM_BUF   0x50000000u
#define SNES_ROM_CAP   0x00800000u        /* 8 MB (largest SNES carts ~6 MB) */
#define SNES_HEAP_BASE 0x50800000u
#define SNES_HEAP_SZ   0x02400000u        /* 36 MB for snes9x (SMW peaks ~19 MB); shrunk from
                                           * 48 MB to free mapped arena tail for the savestate
                                           * buffer - see SNES_STATE_BUF. */
/* Save-state buffer. The SNES session's MMU maps ONLY its own arena [0x50800000,0x53800000)
 * as WB during a game; addresses at/above 0x53800000 (the old buffer spot, AND the menu's
 * 0x54xxxxxx transition buffers) are UNMAPPED then - writes vanish and reads return a
 * constant, which silently corrupted every savestate ("save states broken"). So the buffer
 * MUST live inside the mapped arena. The heap is shrunk to 36 MB (ends 0x53000000), leaving
 * the mapped tail [0x52C00000,0x53800000) free; put the 4 MB state buffer at 0x52C00000
 * (ends 0x53000000) and the self-test scratch above it - both below the menu's wallpaper
 * cache (0x53200000) so nothing overlaps. */
#define SNES_STATE_BUF 0x52C00000u
#define SNES_STATE_CAP 0x00400000u        /* 4 MB (snes9x state ~0.8 MB, more with SA-1/SuperFX) */
#define SNES_STATE_SCRATCH 0x53000000u    /* self-test SD readback scratch (mapped arena tail) */
#define RESET_HOLD_FRAMES 30              /* SELECT+START+L+R held ~0.5 s = soft reset */

/* ---- in-game menu: state, rendering, actions ---- */
static const struct snes_core_exports *s_menu_c;
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static int  s_msel;
static char s_mstat[48];
static int  s_save_slot;   /* manual Save/Load state slot 0..9 (suspend uses a separate "sus") */
static int  s_slot_used;   /* 1 if the current slot's file exists on the card (cached) */

/* build the "st<slot>" extension for a manual save-state slot. */
static void snes_slot_ext(char *e) { e[0] = 's'; e[1] = 't'; e[2] = (char)('0' + s_save_slot); e[3] = 0; }

/* refresh s_slot_used by probing the current slot's file (16-byte read; cheap, menu-time only). */
static void snes_slot_check(void)
{
	unsigned char tmp[16]; char ext[4];
	if (!s_menu_vol || !s_menu_rom) { s_slot_used = 0; return; }
	snes_slot_ext(ext);
	s_slot_used = gba_sd_read_named(s_menu_vol, "/states/snes", s_menu_rom->name, ext, tmp, sizeof tmp) > 0;
}
enum { SM_BRIGHT, SM_VOLUME, SM_FILTER, SM_ASPECT, SM_OVERSCAN, SM_AUDIO, SM_HIRES,
       SM_CPU, SM_RUNAHEAD, SM_BENCH, SM_PANEL,
       SM_SLOT, SM_SAVE, SM_LOAD, SM_RESET, SM_CLOSE, SM_COUNT };

static const char *snes_filter_name(int f)
{ return f == 1 ? "Scanlines" : f == 2 ? "LCD Grid" : f == 3 ? "Dot Matrix" : "Off"; }

/* snes9x core-option choices exposed in the Pico menu: {label, libretro value}. The
 * runner pushes the selected value with c->set_option(key, value); snes9x reflows on the
 * next frame (geometry for aspect/overscan). Index per item is tracked in s_opt_idx[]. */
struct snes_opt_choice { const char *label, *value; };
static const struct snes_opt_choice s_asp_ch[]  = { {"4:3","4:3"}, {"Pixel","uncorrected"}, {"NTSC","ntsc"}, {"PAL","pal"}, {"Stretch","4:3"} };
#define SNES_ASPECT_STRETCH 4   /* index of "Stretch": fill the whole 1280x960 panel, no bars */
static const struct snes_opt_choice s_ovs_ch[]  = { {"Crop 8px","enabled"}, {"Crop 12px","12_pixels"}, {"Crop 16px","16_pixels"}, {"Off","disabled"} };
static const struct snes_opt_choice s_aud_ch[]  = { {"Gaussian","gaussian"}, {"Cubic","cubic"}, {"Sinc","sinc"}, {"Linear","linear"}, {"None","none"} };
static const struct snes_opt_choice s_hib_ch[]  = { {"Off","disabled"}, {"Merge","merge"}, {"Blur","blur"} };
enum { OI_ASPECT, OI_OVERSCAN, OI_AUDIO, OI_HIRES, OI_N };
static struct { const char *key; const struct snes_opt_choice *ch; int n; } s_opt_def[OI_N] = {
	{ "snes9x_aspect",              s_asp_ch, 5 },
	{ "snes9x_overscan",           s_ovs_ch, 4 },
	{ "snes9x_audio_interpolation", s_aud_ch, 5 },
	{ "snes9x_hires_blend",        s_hib_ch, 3 },
};
static int s_opt_idx[OI_N];   /* current selection per option (defaults = index 0) */

/* aspect ratio (x1000) the display should stretch to; 0 = integer/pixel. Updated by the
 * runner from c->aspect_x1000() whenever aspect/overscan changes. Read by ayaneo_snes_show_frame. */
volatile unsigned g_snes_aspect_x1000;
/* 1 = "Stretch": the display fills the whole panel (no bars), overriding g_snes_aspect_x1000. */
volatile int g_snes_stretch;

static char *smput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *smputu(char *p, unsigned v) { char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n]; return p; }

static const char *sm_label(int i) { switch (i) {
	case SM_BRIGHT: return "Brightness"; case SM_VOLUME: return "Volume";
	case SM_FILTER: return "LCD Filter";
	case SM_ASPECT: return "Aspect Ratio"; case SM_OVERSCAN: return "Overscan";
	case SM_AUDIO:  return "Audio Filter";  case SM_HIRES:    return "Hi-Res Blend";
	case SM_CPU:    return "CPU Clock"; case SM_RUNAHEAD: return "Run-Ahead";
	case SM_BENCH:  return "Benchmark (Uncap)"; case SM_PANEL: return "Panel Refresh";
	case SM_SLOT:   return "Save Slot";
	case SM_SAVE:   return "Save State"; case SM_LOAD:   return "Load State";
	case SM_RESET:  return "Reset Game"; case SM_CLOSE:  return "Close"; } return ""; }
static const char *sm_value(int i, char *buf) { char *p = buf;
	switch (i) {
	case SM_BRIGHT: p = smputu(p, (unsigned)ayaneo_brightness_pct()); p = smput(p, "%"); break;
	case SM_VOLUME: p = smputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = smput(p, "%"); break;
	case SM_FILTER: p = smput(p, snes_filter_name(ayaneo_get_lcd_filter())); break;
	case SM_ASPECT: case SM_OVERSCAN: case SM_AUDIO: case SM_HIRES: {
		int oi = i - SM_ASPECT; p = smput(p, s_opt_def[oi].ch[s_opt_idx[oi]].label); break; }
	case SM_CPU: { static unsigned mhz, tick;   /* cache: PLL read-back low bits jitter */
		if (!mhz || tick-- == 0) { mhz = ayaneo_get_cpu_mhz(); tick = 40; }
		p = smputu(p, mhz); p = smput(p, " MHz"); break; }
	case SM_RUNAHEAD: p = smput(p, snes_ra_name(ayaneo_get_preempt_frames())); break;
	case SM_BENCH: if (g_snes_benchmark) { p = smputu(p, (unsigned)s_snes_fps); p = smput(p, " fps"); }
		else p = smput(p, "Off"); break;
	case SM_PANEL: { unsigned hz = s_snes_hz1000;   /* Hz*1000 -> "60.11 Hz" */
		p = smputu(p, hz / 1000); p = smput(p, ".");
		{ unsigned f = (hz % 1000) / 10; if (f < 10) p = smput(p, "0"); p = smputu(p, f); }
		p = smput(p, " Hz"); break; }
	case SM_SLOT: p = smputu(p, (unsigned)s_save_slot); p = smput(p, s_slot_used ? " used" : " empty"); break;
	case SM_SAVE: case SM_LOAD: p = smput(p, "[A]"); break;
	default: break; } *p = 0; return buf; }

int snes_menu_open(void) { return g_snes_menu_open; }

void snes_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int rowH = 38, panelW = 520, panelH = 84 + SM_COUNT * rowH + 42;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2, x = px + 28, y = py + 84, i;
	char val[48];
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "GammaOS Pico");
	for (i = 0; i < SM_COUNT; i++, y += rowH) {
		unsigned int fg = (i == s_msel) ? 0xFF101018u : 0xFFC8D0E0u; int vw;
		if (i == s_msel) ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, y, 2, fg, sm_label(i));
		sm_value(i, val); for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	if (s_mstat[0]) ayaneo_text(buf, pitch, x, py + panelH - 40, 2, 0xFF80E080u, s_mstat);
	ayaneo_text(buf, pitch, x, py + panelH - 16, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

/* returns 1 to close the menu (Reset/Close), else 0 */
static int sm_change(int i, int dir, int act)
{
	s_mstat[0] = 0;
	switch (i) {
	case SM_BRIGHT: if (dir) { ayaneo_brightness_step(dir); ayaneo_menu_settings_persist(); } break;
	case SM_VOLUME: if (dir) { ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); ayaneo_menu_settings_persist(); } break;
	case SM_FILTER: if (dir) { ayaneo_set_lcd_filter((ayaneo_get_lcd_filter() + dir + 4) % 4); ayaneo_menu_settings_persist(); } break;
	case SM_ASPECT: case SM_OVERSCAN: case SM_AUDIO: case SM_HIRES:
		if (dir && s_menu_c->set_option) {
			int oi = i - SM_ASPECT, n = s_opt_def[oi].n, k;
			unsigned packed = 0;
			s_opt_idx[oi] = (s_opt_idx[oi] + dir + n) % n;
			s_menu_c->set_option(s_opt_def[oi].key, s_opt_def[oi].ch[s_opt_idx[oi]].value);
			if (oi == OI_ASPECT) g_snes_stretch = (s_opt_idx[OI_ASPECT] == SNES_ASPECT_STRETCH);
			for (k = 0; k < OI_N; k++) packed |= (unsigned)(s_opt_idx[k] & 0xFF) << (k * 8);
			ayaneo_set_snes_opts(packed);   /* persist all picks */
			ayaneo_menu_settings_persist();
		}
		break;
	case SM_CPU:    if (dir) snes_cpu_step(dir); break;   /* not persisted (session floors at 1400) */
	case SM_RUNAHEAD: if (dir) {
		int pf = (ayaneo_get_preempt_frames() + dir + 4) % 4;
		ayaneo_set_preempt_frames(pf);
		ayaneo_set_cpu_mhz(s_snes_ra_opp[pf]);   /* escalate the clock for pf+1 emulations/frame */
		ayaneo_menu_settings_persist();
	} break;
	case SM_BENCH:  if (dir || act) g_snes_benchmark = !g_snes_benchmark; break;
	case SM_PANEL:  break;   /* read-only */
	case SM_SLOT: if (dir) { s_save_slot = (s_save_slot + dir + 10) % 10;
			ayaneo_set_snes_slot(s_save_slot); ayaneo_menu_settings_persist();
			snes_slot_check(); } break;   /* refresh used/empty for the new slot */
	case SM_SAVE: if (act) { unsigned char *st = (unsigned char *)SNES_STATE_BUF; unsigned ssz = s_menu_c->state_size();
			char ext[4]; int ok; void *m = s_menu_c->heap_mark ? s_menu_c->heap_mark() : 0;
			snes_slot_ext(ext);
			ok = (ssz && ssz <= SNES_STATE_CAP && s_menu_c->state_save(st, ssz) == 0 &&
			      gba_sd_write_named(s_menu_vol, "/states/snes", s_menu_rom->name, ext, st, ssz) == 0);
			smput(s_mstat, ok ? "State saved" : "Save failed");
			if (ok) s_slot_used = 1;
			if (m && s_menu_c->heap_reset) s_menu_c->heap_reset(m); } break;
	case SM_LOAD: if (act) { unsigned char *st = (unsigned char *)SNES_STATE_BUF;
			char ext[4]; unsigned n; void *m = s_menu_c->heap_mark ? s_menu_c->heap_mark() : 0;
			snes_slot_ext(ext);
			n = gba_sd_read_named(s_menu_vol, "/states/snes", s_menu_rom->name, ext, st, SNES_STATE_CAP);
			smput(s_mstat, (n && s_menu_c->state_load(st, n) == 0) ? "State loaded" : "No save state");
			if (m && s_menu_c->heap_reset) s_menu_c->heap_reset(m); } break;
	case SM_RESET: if (act) { s_menu_c->reset(); return 1; } break;
	case SM_CLOSE: if (act) return 1; break;
	}
	return 0;
}

static void snes_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct snes_core_exports *c;
	unsigned char *rombuf = (unsigned char *)SNES_ROM_BUF;
	unsigned romsz;
	unsigned bw = 0, bh = 0, mw = 0, mh = 0, sr = 0;
	unsigned saved_mhz = 0;
	int aya_hold = 0;

	g_snes_dbg_stage = 1; g_snes_dbg_frames = 0; g_snes_dbg_exit = 0; g_snes_dbg_audframes = 0;
	g_snes_dbg_heapused = 0;
	g_snes_dbg_w = g_snes_dbg_h = 0; g_snes_dbg_loadrc = 0xFF;

	c = snes_core_load();
	if (!c) { g_snes_dbg_exit = 2; return; }   /* blob load failed */
	g_snes_dbg_stage = 2;

	romsz = gba_sd_load_rom(vol, rom, rombuf, SNES_ROM_CAP);
	g_snes_dbg_romsz = romsz;
	if (!romsz) { g_snes_dbg_exit = 3; return; }   /* ROM read failed */
	g_snes_dbg_stage = 3;

	c->heap_init((void *)SNES_HEAP_BASE, SNES_HEAP_SZ);
	c->init();
	g_snes_dbg_stage = 4;
	g_snes_dbg_loadrc = (unsigned)c->load(rombuf, romsz);
	if (g_snes_dbg_loadrc != 0) { g_snes_dbg_exit = 4; return; }   /* core load failed */
	g_snes_dbg_stage = 5;
	c->av_info(&bw, &bh, &mw, &mh, &sr);
	g_snes_dbg_w = bw; g_snes_dbg_h = bh;

	/* cartridge battery save (.srm) from the card, if any. */
	{
		unsigned sz = c->sram_size();
		void *p = c->sram_ptr();
		if (p && sz && sz <= 0x20000u)   /* SNES SRAM max 128 KB */
			gba_sd_read_named(vol, "/saves/snes", rom->name, "srm", (unsigned char *)p, sz);
	}

	/* Suspend/resume: reload the AUTO save STATE from the last exit so the game resumes
	 * where it left off, unless B is held at launch (start fresh). Mirrors the GB/GBC flow.
	 * IMPORTANT: the auto slot is "sus", SEPARATE from the manual "st0" slot the Pico menu
	 * Save/Load use - otherwise every exit would clobber the player's manual save (that was
	 * the "save states are broken" bug). The host round-trip proved snes9x
	 * serialize/unserialize is deterministic. */
	if (!PRESSED(GPIO_B)) {
		unsigned char *st = (unsigned char *)SNES_STATE_BUF;
		unsigned n = gba_sd_read_named(vol, "/states/snes", rom->name, "sus", st, SNES_STATE_CAP);
		if (n) c->state_load(st, n);
	}

	/* snes9x mainline is accuracy-first (full 65816 + SPC700 + PPU + DSP each frame), which
	 * is heavy on the A55; the menu/other cores idle around 1200 MHz. Raise the ARM PLL to
	 * at least 1400 MHz for the SNES session so plain-mapper games hit full speed, but never
	 * downclock a user who manually picked a higher OPP. Restored on session exit. NOTE:
	 * ayaneo_set_cpu_mhz only moves the PLL, not core voltage - 1400 is the OPP just above
	 * the boot Vproc point, the same one the "Max" preempt tier uses. */
	saved_mhz = ayaneo_get_cpu_mhz();
	if (saved_mhz < 1400) ayaneo_set_cpu_mhz(1400);

	/* If the menu armed a launch punch (snapshot at 0x54000000, gba_punch_ready), DON'T
	 * clear to black - keep the frozen menu on screen so the growing gameplay circle opens
	 * over it (matches GBA/GBC). Otherwise blank the panel. */
	{ extern int gba_punch_ready; if (!gba_punch_ready) ayaneo_display_prepare(); }
	ayaneo_gbc_audio_init();
	ayaneo_snes_audio_reset();
	mtk_wdt_disable();
	/* Switch the panel to ~60.11 Hz for SNES (vfp swap). The vsync-locked present in
	 * ayaneo_snes_show_frame then paces emulation to the panel scan - smooth, tear-free,
	 * no 13 MHz busy-wait needed. Restored to 59.749 Hz on exit below. */
	ayaneo_dsi_set_vfp(SNES_VFP);
	g_snes_dbg_vfp = ayaneo_dsi_get_vfp();   /* read-back: should equal SNES_VFP (17) */

	/* hand the in-game menu this session's context */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom;
	s_msel = 0; s_mstat[0] = 0; g_snes_menu_open = 0;
	s_save_slot = ayaneo_get_snes_slot();   /* restore the last-used manual save slot */
	snes_slot_check();                      /* seed the used/empty indicator for it */
	/* Restore the persisted snes9x option picks (aspect/overscan/audio/hires) and push them
	 * to the core so a game launches with the player's last choices, not defaults. */
	{
		unsigned packed = ayaneo_get_snes_opts();
		int oi;
		for (oi = 0; oi < OI_N; oi++) {
			int idx = (int)((packed >> (oi * 8)) & 0xFF);
			if (idx < 0 || idx >= s_opt_def[oi].n) idx = 0;
			s_opt_idx[oi] = idx;
			if (idx != 0 && c->set_option)   /* index 0 == core default -> no need to push */
				c->set_option(s_opt_def[oi].key, s_opt_def[oi].ch[idx].value);
		}
		g_snes_stretch = (s_opt_idx[OI_ASPECT] == SNES_ASPECT_STRETCH);
	}
	g_snes_aspect_x1000 = (c->aspect_x1000) ? c->aspect_x1000() : 0;

	/* Launch punch-hole (matches GBA/GBC): run a few frames so real gameplay is on screen
	 * (not a black/boot frame), pre-render it full-screen, then grow a circle from 0 to the
	 * panel diagonal over the frozen menu snapshot at 0x54000000. Skipped if the menu didn't
	 * arm it (e.g. oem snes-launch). */
	{
		extern int  gba_punch_ready;
		extern void ayaneo_snes_punch_prerender(const unsigned short *, unsigned, unsigned, unsigned);
		extern void ayaneo_gba_punch_frame_pre(const unsigned int *, int);
		if (gba_punch_ready) {
			struct snes_frame pf; int i, w; pf.video = 0;
			gba_punch_ready = 0;
			for (w = 0; w < 15; w++) { c->run(&pf); if (pf.video && pf.width && w >= 3) break; }
			if (c->aspect_x1000) g_snes_aspect_x1000 = c->aspect_x1000();
			if (pf.video && pf.width && pf.height) {
				ayaneo_snes_punch_prerender((const unsigned short *)pf.video, pf.width, pf.height, pf.pitch / 2u);
				for (i = 1; i <= 20; i++) {
					int r = 820 * i / 20; if (r < 1) r = 1;
					ayaneo_gba_punch_frame_pre((const unsigned int *)0x54000000u, r);
					mtk_wdt_restart();
				}
			}
		}
	}

	/* Run-ahead enable gate: the state must fit in the 2 MB SNES_AHEAD_BUF slot. ra_ssz != 0
	 * enables run-ahead; the save/load themselves pass the full buffer CAPACITY (not this
	 * one-shot size) so a game whose state grows later never truncates - unfreeze stops at
	 * the real end regardless. */
	unsigned ra_ssz = c->state_size();
	if (ra_ssz == 0 || ra_ssz > SNES_AHEAD_CAP) ra_ssz = 0;
	{ int pf0 = ayaneo_get_preempt_frames();   /* honour a persisted tier: escalate the clock */
	  if (pf0 > 0 && pf0 <= 3 && ayaneo_get_cpu_mhz() < s_snes_ra_opp[pf0]) ayaneo_set_cpu_mhz(s_snes_ra_opp[pf0]); }

	int reset_hold = 0, aya_prev = 0, ff_prev = 0;
	int up_p = 0, dn_p = 0, lt_p = 0, rt_p = 0, a_p = 0, b_p = 0;
	for (;;) {
		struct snes_frame f;
		int aya;
		mtk_wdt_restart();

		/* The game keeps running under the menu (frame + audio keep flowing); its input
		 * is gated off in ayaneo_snes_pad_mask while the menu is open. */
		c->run(&f);
		g_snes_dbg_frames++;
		/* keep the display's target aspect current (cheap; refreshed periodically) - it
		 * changes when the player switches Aspect Ratio or Overscan in the menu. */
		if (c->aspect_x1000 && (g_snes_dbg_frames & 15u) == 0) g_snes_aspect_x1000 = c->aspect_x1000();
		if (g_snes_test_limit && g_snes_dbg_frames >= g_snes_test_limit) { g_snes_dbg_exit = 5; }
		if (f.video && f.width && f.height) {
			g_snes_dbg_w = f.width; g_snes_dbg_h = f.height; g_snes_dbg_pitch = f.pitch;
			{	/* full-frame sample: non-zero pixel count + a content hash (to tell a
				 * static black frame from live-but-mis-displayed content). */
				const unsigned short *p = (const unsigned short *)f.video;
				unsigned stride = f.pitch / 2u, y, x, nz = 0, hash = 2166136261u;
				for (y = 0; y < f.height; y += 8)
					for (x = 0; x < f.width; x += 8) {
						unsigned v = p[y * stride + x];
						if (v) nz++;
						hash = (hash ^ v) * 16777619u;
					}
				g_snes_dbg_nz = nz;
				if (hash != s_snes_dbg_lasthash) { g_snes_dbg_changed++; s_snes_dbg_lasthash = hash; }
			}
		}
		/* Fast-forward: hold R2 (second-stage right trigger, matches GBA) to run the
		 * emulation flat out. Present sparsely (1 in 8) and skip the vsync wait on the other
		 * frames so nothing paces the loop; audio and run-ahead are suppressed. Off under the
		 * menu, while benchmarking, or in the headless test. */
		int ff = PRESSED(GPIO_R2) && !g_snes_menu_open && !g_snes_benchmark && !g_snes_test_limit;
		/* On FF entry, clear the AFE ring once so the DMA loops SILENCE (audio is suppressed
		 * while fast-forwarding); without this it drones the last 341 ms buffer. */
		if (ff && !ff_prev) ayaneo_menu_audio_silence();
		ff_prev = ff;

		/* Committed-frame audio submitted BEFORE any run-ahead look-ahead overwrites the
		 * blob's audio buffer; muted while benchmarking or fast-forwarding so the ring never
		 * throttles the uncapped loop. */
		if (f.audio && f.frames && !g_snes_benchmark && !ff) {
			g_snes_dbg_audframes += f.frames;
			ayaneo_snes_audio_submit(f.audio, f.frames, sr ? sr : 32040u);
		}
		/* Run-ahead: advance the DISPLAY pf frames into the future with the current input,
		 * then rewind so the real emulation still advances exactly one frame per loop. Off
		 * under the menu (do not race ahead behind the overlay), while benchmarking/fast-
		 * forwarding, or in the headless test. Look-ahead frames are muted. */
		{
			extern volatile int g_snes_dbg_ra;   /* oem snes-ra:N forces run-ahead N in the headless test */
			int pf = 0;
			int i;
			void *hmark = 0;
			if (!ff && !g_snes_menu_open && !g_snes_benchmark && ra_ssz)
				pf = g_snes_test_limit ? g_snes_dbg_ra : ayaneo_get_preempt_frames();
			if (pf > 0) {
				/* Reclaim serialize/unserialize temporaries each frame: snes9x `new`s ~15
				 * block buffers per state op and the bump arena never frees, so without this
				 * run-ahead leaks ~0.5 MB/frame and crashes when the arena runs out. */
				if (c->heap_mark) hmark = c->heap_mark();
				c->state_save((void *)SNES_AHEAD_BUF, SNES_AHEAD_CAP);
				for (i = 0; i < pf; i++) c->run(&f);
			}
			if (ff) {
				/* uncapped: present ~every 8th frame (one vsync per 8 emulated frames),
				 * skip present/vsync otherwise so emulation runs as fast as the CPU allows. */
				if ((g_snes_dbg_frames & 7u) == 0 && f.video && f.width && f.height)
					ayaneo_snes_show_frame((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			} else if (f.video && f.width && f.height)
				ayaneo_snes_show_frame((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			else if (!g_snes_benchmark)
				priamry_display_wait_for_vsync();   /* keep pacing if a frame was dropped */
			if (pf > 0) {
				c->state_load((const void *)SNES_AHEAD_BUF, SNES_AHEAD_CAP);   /* rewind */
				if (hmark && c->heap_reset) c->heap_reset(hmark);      /* free the temporaries */
			}
			/* leak watch (headless test): peak arena usage should stay FLAT with run-ahead
			 * on if the mark/reset works; without it, it climbs ~0.5 MB/frame. */
			if (g_snes_test_limit && c->dbg_get) {
				unsigned hu = c->dbg_get(0);
				if (hu > g_snes_dbg_heapused) g_snes_dbg_heapused = hu;
			}
		}

		/* Timing off the 13 MHz gpt4 counter (NOT 812.5 kHz - that was the bug that showed
		 * ~2.7 Hz). Panel refresh: 8-frame average of the vsync-locked loop period, Hz*1000 =
		 * 8*13e9/ticks; reject doubled/outlier frames (period outside 8..23 ms) exactly like
		 * the GBA path. Benchmark FPS: frames * 13e6 / ticks over ~0.5 s. */
		{
			static unsigned h_acc, h_n, h_last;
			static unsigned b_acc, b_n, b_last;
			unsigned now = gpt4_get_current_tick();
			if (h_last) { unsigned lt = now - h_last;
				if (lt > 104000u && lt < 300000u) { h_acc += lt; if (++h_n >= 8) {
					if (h_acc) s_snes_hz1000 = (unsigned)(104000000000ULL / (unsigned long long)h_acc);
					h_acc = 0; h_n = 0; } } }
			h_last = now;
			if (g_snes_benchmark) {
				if (b_last) { b_acc += now - b_last; b_n++;
					if (b_acc >= 6500000u) {   /* ~0.5 s at 13 MHz */
						s_snes_fps = (int)((unsigned long long)b_n * 13000000ULL / b_acc);
						b_acc = 0; b_n = 0; } }
				b_last = now;
			} else { b_last = 0; b_acc = 0; b_n = 0; }
		}

		/* AYA taps toggle the menu; holding AYA ~1.5 s force-exits to the selector. */
		aya = PRESSED(GPIO_AYA);
		if (aya && !aya_prev) { g_snes_menu_open = !g_snes_menu_open; s_mstat[0] = 0; }
		aya_prev = aya;
		if (aya) { if (++aya_hold >= 90) {
			/* Arm the reverse punch: the menu re-entry shrinks this frozen frame back into
			 * the carousel (matches GBA/GBC). Not for the headless test-limit exit. */
			extern void snes_menu_arm_reverse(const unsigned short *, unsigned, unsigned, unsigned);
			if (!g_snes_test_limit && f.video && f.width && f.height)
				snes_menu_arm_reverse((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			g_snes_dbg_exit = 1; break;
		} } else aya_hold = 0;

		if (g_snes_menu_open) {
			int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
			int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
			int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
			if (up && !up_p) s_msel = (s_msel + SM_COUNT - 1) % SM_COUNT;
			if (dn && !dn_p) s_msel = (s_msel + 1) % SM_COUNT;
			if (lt && !lt_p) sm_change(s_msel, -1, 0);
			if (rt && !rt_p) sm_change(s_msel, +1, 0);
			if (a  && !a_p)  { if (sm_change(s_msel, 0, 1)) g_snes_menu_open = 0; }
			if (b  && !b_p)  g_snes_menu_open = 0;
			up_p = up; dn_p = dn; lt_p = lt; rt_p = rt; a_p = a; b_p = b;
			reset_hold = 0;
		} else {
			/* Soft reset: SELECT+START+L+R held ~0.5 s (mirrors GB/GBC/GBA). */
			if (PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) && PRESSED(GPIO_LB) && PRESSED(GPIO_RB)) {
				if (++reset_hold >= RESET_HOLD_FRAMES) { c->reset(); reset_hold = 0; }
			} else reset_hold = 0;
		}
		if (g_snes_test_limit && g_snes_dbg_frames >= g_snes_test_limit) break;   /* oem snes-launch */
	}

	/* Headless save-state regression check - only in frame-limited (oem snes-launch) mode,
	 * so it never touches normal play. Exercises the full chain: core freeze -> SD write ->
	 * SD read-back byte-compare -> core unfreeze. ss_core=1 & ss_sd=1 means savestates work. */
	if (g_snes_test_limit) {
		unsigned char *st  = (unsigned char *)SNES_STATE_BUF;
		unsigned char *st2 = (unsigned char *)SNES_STATE_SCRATCH;
		unsigned ssz = c->state_size();
		g_snes_dbg_ss_size = ssz; g_snes_dbg_ss_core = 0; g_snes_dbg_ss_sd = 0;
		if (ssz && ssz <= SNES_STATE_CAP && c->state_save(st, ssz) == 0) {
			int wr = gba_sd_write_named(vol, "/states/snes", rom->name, "sstst", st, ssz);
			unsigned rd = gba_sd_read_named(vol, "/states/snes", rom->name, "sstst", st2, SNES_STATE_CAP);
			if (wr == 0 && rd == ssz) {
				unsigned i, diff = 0;
				for (i = 0; i < ssz; i++) if (st[i] != st2[i]) { diff = 1; break; }
				g_snes_dbg_ss_sd = diff ? 0 : 1;
			}
			g_snes_dbg_ss_core = (c->state_load(st, ssz) == 0) ? 1 : 0;
		}
	}

	g_snes_menu_open = 0;
	g_snes_test_limit = 0;

	ayaneo_dsi_set_vfp(DEFAULT_VFP);   /* restore 59.749 Hz for the menu / other cores */
	if (saved_mhz) ayaneo_set_cpu_mhz(saved_mhz);   /* restore the pre-session ARM clock */

	/* Suspend: write the AUTO save STATE ("sus" slot) so the next launch resumes here. Kept
	 * separate from the manual "st0" slot so exiting never overwrites a manual Save State. */
	{
		unsigned char *st = (unsigned char *)SNES_STATE_BUF;
		unsigned ssz = c->state_size();
		if (ssz && ssz <= SNES_STATE_CAP && c->state_save(st, ssz) == 0)
			gba_sd_write_named(vol, "/states/snes", rom->name, "sus", st, ssz);
	}

	/* persist SRAM on exit */
	{
		unsigned sz = c->sram_size();
		void *p = c->sram_ptr();
		if (p && sz && sz <= 0x20000u)
			gba_sd_write_named(vol, "/saves/snes", rom->name, "srm", p, sz);
	}
	c->unload();
	ayaneo_menu_audio_silence();
}

/* ---- dedicated emulation thread (see CORE_PORTING_NOTES item 2) ---- */
static thread_t *s_snes_thread;
static event_t   s_snes_kick, s_snes_done;
static fat_vol            *s_snes_vol;
static const gba_rom_entry *s_snes_rom;

static int snes_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_snes_kick);
		snes_session_body(s_snes_vol, s_snes_rom);
		event_signal(&s_snes_done, false);
	}
	return 0;
}

void snes_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	s_snes_vol = vol; s_snes_rom = rom;
	if (!s_snes_thread) {
		event_init(&s_snes_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_snes_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_snes_thread = thread_create("snes_emu", snes_thread_fn, 0, DEFAULT_PRIORITY, 262144);
		thread_resume(s_snes_thread);
	}
	event_signal(&s_snes_kick, true);
	event_wait(&s_snes_done);
}
