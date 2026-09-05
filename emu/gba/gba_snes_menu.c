/*
 * GBA-from-SD ROM selector rendered with the REAL SNES-Classic-mini menu engine
 * (emu/gba/menu/snes_menu.c, imported 1:1 from the lk-snes-menu branch). The
 * authentic SNES home menu (menubar, wallpaper, fonts, sounds, carousel, HUD,
 * SUPER NINTENDO bar) drives the flow; only the per-card boxart is swapped for a
 * GBA cartridge placeholder and the roster is the microSD ROM list. A/Start
 * launches the focused ROM.
 *
 * gba_snes_menu_run() mirrors the old gba_menu_run() contract: returns the picked
 * ROM index, or -2 if the SNES asset pack is not present in boot_b (the caller
 * falls back to the plain list -> never-brick).
 *
 * Runs at 2000 MHz for 60fps (reuses the SNES perf: cached wallpaper + static
 * chrome + card-tile cache). During ROM-select the 64MB GBA emulator arena and
 * the free window above it are all unused, so the whole [0x50000000,0x56000000)
 * WB region backs the pack blob + node pools + render caches.
 */
#include "menu/snes_pack.h"
#include "menu/snes_render.h"
#include "menu/snes_menu.h"
#include "menu/snes_audio.h"
#include "menu/gba_name.h"          /* gba_clean_name (shared with host_render) */
#include "sd_fat.h"                 /* gba_rom_entry */
#include "gba_boxart_sd.h"          /* gba_boxart_load_sd */
#include <string.h>                 /* memcpy for the launch snapshot */
#include <stdint.h>                 /* uintptr_t, size_t */

extern fat_vol *gba_sd_menu_vol(void);   /* the mounted SD volume (gba_driver.c) */

/* Punch-hole launch transition (see ayaneo_gba_punch_frame in mt_disp_drv.c): the
 * menu captures its final frame here on launch, then the driver composites live
 * gameplay inside a growing circle over it. The snapshot lives just above the 64MB
 * gpSP arena [0x50000000,0x54000000) - free during the game, in the mapped WB
 * scratch window - so it survives the ROM load into the transition. */
#define GBA_PUNCH_SNAP_PA  0x54000000u
int gba_punch_ready = 0;            /* set on launch; consumed by the driver loop */

/* Reverse punch-hole transition (in-game "Close" -> back to the SNES selector): the
 * driver freezes the last game frame here and arms it; on re-entry the menu renders
 * one frame, then shrinks the frozen game into a hole that reveals the menu. Both
 * buffers sit in the free scratch above the menu caches. These three transition
 * buffers are FIXED; the boxart + card-tile caches are packed into the disjoint gaps
 * around them (see the SNES_CTILE2_PA/SNES_BOXART_PA/SNES_FCT_PA layout below). */
#include "menu/gba_punch.h"         /* gba_punch_composite */
extern unsigned int gpt4_get_current_tick(void);
extern void arch_clean_cache_range(unsigned long start, unsigned int len);
#define GBA_REVERSE_SNAP_PA 0x55000000u   /* rendered menu frame (fb-size) = reveal */
#define GBA_GAME_FREEZE_PA  0x55800000u   /* frozen 240x160 RGB565 game frame */
#define GBA_GAME_FULL_PA    0x55900000u   /* game pre-rendered full-screen BGRA (fast reverse) */
#define GBA_REVERSE_MS      180u
static int g_reverse_punch = 0;
static int g_reverse_is_gbc = 0;   /* 0 = GBA 240x160x5, 1 = GB/GBC 160x144x6, 2 = SNES, 3 = Genesis */
static int g_reverse_snes_w, g_reverse_snes_h, g_reverse_snes_scale;
static int g_reverse_gen_w, g_reverse_gen_h;   /* frozen Genesis frame dims (aspect-aware prerender) */
volatile int g_dbg_arm_cnt;   /* incremented each time the driver arms the reverse */

/* Called by the driver on "Close" with the last game frame; armed here, consumed by
 * the reverse-punch block at the top of gba_snes_menu_run. */
void gba_menu_arm_reverse(const unsigned short *game_frame)
{
	g_dbg_arm_cnt++;
	if (game_frame) {
		memcpy((void *)(uintptr_t)GBA_GAME_FREEZE_PA, game_frame, 240u * 160u * 2u);
		arch_clean_cache_range(GBA_GAME_FREEZE_PA, 240u * 160u * 2u);
		g_reverse_punch = 1;
		g_reverse_is_gbc = 0;
	}
}

/* GB/GBC variant: the frozen frame is 160x144 (the reverse renders it at 6x). */
void gbc_menu_arm_reverse(const unsigned short *game_frame)
{
	g_dbg_arm_cnt++;
	if (game_frame) {
		memcpy((void *)(uintptr_t)GBA_GAME_FREEZE_PA, game_frame, 160u * 144u * 2u);
		arch_clean_cache_range(GBA_GAME_FREEZE_PA, 160u * 144u * 2u);
		g_reverse_punch = 1;
		g_reverse_is_gbc = 1;
	}
}

/* SNES variant: the frozen frame is 256/512 wide (RGB565) with an arbitrary stride, so
 * pack it CONTIGUOUS into GBA_GAME_FREEZE_PA (gba_punch_prerender assumes stride == width).
 * The reverse renders it at integer scale (4x/2x) centred - a brief transition, so the
 * slight width difference from the live aspect-stretched view is imperceptible. */
void snes_menu_arm_reverse(const unsigned short *game_frame, unsigned sw, unsigned sh, unsigned spitch)
{
	g_dbg_arm_cnt++;
	if (game_frame && sw && sh && sw <= 512 && sh <= 512) {
		unsigned y; unsigned short *d = (unsigned short *)(uintptr_t)GBA_GAME_FREEZE_PA;
		for (y = 0; y < sh; y++) memcpy(d + y * sw, game_frame + y * spitch, sw * 2u);
		/* Flush to DRAM: this runs on the SNES emulation thread, but the reverse-punch that
		 * reads GBA_GAME_FREEZE_PA runs on the menu thread. Without the write-back the menu
		 * thread could read a stale (e.g. launch-time) copy from DRAM = the stale-frame bug. */
		arch_clean_cache_range(GBA_GAME_FREEZE_PA, sw * sh * 2u);
		g_reverse_snes_w = (int)sw; g_reverse_snes_h = (int)sh;
		g_reverse_snes_scale = (sw <= 256) ? 4 : 2;
		g_reverse_punch = 1;
		g_reverse_is_gbc = 2;
	}
}

/* Genesis variant: the frozen frame is up to 320x240 (RGB565) at the core's 720px stride, so pack
 * it CONTIGUOUS into GBA_GAME_FREEZE_PA and record its dims. The reverse renders it with the same
 * aspect-mode geometry as the live display (ayaneo_genesis_punch_prerender honours g_genesis_aspect). */
void genesis_menu_arm_reverse(const unsigned short *game_frame, unsigned sw, unsigned sh, unsigned spitch)
{
	g_dbg_arm_cnt++;
	if (game_frame && sw && sh && sw <= 512 && sh <= 512) {
		unsigned y; unsigned short *d = (unsigned short *)(uintptr_t)GBA_GAME_FREEZE_PA;
		for (y = 0; y < sh; y++) memcpy(d + y * sw, game_frame + y * spitch, sw * 2u);
		arch_clean_cache_range(GBA_GAME_FREEZE_PA, sw * sh * 2u);   /* emu thread -> menu thread (see above) */
		g_reverse_gen_w = (int)sw; g_reverse_gen_h = (int)sh;
		g_reverse_punch = 1;
		g_reverse_is_gbc = 3;
	}
}

/* ---- LK / driver primitives (externs; no LK headers pulled in here) ---- */
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern void ayaneo_fill_blend(unsigned int *buf, unsigned int pitch_w,
			      int x, int y, int w, int h, unsigned int argb, int alpha);
extern void mtk_wdt_restart(void);
/* Aspect-aware full-screen prerender (writes GBA_GAME_FULL_PA = 0x55900000), identical to the
 * launch forward-punch and the live SNES display geometry - reused for the reverse so the frozen
 * frame shrinks from EXACTLY what was last on screen (same size/aspect), not a centred integer box. */
extern void ayaneo_snes_punch_prerender(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern void thread_sleep(unsigned);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern int  partition_read(const char *name, unsigned long long off, void *buf, unsigned long len);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern unsigned int ayaneo_get_cpu_mhz(void);
extern void ayaneo_display_prepare(void);
extern void ayaneo_display_prepare_white(void);   /* whiteout handover (no black flash) */
extern void ayaneo_gbc_audio_init(void);
extern int  ayaneo_menu_audio_room(void);
extern void ayaneo_menu_audio_submit(const short *stereo, unsigned frames);
extern void ayaneo_menu_audio_silence(void);
extern int  ayaneo_get_mute_menu(void);
extern int  ayaneo_get_mute_bios(void);
extern void ayaneo_set_mute_menu(int v);
extern void ayaneo_set_mute_bios(int v);
extern void ayaneo_settings_save(void);
extern void ayaneo_menu_settings_persist(void);    /* persist to BOTH eMMC + SD (menu path) */
extern int  ayaneo_present_skip_framedone;         /* 0 = present blocks on vsync */
extern int  ayaneo_wait_frame_done(void);          /* block one vsync WITHOUT re-latching the OVL */
extern unsigned int gpt4_get_current_tick(void);   /* 13 MHz free-running counter */
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w, int x, int y,
			int scale, unsigned int argb, const char *s);

/* GBA button GPIOs (active-low), same panel as the SNES build's map */
extern int  mt_get_gpio_in(unsigned pin);
#define GP(n) (n)
#define K_LEFT 78
#define K_RIGHT 80
#define K_UP 89
#define K_DOWN 79
#define K_A 83
#define K_B 82
#define K_START 91
#define K_SELECT 90
#define K_LB 92    /* GBA L shoulder = page jump back */
#define K_RB 81    /* GBA R shoulder = page jump forward */
#define PRESSED(g) (mt_get_gpio_in(GP(g)) == 0)

extern int pmic_detect_powerkey(void);
extern void mt_power_off(void);

/* Volume / brightness control from the menu (same as the in-game poll): the two
 * hardware volume keys change volume; SELECT + a volume key changes brightness. A
 * small OSD bar is drawn over the menu, and the persist is deferred so the eMMC/SD
 * write does not stall the loop (mirror of gba_driver.c poll_volume). */
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_brightness_pct(void);
extern int  mtk_detect_key(unsigned short key);   /* HW keycode: 0x11 vol+, 0x00 vol- */
extern void ayaneo_settings_save(void);

/* Live debug metrics, read by the fastboot debug channel (emu/gba/menu_fastboot.c)
 * from its own thread while this menu runs. Plain globals: single writer (this
 * loop), racy reads are fine for diagnostics. */
volatile unsigned int g_dbg_render_us;     /* last snes_menu_render time (us) */
volatile unsigned int g_dbg_peak_us;       /* peak render over ~2s */
volatile unsigned int g_dbg_fps;           /* measured loop rate */
volatile unsigned int g_dbg_present_cnt;   /* frames actually presented (re-latched) */
volatile unsigned int g_dbg_hz1000;        /* measured panel refresh * 1000 (averaged) */
volatile int          g_dbg_focus;         /* current focused game index */
volatile unsigned int g_dbg_boxart_ok;     /* per-ROM boxart tiles that decoded from SD */
volatile unsigned int g_dbg_boxart_tot;    /* ROMs the boxart preload attempted */

/* Debug input injection (fastboot `oem key:<name>`): drive the live menu over USB.
 * One button at a time, held a few frames so the menu edge-detects a clean press.
 * 0..9 = L R U D A B Select Start LB RB; -1 = none. */
volatile int          g_dbg_force_launch;   /* fastboot `oem launch`: force-launch focused ROM */
volatile int          g_dbg_snes_launch;    /* fastboot `oem snes-launch`: launch the first SNES ROM */

/* fastboot `oem key:<name>`: inject one button into the live menu for a few frames so
 * the menu edge-detects a clean press. g_dbg_key = code 0..9 (L R U D A B Sel Start LB
 * RB), g_dbg_key_hold = frames left to hold. g_dbg_peak_reset zeroes the peak tracker so
 * the next movement's peak render us is measured clean (set at the start of an inject). */
volatile int          g_dbg_key = -1;
volatile int          g_dbg_key_hold;
volatile int          g_dbg_peak_reset;

/* ---- menu volume / brightness (OSD + deferred persist) ---- */
static int      s_av_kind;        /* 0 none, 1 volume, 2 brightness */
static int      s_av_pct;         /* 0..100 for the bar */
static unsigned s_av_until;       /* 13 MHz tick the OSD hides */
static int      s_av_dirty;       /* a change is pending persist */
static unsigned s_av_tick;        /* 13 MHz tick of the last change */

/* Read the two hardware volume keys (edge-detected) and adjust volume, or
 * brightness when SELECT is held. Sets the OSD + marks the persist dirty. */
static void menu_av_poll(void)
{
	static int up_prev, dn_prev;
	int up = mtk_detect_key(0x11);        /* VOL_UP  */
	int dn = mtk_detect_key(0x00);        /* VOL_DOWN */
	int sel = PRESSED(K_SELECT);
	int dir = 0;

	if (up && !up_prev) dir = +1;
	else if (dn && !dn_prev) dir = -1;
	if (dir) {
		if (sel) {
			s_av_kind = 2;
			s_av_pct = ayaneo_brightness_step(dir);
		} else {
			int v = ayaneo_gbc_audio_get_volume() + dir * 5;
			ayaneo_gbc_audio_set_volume(v);
			s_av_kind = 1;
			s_av_pct = ayaneo_gbc_audio_get_volume();   /* clamped 0..100 */
		}
		if (s_av_pct < 0) s_av_pct = 0; else if (s_av_pct > 100) s_av_pct = 100;
		s_av_until = gpt4_get_current_tick() + 19500000u;   /* ~1.5s */
		s_av_dirty = 1;
		s_av_tick = gpt4_get_current_tick();
	}
	up_prev = up; dn_prev = dn;

	/* flush the persist ~0.7s after the last change (off the key-repeat hot path).
	 * Persist to BOTH eMMC AND the SD card: in SD mode the boot settings are loaded
	 * from the SD card, so an eMMC-only write is lost on the next power-cycle. */
	if (s_av_dirty && (gpt4_get_current_tick() - s_av_tick) >= 9100000u) {
		s_av_dirty = 0;
		ayaneo_menu_settings_persist();
	}
}

/* Draw the volume/brightness OSD bar over the rendered menu while it is active. */
static void menu_av_draw(unsigned int *fb, unsigned int pitch, int W, int H)
{
	int bw = 420, bh = 46, bx, by, fillw;
	if (!s_av_kind) return;
	if ((int)(gpt4_get_current_tick() - s_av_until) >= 0) { s_av_kind = 0; return; }
	bx = (W - bw) / 2; by = H - 150;
	/* brightness = amber bar, volume = blue bar (so the two read apart without text) */
	ayaneo_fill(fb, pitch, bx - 8, by - 8, bw + 16, bh + 16, 0xE0101018u);   /* panel */
	ayaneo_fill(fb, pitch, bx, by, bw, bh, 0xFF303848u);                     /* track */
	fillw = bw * s_av_pct / 100; if (fillw < 0) fillw = 0; if (fillw > bw) fillw = bw;
	ayaneo_fill(fb, pitch, bx, by, fillw, bh, s_av_kind == 2 ? 0xFFF0A050u : 0xFF50A0F0u);
}

/* ---- boot_b SNES pack (SNSZ) location in the GBA-SD flow ----
 * In the SD flow the old ROM region at 0x01100000 is free (ROM comes from SD), so
 * the SNES menu pack lives there: ["SNSZ"][u32 rawlen][u32 complen][raw-deflate]. */
#define SNES_PART     "boot_b"
#define SNES_OFF      0x01100000ull
#define SNES_MAGIC    0x5A534E53u          /* "SNSZ" */

/* DRAM map (WB window [0x50000000,0x56000000) is all free during ROM-select). */
#define SNES_BLOB_PA   0x50000000u   /* decompressed pack (region ends at HOME_PA) */
#define SNES_HOME_PA   0x51800000u   /* home rnode pool (16MB) */
#define SNES_BG_PA     0x52800000u   /* bg rnode pool (2MB) */
#define SNES_COMP_PA   0x52A00000u   /* compressed staging (deflate, <=8MB) */
#define SNES_WP_PA     0x53200000u   /* wallpaper cache (1536*720*4 = 4.2MB) */
#define SNES_CHROME_PA 0x53700000u   /* static chrome cache (1280*960*4 = 4.7MB) */
#define SNES_WP43_PA   0x54000000u   /* 4:3 warped-wallpaper cache (2701*960*4 = 10.4MB, ends ~0x549E4400) */
#define SNES_CTILE_PA  0x54C00000u   /* (legacy cap-1 shared-tile region; unused with boxart) */
/*
 * Per-ROM boxart + the enlarged card-tile cache live in the WB-mapped
 * [0x4E,0x56)M window (0x56000000+ faults), and MUST NOT overlap the game/close
 * transition buffers, which are FIXED: reveal 0x55000000..+fb (fb=1280*960*4=
 * 0x4B0000), game freeze 0x55800000 (tiny), game-full 0x55900000..+fb. Earlier
 * these were sized into that territory (boxart at 0x54D00000 cap 90, ctile at
 * 0x55A40000), so a game->menu reverse-punch overwrote the tiles = corrupted
 * cards on exit. Repack into the disjoint gaps around the transition buffers:
 *   ctile   0x54A80000 .. 0x54FC6000  (cap 12, below the reveal buffer)
 *   reveal  0x55000000 .. 0x554B0000  (fixed)
 *   boxart  0x55500000 .. 0x557E4000  (cap 20, between reveal end and freeze)
 *   freeze  0x55800000                (fixed, tiny)
 *   full    0x55900000 .. 0x55DB0000  (fixed)
 *   fct     0x55E00000 .. 0x55E70800  (after game-full)
 * Boxart tiles are on the SD card, never in lk_a.
 */
#define SNES_CTILE2_PA   0x54A80000u /* enlarged card-tile cache (12 * 320*360*4 = 5.4MB) */
#define SNES_CTILE2_CAP  12
#define SNES_BOXART_PA   0x55500000u /* per-ROM boxart tiles (between reveal and freeze) */
#define SNES_BOXART_SLOT 0x25000u    /* bytes per tile slot (>= 224*224*3) */
#define SNES_BOXART_CAP  20          /* 20 * 0x25000 = 0x2E4000 -> ends 0x557E4000 < freeze */
#define SNES_FCT_PA      0x55E00000u /* focused (blue) card-body tile (320*360*4 = 450KB) */
/* the decompressed blob must stay strictly inside [BLOB_PA, HOME_PA); cap it 2MB
 * short of the 24MB region so it can never overrun the home node pool */
#define SNES_RAW_MAX   ((SNES_HOME_PA - SNES_BLOB_PA) - 2u * 1024 * 1024)  /* 22MB */
#define SNES_COMP_MAX  (8u  * 1024 * 1024)
#define HOME_CAP       (16u * 1024 * 1024 / (unsigned)sizeof(snes_rnode))
#define BG_CAP         (2u  * 1024 * 1024 / (unsigned)sizeof(snes_rnode))

static snes_pack s_pk;
static snes_menu s_menu;
static snes_mixer s_mix;
static int s_show_hud;   /* perf HUD off by default; Start+Select toggles it */
static int s_hud_combo;  /* edge-latch for the toggle combo */
static short s_mixbuf[16384 * 2];

/* roster name storage (stripped ".gba"), + a pointer table for the menu */
static char          s_names[128][128];
static const char   *s_nameptr[128];
static unsigned char s_types[128];   /* per-ROM console type (GBA_CONSOLE_*) for badges */

static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Read + inflate the SNES pack from boot_b into SNES_BLOB_PA, open it. 0 = ok. */
static int load_pack(void)
{
	unsigned char hdr[12];
	uint32_t magic, rawlen, complen;
	unsigned char *comp = (unsigned char *)SNES_COMP_PA;
	unsigned long zlen;
	if (partition_read(SNES_PART, SNES_OFF, hdr, 12) != 12) return -1;
	magic = rd32(hdr); rawlen = rd32(hdr + 4); complen = rd32(hdr + 8);
	if (magic != SNES_MAGIC || rawlen == 0 || rawlen > SNES_RAW_MAX ||
	    complen == 0 || complen > SNES_COMP_MAX) return -2;
	if (partition_read(SNES_PART, SNES_OFF + 12, comp, complen) != (long)complen) return -3;
	zlen = complen;
	if (zunzip(comp, &zlen, (void *)SNES_BLOB_PA, (int)rawlen, 0) != 0) return -4;
	if (snes_pack_open(&s_pk, (void *)SNES_BLOB_PA, rawlen) != 0) return -5;
	return 0;
}

/* Clean a ROM file name for the title: drop the ".gba" extension and the trailing
 * No-Intro region/dump tag groups " (USA)", " (Rev 1)", " [!]" ... (the SNES title
 * font also lacks '(' ')' ',' glyphs, so these would render as gaps). Trailing
 * spaces are trimmed. dst holds up to 128 chars. */
/* Build the display-name table from the SD roster (cleaned titles). */
static void build_names(const gba_rom_entry *roms, int nrom)
{
	int i;
	for (i = 0; i < nrom && i < 128; i++) {
		gba_clean_name(roms[i].name, s_names[i]);
		if (s_names[i][0] == 0) {                       /* pathological all-tag name */
			int L = 0; const char *nm = roms[i].name;
			while (nm[L] && L < 127) { s_names[i][L] = nm[L]; L++; }
			s_names[i][L] = 0;                       /* fall back to the raw name */
		}
		s_nameptr[i] = s_names[i];
		s_types[i] = roms[i].type;                      /* GB / GBC / GBA badge */
	}
}

static void play_sound(uint32_t h, int loop)
{
	const snes_snd_entry *s;
	if (!h) return;
	s = snes_res_snd(&s_pk, h);
	if (!s) return;
	snes_audio_play(&s_mix, (const int16_t *)(s_pk.base + s->pcm), s->frames,
			s->rate, s->loop_start, s->loop_end, loop,
			loop ? 200 : 256 /* gain 0..256 */, loop /* is_bgm */);
}

static void pump_audio(void)
{
	uint32_t h;
	int need;
	int mute = ayaneo_get_mute_menu();	/* mute the menu music + SFX (persisted) */
	while ((h = snes_menu_next_sound(&s_menu)) != 0)
		if (!mute) play_sound(h, 0);	/* drop queued SFX while muted */
	need = ayaneo_menu_audio_room();
	if (need > 16384) need = 16384;
	if (need > 0) {
		/* still mix (advances the BGM loop cursors) then zero the output when
		 * muted, so the ring is fed silence - no underrun, no looped tail. */
		snes_audio_mix(&s_mix, s_mixbuf, (unsigned)need);
		if (mute)
			memset(s_mixbuf, 0, (unsigned)need * 2u * sizeof(s_mixbuf[0]));
		ayaneo_menu_audio_submit(s_mixbuf, (unsigned)need);
	}
}

/* Reverse punch-hole: render the current menu to the SNAP reveal buffer, then shrink
 * the frozen game frame (GBA_GAME_FREEZE_PA) into a hole over `ms`, revealing the menu.
 * Shared by the close-re-entry path and the `oem rev` isolation test. Captures its own
 * frames if a motion capture is armed (g_cap_want) so the transition is inspectable. */
volatile int g_dbg_reverse_ran;
static void play_reverse_punch(unsigned int ms)
{
	unsigned int cp, cw, ch, pstart, ticks = ms * 13000u;
	unsigned int *cfb = ayaneo_canvas_back(&cp, &cw, &ch);
	snes_target lt;
	lt.fb = cfb; lt.pitch = cp; lt.W = (int)cw; lt.H = (int)ch;
	lt.offx = ((int)cw - SNES_VW) / 2; lt.offy = ((int)ch - SNES_VH) / 2;
	snes_target_view(&lt, 1.0f, 1.0f, 0.0f, 0.0f);
	if (lt.offy > 0) {
		ayaneo_fill(cfb, cp, 0, 0, (int)cw, lt.offy, 0xFF000000u);
		ayaneo_fill(cfb, cp, 0, lt.offy + SNES_VH, (int)cw, lt.offy, 0xFF000000u);
	}
	snes_menu_render(&s_menu, &lt);
	memcpy((void *)(uintptr_t)GBA_REVERSE_SNAP_PA, cfb, (size_t)cp * ch * 4);
	(void)pstart; (void)ticks;
	/* Pre-convert the FROZEN game to a full-screen BGRA buffer ONCE, so each shrink
	 * frame is just memcpy (gba_punch_composite_pre) = ~5ms not ~50ms. FRAME-paced
	 * over a fixed count so the shrink is a smooth, visible 60fps sequence. */
	if (g_reverse_is_gbc == 3) {
		/* Genesis: render the frozen frame with the SAME aspect-mode geometry as the live display
		 * (ayaneo_genesis_punch_prerender honours g_genesis_aspect + g_genesis_aspect_x1000), so the
		 * reverse shrinks the exact last on-screen frame. */
		extern void ayaneo_genesis_punch_prerender(const unsigned short *, unsigned, unsigned, unsigned);
		int gw = g_reverse_gen_w, gh = g_reverse_gen_h;
		ayaneo_genesis_punch_prerender((const unsigned short *)GBA_GAME_FREEZE_PA,
					       (unsigned)gw, (unsigned)gh, (unsigned)gw);
	} else if (g_reverse_is_gbc == 2) {
		/* SNES: render the frozen frame with the SAME aspect-aware geometry as the live display
		 * (ayaneo_snes_punch_prerender reads g_snes_aspect_x1000), so the reverse shrinks the exact
		 * last on-screen frame instead of a centred integer-scaled box. */
		int sw = g_reverse_snes_w, sh = g_reverse_snes_h;
		ayaneo_snes_punch_prerender((const unsigned short *)GBA_GAME_FREEZE_PA,
					    (unsigned)sw, (unsigned)sh, (unsigned)sw);
	} else if (g_reverse_is_gbc == 1)
		gba_punch_prerender((uint32_t *)(uintptr_t)GBA_GAME_FULL_PA, (int)cp, (int)cw, (int)ch,
				    (const unsigned short *)GBA_GAME_FREEZE_PA, 6, 160, 144,
				    ((int)cw - 960) / 2, ((int)ch - 864) / 2);
	else
		gba_punch_prerender((uint32_t *)(uintptr_t)GBA_GAME_FULL_PA, (int)cp, (int)cw, (int)ch,
				    (const unsigned short *)GBA_GAME_FREEZE_PA, 5, 240, 160,
				    ((int)cw - 1200) / 2, ((int)ch - 800) / 2);
	{
		int i, N = 20;
		for (i = 1; i <= N; i++) {
			unsigned int p2, w2, h2;
			unsigned int *db = ayaneo_canvas_back(&p2, &w2, &h2);
			int r = 820 * (N - i) / N;                   /* MAX -> 0: game shrinks */
			gba_punch_composite_pre(db, (const uint32_t *)GBA_REVERSE_SNAP_PA,
						(const uint32_t *)GBA_GAME_FULL_PA, (int)p2,
						(int)w2, (int)h2, -1, -1, r);
			ayaneo_canvas_present();
			mtk_wdt_restart();
		}
	}
}

/*
 * Run the SNES-style ROM selector. Returns the chosen ROM index, or -2 if the
 * SNES pack is missing (caller falls back to the plain list).
 */
int gba_snes_menu_run(const gba_rom_entry *roms, int nrom, int start_sel)
{
	const snes_img_entry *cart;
	unsigned int saved_mhz;
	int pwr_armed = 0;
	int do_reverse = g_reverse_punch;	/* consume now so a failed re-entry (pack missing)
						 * cannot leave a stale reverse armed for next time */
	g_reverse_punch = 0;
	/* Calibrate the analog joypad once here at the carousel (sticks centred / triggers released,
	 * before any game): powers the rail, inits the SGM58031, and samples the rest baseline. */
	{ extern void ayaneo_joypad_calibrate(int force); ayaneo_joypad_calibrate(0); }
	int fade_in = 18;   /* fade the menu in from WHITE on entry over 0.3s (the boot-logo
			     * animation ends on a whiteout, so a white->menu fade is a seamless
			     * handover; was black). matching
			     * the SNES sys_fade IN_DURATION (reveal); the BIOS intro left the
			     * panel black. The first frame is fully black, which also hides the
			     * one-time wallpaper/chrome cache build hitch. */
	if (do_reverse) fade_in = 0;   /* the reverse punch IS the transition - no white wash */

	if (nrom <= 0) return -1;

	/* Own the panel and paint it BEFORE the slow pack decompress + one-time wallpaper/
	 * chrome cache build, so the boot-logo -> menu handover is not a couple of black
	 * frames. Normal entry: whiteout (the menu then fades in from white = seamless).
	 * Reverse entry (returning from a closed game): leave the frozen game frame on the
	 * panel - the game already owns the canvas - so the reverse punch reveals the menu
	 * with no black/white flash in between. */
	saved_mhz = ayaneo_get_cpu_mhz();
	/* Boost the software renderer + pack decompress above the 600 MHz emulation clock,
	 * but cap at 1200 MHz. ayaneo_set_cpu_mhz only moves the PLL, not the core voltage
	 * (LK has no DVFS), so 2000 MHz ran at the fixed boot voltage and was unstable at
	 * idle; 1200 MHz (the Responsive gameplay tier) is a stable sustained point. The
	 * per-frame guard below re-asserts it if a game-close restore drops it. */
	ayaneo_set_cpu_mhz(1200);
	ayaneo_present_skip_framedone = 0;
	if (!do_reverse) {
		unsigned int wp, ww, wh;
		unsigned int *wfb;
		/* Whiteout prepare: paints BOTH buffers white, including the one being
		 * scanned right now, so the BIOS-intro -> menu handover has no black flash
		 * (plain ayaneo_display_prepare blacks the live buffer = the reported black
		 * frame). Then present a white back buffer too and hold it through the slow
		 * pack load; the menu fades in from white = seamless. */
		ayaneo_display_prepare_white();
		wfb = ayaneo_canvas_back(&wp, &ww, &wh);
		ayaneo_fill(wfb, wp, 0, 0, (int)ww, (int)wh, 0xFFFFFFFFu);
		ayaneo_canvas_present();
	}

	if (load_pack() != 0) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }

	if (snes_menu_init(&s_menu, &s_pk, (snes_rnode *)SNES_HOME_PA, HOME_CAP,
			   (snes_rnode *)SNES_BG_PA, BG_CAP, (uint32_t *)SNES_WP_PA,
			   (uint32_t *)SNES_CHROME_PA) != 0)
		{ ayaneo_set_cpu_mhz(saved_mhz); return -2; }

	/* enable the per-phase render profiler (g_perf[]) so `oem diag` can show where
	 * the frame time goes (wp / chrome / carousel / filmstrip / rest). */
	{
		extern unsigned (*g_perf_tick)(void);
		g_perf_tick = gpt4_get_current_tick;
	}

	build_names(roms, nrom);
	cart = snes_res_img(&s_pk, snes_hash("gba_cart"));
	snes_menu_set_gba_roster(&s_menu, s_nameptr, nrom, cart);
	/* console-type badges (GB / GBC / GBA / SNES / Genesis / SMS / GG / SG logo bottom-right of each card) */
	snes_menu_set_console_badges(&s_menu, s_types,
				     snes_res_img(&s_pk, snes_hash("logo_gb")),
				     snes_res_img(&s_pk, snes_hash("logo_gbc")),
				     snes_res_img(&s_pk, snes_hash("logo_gba")),
				     snes_res_img(&s_pk, snes_hash("logo_snes")),
				     snes_res_img(&s_pk, snes_hash("logo_genesis")),
				     snes_res_img(&s_pk, snes_hash("logo_sms")),
				     snes_res_img(&s_pk, snes_hash("logo_gg")),
				     snes_res_img(&s_pk, snes_hash("logo_sg")));
	if (start_sel >= 0 && start_sel < nrom) s_menu.focus = start_sel;

	/* Card-tile cache: cap SNES_CTILE2_CAP slots. Without boxart every GBA card body
	 * is the identical placeholder and the engine keeps its single-shared-tile fast
	 * path (only slot 0 used); with per-ROM boxart each card is distinct, so the extra
	 * slots absorb a scroll without thrashing. */
	{
		static int s_ctile_gi[SNES_CTILE2_CAP];
		snes_menu_set_ctile(&s_menu, (uint32_t *)SNES_CTILE2_PA, s_ctile_gi, SNES_CTILE2_CAP);
	}
	snes_menu_set_fct(&s_menu, (uint32_t *)SNES_FCT_PA);
	snes_menu_set_wp43(&s_menu, (uint32_t *)SNES_WP43_PA);

	/* Load each ROM's box art from the SD card (/roms/gba/boxart/<romstem>.ART) into
	 * the boxart DRAM region and hand the menu the per-game image table. A missing or
	 * oversized tile leaves w=0 -> that card falls back to the placeholder cartridge.
	 * One-time at menu init (snes_menu is pure and cannot read the SD lazily); the raw
	 * .ART is read through the deflate staging (free after load_pack). */
	{
		static snes_img_entry s_boxart[128];
		static snes_pack s_boxart_pk;
		unsigned char *region = (unsigned char *)SNES_BOXART_PA;
		unsigned char *scratch = (unsigned char *)SNES_COMP_PA;
		fat_vol *vol = gba_sd_menu_vol();
		int i, any = 0, tot = 0;
		s_boxart_pk.base = region;
		for (i = 0; i < nrom && i < 128; i++) {
			s_boxart[i].w = 0;			/* default: no art */
			if (vol && i < SNES_BOXART_CAP) {
				unsigned off = (unsigned)i * SNES_BOXART_SLOT;
				tot++;
				if (gba_boxart_load_sd(vol, roms[i].name, scratch, SNES_COMP_MAX,
						       region + off, SNES_BOXART_SLOT, off,
						       &s_boxart[i]) == 0)
					any++;
			}
		}
		g_dbg_boxart_ok = (unsigned)any;	/* reported via oem diag bx=ok/tot */
		g_dbg_boxart_tot = (unsigned)tot;
		if (any)
			snes_menu_set_gba_boxart(&s_menu, s_boxart, &s_boxart_pk);
	}

	/* Repurpose the top two cosmetic Options toggles as functional, persisted
	 * audio-mute settings: relabel them (fall back to a shorter label if the
	 * authored pool slot is too small) and seed the toggle state from the saved
	 * values. setting2 stays the untouched third cosmetic toggle. */
	if (snes_menu_relabel_option(&s_menu, 0, "Mute BIOS Audio") != 0)
		snes_menu_relabel_option(&s_menu, 0, "Mute BIOS");
	if (snes_menu_relabel_option(&s_menu, 1, "Mute Menu Audio") != 0)
		snes_menu_relabel_option(&s_menu, 1, "Mute Menu");
	s_menu.opt_on = (s_menu.opt_on & ~3u)
		| (ayaneo_get_mute_bios() ? 1u : 0u)
		| (ayaneo_get_mute_menu() ? 2u : 0u);
	snes_menu_apply_options(&s_menu);	/* show the saved toggle state on open */
	/* Resume-panel overlay cache: reuse the deflate staging (free after load_pack).
	 * fb-size = pitch*960*4 <= 1536*960*4 = 5.9MB < the 8MB comp region. */
	snes_menu_set_rcache(&s_menu, (uint32_t *)SNES_COMP_PA);

	/* (panel already owned + painted above, before the slow build; clock at 2100 too) */
	snes_audio_init(&s_mix);
	ayaneo_gbc_audio_init();
	if (s_menu.bgm) play_sound(s_menu.bgm, 1);

	/* The Pocket Air Mini panel is physically 1280x960 (4:3), so run the SNES
	 * menu's 4:3 layout (fills the panel; menubar pinned to the top, SUPER
	 * NINTENDO bar to the bottom) rather than letterboxing the 720 design. Probe
	 * the canvas height once and set the initial aspect so the very first frame is
	 * already 4:3 (no 16:9 flash + chrome rebuild). The loop keeps this in sync. */
	{
		unsigned int pitch0, W0, H0;
		(void)ayaneo_canvas_back(&pitch0, &W0, &H0);
		s_menu.aspect = ((int)H0 >= 960) ? 1 : 0;
		s_menu.chrome_ready = 0;
	}

	/* Build the wallpaper + 4:3 warped caches NOW (bilinear, ~2.6M px one-time) so the
	 * first on-screen scroll is a pure memcpy and never a dropped frame. Without this
	 * the lazy build lands on the first movement as a ~24ms hitch (one-frame flicker). */
	snes_menu_prewarm(&s_menu);
	/* Also pre-render the per-ROM card tiles so a box-art scroll is all cache hits
	 * (a cold miss mid-scroll renders a full card + box-art blit and drops the
	 * frame). No-op without box art. */
	snes_menu_prewarm_cards(&s_menu);

	/* Reverse punch-hole (returning from a closed game): render one menu frame as the
	 * reveal, then shrink the frozen last game frame into a hole so the menu appears
	 * around it - the mirror of the launch transition. Time-paced to GBA_REVERSE_MS. */
	if (do_reverse) { g_dbg_reverse_ran++; play_reverse_punch(GBA_REVERSE_MS); }

	{
	unsigned int prev_frame = 0;
	/* Swallow A/Start that are still HELD when the menu opens - e.g. carried over
	 * from selecting "Close" in the in-game menu, or from the A that launched the
	 * game - so re-entering the menu does not instantly re-launch the focused ROM.
	 * The menu edge-detects a launch (A pressed this frame, not last), and a held
	 * button on the first frame reads as a fresh edge. Gate until both are released
	 * once. (Tighter input debounce made the close land a frame sooner, so the
	 * button is more often still down on entry - this fixes it for any debounce.) */
	int launch_gate = 1;
	for (;;) {
		unsigned int pitch, W, H;
		unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
		snes_target t;
		snes_input in;
		int launch;
		float dt;
		/* Keep the menu at a FIXED, stable OPP. The software renderer is clock-bound,
		 * so run it above the 600 MHz emulation clock, but cap at 1200 MHz: 2000 MHz
		 * runs at the fixed boot voltage (LK has no DVFS) and was unstable, and toggling
		 * the clock per-frame was worse. 1200 MHz (the Responsive gameplay tier) is a
		 * stable sustained point. Re-assert only when it has actually dropped (e.g. a
		 * game-close restored the 600 MHz clock), so no per-frame PLL relock. */
		if (ayaneo_get_cpu_mhz() < 1100) ayaneo_set_cpu_mhz(1200);
		/* real elapsed dt (not a fixed 1/60) so animations run at the correct
		 * wall-clock speed even when the frame rate dips below 60. Clamp to a sane
		 * range so a first frame / long stall does not jump the animations. */
		{
			unsigned int nowf = gpt4_get_current_tick();
			dt = prev_frame ? (float)(nowf - prev_frame) / 13000000.0f : 1.0f / 60.0f;
			prev_frame = nowf;
			if (dt < 0.004f) dt = 0.004f; else if (dt > 0.1f) dt = 0.1f;
		}

		/* Maximum debounce for the carousel (same 4-frame agreement as the Pico
		 * menu): reject contact bounce / line glitches so no stray nav or launch
		 * fires. Build a raw mask, debounce, then unpack. */
		{
			extern unsigned ayaneo_menu_debounce(unsigned raw, unsigned *hist);
			enum { DL=1, DR=2, DU=4, DD=8, DA=16, DB=32, DST=64, DSE=128, DLB=256, DRB=512 };
			static unsigned mhist[3];	/* >= MENU_DEBOUNCE-1 words */
			unsigned raw = 0, deb;
			{ extern void ayaneo_joypad_poll(void); ayaneo_joypad_poll(); }   /* refresh the stick cache once/frame */
			if (PRESSED(K_LEFT))  raw |= DL;
			if (PRESSED(K_RIGHT)) raw |= DR;
			if (PRESSED(K_UP))    raw |= DU;
			if (PRESSED(K_DOWN))  raw |= DD;
			if (PRESSED(K_A))     raw |= DA;
			if (PRESSED(K_B))     raw |= DB;
			if (PRESSED(K_START)) raw |= DST;
			if (PRESSED(K_SELECT))raw |= DSE;
			if (PRESSED(K_LB))    raw |= DLB;
			if (PRESSED(K_RB))    raw |= DRB;
			/* Left analog stick also scrolls the carousel. Map the joypad D-pad bits (JOY_UP=1,
			 * DOWN=2, LEFT=4, RIGHT=8) onto this menu's layout (DL=1,DR=2,DU=4,DD=8); deadzoned, so a
			 * centred stick adds nothing and the carousel behaves exactly as before at rest. */
			{ extern unsigned int ayaneo_joypad_dpad(void); unsigned int jd = ayaneo_joypad_dpad();
			  if (jd & 0x04u) raw |= DL; if (jd & 0x08u) raw |= DR;
			  if (jd & 0x01u) raw |= DU; if (jd & 0x02u) raw |= DD; }
			deb = ayaneo_menu_debounce(raw, mhist);
		in.left = !!(deb & DL); in.right = !!(deb & DR);
		in.up = !!(deb & DU); in.down = !!(deb & DD);
		in.a = !!(deb & DA); in.b = !!(deb & DB);
		in.lb = !!(deb & DLB); in.rb = !!(deb & DRB);
		/* Start+Select held together toggles the perf HUD (below) and is consumed
		 * so it does not also launch/sort; pressed individually they behave normally.
		 * Once the combo engages, keep swallowing whichever button lingers until BOTH
		 * release - otherwise lifting Select while still holding Start would fire a
		 * launch (the menu edge-detects start with pstart=0 from the consumed frame). */
		{
			int cs = !!(deb & DST), csel = !!(deb & DSE);
			if (cs && csel) {
				if (!s_hud_combo) { s_show_hud = !s_show_hud; }
				s_hud_combo = 1;
				in.start = 0; in.select = 0;
			} else if (s_hud_combo && (cs || csel)) {
				in.start = 0; in.select = 0;   /* combo still releasing */
			} else {
				s_hud_combo = 0;
				in.start = cs; in.select = csel;
			}
		}
		}	/* end debounce block */

		/* entry gate: while a launch button (A / Start) is still held from the close
		 * or launch, force it off so no launch edge fires; clear once both release. */
		if (launch_gate) {
			if (in.a || in.start) { in.a = 0; in.start = 0; }
			else launch_gate = 0;
		}

		/* fastboot-injected key: OR one button on for g_dbg_key_hold frames. */
		if (g_dbg_key_hold > 0) {
			int *b[10] = { &in.left, &in.right, &in.up, &in.down, &in.a,
				       &in.b, &in.select, &in.start, &in.lb, &in.rb };
			if (g_dbg_key >= 0 && g_dbg_key < 10) *b[g_dbg_key] = 1;
			g_dbg_key_hold--;
		}

		menu_av_poll();   /* volume / brightness keys (+ deferred persist) */

		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2; t.offy = ((int)H - SNES_VH) / 2;
		snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
		{
			int want = ((int)H >= 960) ? 1 : 0;
			if (want != s_menu.aspect) { s_menu.aspect = want; s_menu.chrome_ready = 0; }
		}
		/* clear letterbox bars (wallpaper covers the 720 region) */
		if (t.offy > 0) {
			ayaneo_fill(fb, pitch, 0, 0, (int)W, t.offy, 0xFF000000u);
			ayaneo_fill(fb, pitch, 0, t.offy + SNES_VH, (int)W, t.offy, 0xFF000000u);
		}

		snes_menu_update(&s_menu, &in, dt);

		/* Settings > "Boot to OS": drop the sticky marker and hard-reset. Falling
		 * through to boot_linux in-place hangs (the emulator owns the clocks, DMA,
		 * caches and watchdog), so we reboot clean; from then on the device boots
		 * Android until the user holds SELECT at boot to return to the emulator. */
		if (snes_menu_take_sysreset(&s_menu)) {
			extern void ayaneo_boot_to_os(void);
			ayaneo_boot_to_os();   /* never returns */
		}

		/* Persist + apply the two audio-mute toggles when the user flips them in
		 * the Options screen (setting0 = BIOS chime, setting1 = menu audio). Only
		 * writes on an actual change; the initial state was seeded from the saved
		 * values so this does not fire on entry. */
		{
			static int mute_last = -1;
			int mb = (int)(s_menu.opt_on & 3u);
			if (mb != mute_last) {
				if (mute_last >= 0) {
					ayaneo_set_mute_bios(mb & 1);
					ayaneo_set_mute_menu((mb >> 1) & 1);
					ayaneo_menu_settings_persist();	/* eMMC + SD (SD-mode boot reads SD) */
				}
				mute_last = mb;
			}
		}
		/* TEMP diagnostic top-left: "R<render> P<peak> <fps>f". R = current
		 * snes_menu_render ms; P = the PEAK render ms over the last ~2s (the worst
		 * frame is what breaks vsync and causes the flicker, not the average). Both
		 * must stay < 16.7ms for a solid 60fps. */
		{
			static unsigned int last_t, peak_us, peak_hold;
			unsigned int r0 = gpt4_get_current_tick();
			unsigned int rus, lus, fps, now;
			snes_menu_render(&s_menu, &t);
			now = gpt4_get_current_tick();
			rus = (now - r0) / 13u;
			lus = last_t ? (r0 - last_t) / 13u : 0u;
			fps = lus ? (1000000u + lus / 2u) / lus : 0u;
			last_t = r0;
			/* precise panel refresh: average the vsync-locked loop period (in 13 MHz
			 * TICKS, not rounded us) over an 8-frame window -> Hz*1000 = 8*13e9/ticks.
			 * present() blocks on the panel vsync so the period IS the panel period; a
			 * short window updates ~every 0.13 s (fine for polling) and ticks + the
			 * 64-bit divide give sub-milli-Hz precision. Reject doubled frames (>23 ms). */
			{
				static unsigned int acc_ticks, hz_last; static int cnt;
				unsigned int lt = hz_last ? (r0 - hz_last) : 0u;
				hz_last = r0;
				if (lt > 104000u && lt < 300000u) {
					acc_ticks += lt;
					if (++cnt >= 8) {
						g_dbg_hz1000 = (unsigned int)(104000000000LL / (long long)acc_ticks);
						acc_ticks = 0; cnt = 0;
					}
				}
			}
			if (g_dbg_peak_reset) { g_dbg_peak_reset = 0; peak_us = 0; peak_hold = 0; }
			if (rus > peak_us) { peak_us = rus; peak_hold = 120; }
			else if (peak_hold) peak_hold--; else peak_us = rus;   /* decay after ~2s */
			g_dbg_render_us = rus; g_dbg_peak_us = peak_us; g_dbg_fps = fps;
			(void)s_show_hud;
		}
		if (fade_in > 0) {   /* fade in from WHITE (255 -> 0) over 18 frames = 0.3s */
			ayaneo_fill_blend(fb, pitch, 0, 0, (int)W, (int)H, 0xFFFFFFFFu,
					  (fade_in * 255) / 18);
			fade_in--;
		}
		menu_av_draw(fb, pitch, (int)W, (int)H);   /* volume/brightness OSD bar */
		pump_audio();

		/* debug: fastboot `oem nav:!` force-launches the focused ROM (injected edge
		 * presses do not fire the menu launch), driving the launch->close round-trip
		 * over USB for testing the punch + reverse transitions. */
		if (g_dbg_force_launch) {
			g_dbg_force_launch = 0;
			if (s_menu.gba_mode && s_menu.ngames > 0) {
				int n = s_menu.ngames, f = ((s_menu.focus % n) + n) % n;
				s_menu.launch = s_menu.order[f];
			}
		}
		/* fastboot `oem snes-launch`: launch the FIRST SNES ROM (frame-limited via
		 * g_snes_test_limit), so the SNES path can be validated over USB without
		 * navigating to it or risking a wrong (non-SNES) force-launch. */
		if (g_dbg_snes_launch) {
			int i;
			g_dbg_snes_launch = 0;
			for (i = 0; i < nrom && i < 128; i++)
				if (s_types[i] == GBA_CONSOLE_SNES) { s_menu.launch = i; break; }
		}
		launch = snes_menu_take_launch(&s_menu);
		if (launch >= 0 && launch < nrom) {
			/* Punch-hole launch: instead of fading the menu to black, CAPTURE the
			 * final menu frame so the driver can composite live gameplay inside a
			 * growing circle over it (ayaneo_gba_punch_frame). Render the menu once
			 * more to the back buffer, copy it to the snapshot region (free above the
			 * gpSP arena during the game), and present it so the frozen menu is on
			 * screen through the ROM load until the punch-hole starts eating it. */
			unsigned int cp, cw, ch;
			unsigned int *cfb = ayaneo_canvas_back(&cp, &cw, &ch);
			snes_target lt;
			lt.fb = cfb; lt.pitch = cp; lt.W = (int)cw; lt.H = (int)ch;
			lt.offx = ((int)cw - SNES_VW) / 2; lt.offy = ((int)ch - SNES_VH) / 2;
			snes_target_view(&lt, 1.0f, 1.0f, 0.0f, 0.0f);
			if (lt.offy > 0) {
				ayaneo_fill(cfb, cp, 0, 0, (int)cw, lt.offy, 0xFF000000u);
				ayaneo_fill(cfb, cp, 0, lt.offy + SNES_VH, (int)cw, lt.offy, 0xFF000000u);
			}
			snes_menu_render(&s_menu, &lt);
			memcpy((void *)(uintptr_t)GBA_PUNCH_SNAP_PA, cfb, (size_t)cp * ch * 4);
			gba_punch_ready = 1;
			ayaneo_canvas_present();   /* show the frozen menu during the ROM load */
			/* Stop the BGM and zero the WHOLE audio ring before handing off: no one
			 * feeds the AFE ring while the game ROM loads/decompresses, and the DMA
			 * loops the entire 341ms ring, so a submit-at-write-cursor silence tail
			 * is not enough - it would still wrap and replay older BGM frames. Clear
			 * the mixer voices and wipe the whole ring so the DMA loops silence. */
			snes_audio_init(&s_mix);
			ayaneo_menu_audio_silence();
			ayaneo_set_cpu_mhz(saved_mhz);   /* restore the emulation clock */
			return launch;
		}

		/* Present EVERY frame at a STEADY cadence (blocks on FRAME_DONE = vsync).
		 * This is exactly what the b369be3 SNES menu did when it was rock-solid with
		 * NO flicker. The later "present gate" that SKIPPED byte-identical frames was
		 * the regression: skipping a present shifts the phase of the next present
		 * relative to the scan, so the immediate (CMDQ-disabled) OVL re-latch lands at
		 * a DIFFERENT scanline each time = the rolling band the user sees. A constant
		 * every-frame cadence latches at the same phase every frame, so there is no
		 * roll. Do NOT reintroduce a frame-skipping present gate. */
		ayaneo_canvas_present();
		g_dbg_present_cnt++;
		mtk_wdt_restart();
		{
			int p = pmic_detect_powerkey();
			if (!p) pwr_armed = 1;
			else if (pwr_armed) {
				/* Flush a pending volume/brightness change BEFORE powering off, so a
				 * change made within the ~0.7s deferred-flush window is not lost. The
				 * settings write is synchronous, so it completes before mt_power_off. */
				if (s_av_dirty) { s_av_dirty = 0; ayaneo_menu_settings_persist(); }
				mt_power_off();
			}
		}
	}
	}
}
