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
#include <platform/ayaneo_volume_input.h>
#include "menu/snes_render.h"
#include "menu/snes_menu.h"
#include "menu/snes_audio.h"
#include "menu/gba_name.h"          /* gba_clean_name (shared with host_render) */
#include "sd_fat.h"                 /* gba_rom_entry */
#include "gba_boxart_sd.h"          /* gba_boxart_load_sd */
#include "pico_artimport.h"         /* pico_artimport_run */
#include "pico_i18n.h"              /* the menus' languages */
#include <arch/arm/mmu.h>           /* arch_mmu_map: the box art slots (SNES_BOXART_PA) */
#include <kernel/thread.h>          /* thread_sleep */
#include <kernel/mutex.h>           /* the box art loader is shared with the listing thread */
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
extern int ayaneo_punch_steps(void);   /* the transition's frames (mt_disp_drv.c) */

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

static void reverse_show_now(void);

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
		reverse_show_now();
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
		reverse_show_now();
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
		reverse_show_now();
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
		reverse_show_now();
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
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern unsigned int ayaneo_get_cpu_mhz(void);
extern void ayaneo_display_prepare(void);
extern void ayaneo_display_prepare_white(void);   /* whiteout handover (no black flash) */
extern void ayaneo_display_prepare(void);         /* the same, black (BIOS intro on black) */
extern int  ayaneo_get_intro_black(void);
extern void ayaneo_gbc_audio_init(void);
extern int  ayaneo_menu_audio_room(void);
extern void ayaneo_menu_audio_submit(const short *stereo, unsigned frames);
extern void ayaneo_menu_audio_silence(void);
extern int  ayaneo_get_mute_menu(void);
extern int  ayaneo_get_mute_bios(void);
extern void ayaneo_set_mute_menu(int v);
extern void ayaneo_set_mute_bios(int v);
extern void ayaneo_settings_save(void);
extern void ayaneo_menu_settings_persist(void);    /* persist to the card, off this thread */
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
#if defined(KONKR_KPA)
/* Konkr: the device tree labels match the shell (see gba_driver.c) */
#define K_A 82
#define K_B 83
#define K_START 90
#define K_SELECT 91
#else
#define K_A 83
#define K_B 82
#define K_START 91
#define K_SELECT 90
#endif
#define K_LB 92    /* GBA L shoulder = page jump back */
#define K_RB 81    /* GBA R shoulder = page jump forward */
#define K_X  84    /* the cap the user reads as X in the carousel = cycle the system filter */
#define PRESSED(g) (mt_get_gpio_in(GP(g)) == 0)

extern int pmic_detect_powerkey(void);
extern void mt_power_off(void);

/* Volume / brightness control from the menu (same as the in-game poll): the two
 * hardware volume keys change volume; SELECT + a volume key changes brightness. A
 * small OSD bar is drawn over the menu, and the persist is deferred so the card
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
volatile int          g_menu_live;         /* 1 while the ROM-select menu loop is running (oem mode) */
volatile int          g_dbg_focus;         /* current focused game index */
volatile unsigned int g_dbg_boxart_ok;     /* per-ROM boxart tiles that decoded from SD */
volatile unsigned int g_dbg_boxart_tot;    /* ROMs the boxart preload attempted */

/* Debug input injection (fastboot `oem key:<name>`): drive the live menu over USB.
 * One button at a time, held a few frames so the menu edge-detects a clean press.
 * 0..9 = L R U D A B Select Start LB RB; -1 = none. */
volatile int          g_dbg_force_launch;   /* fastboot `oem launch`: force-launch focused ROM */
volatile int          g_dbg_snes_launch;    /* fastboot `oem snes-launch`: launch the first SNES ROM */
char                  g_dbg_snes_name[32];   /* ... whose name contains this (case insensitive), if set */
const char           *g_dbg_snes_chosen;      /* the ROM name that matched (for the oem reply) */
static int dbg_name_match(const char *name, const char *sub)
{
	int i, j;
	if (!sub || !sub[0]) return 1;
	if (!name) return 0;
	for (i = 0; name[i]; i++) {
		for (j = 0; sub[j]; j++) {
			char a = name[i + j], b = sub[j];
			if (a >= 'A' && a <= 'Z') a += 32;
			if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b) break;
		}
		if (!sub[j]) return 1;
	}
	return 0;
}

/* fastboot `oem key:<name>`: inject one button into the live menu for a few frames so
 * the menu edge-detects a clean press. g_dbg_key = code 0..9 (L R U D A B Sel Start LB
 * RB), g_dbg_key_hold = frames left to hold. g_dbg_peak_reset zeroes the peak tracker so
 * the next movement's peak render us is measured clean (set at the start of an inject). */
volatile int          g_dbg_key = -1;
volatile int          g_dbg_key_hold;
volatile unsigned int g_dbg_menu_frames, g_dbg_menu_long;   /* loop periods seen / over 20 ms since the last peak reset */
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
	int sel = PRESSED(K_SELECT);
	int dir = ayaneo_volume_dir();

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

	/* flush the persist ~0.7s after the last change (off the key-repeat hot path) */
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

/* ---- the menu pack, on the SD card ----
 * /gammaos/menu.pack: ["SNSZ"][u32 rawlen][u32 complen][raw-deflate], built by
 * tools/ayaneo/gba/build_menu_pack.py. */
#define SNES_PACK_PATH "/gammaos/menu.pack"
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
 *   (boxart: was 0x55500000 .. 0x557E4000, cap 20; now at SNES_BOXART_PA below)
 *   freeze  0x55800000                (fixed, tiny)
 *   full    0x55900000 .. 0x55DB0000  (fixed)
 *   fct     0x55E00000 .. 0x55E70800  (after game-full)
 * Boxart tiles are on the SD card, never in lk_a.
 */
#if defined(KONKR_KPA)
/* Konkr: the virtual 1280x853 menu canvas + rotated-frame scratch take 0x54800000..0x54C2A400
 * (mt_disp_drv.c), so the card tile cache moves up and shrinks to stay below the reveal buffer:
 * 8 * 320*360*4 = 3.5MB, 0x54C40000..0x54FC4000. */
#define SNES_CTILE2_PA   0x54C40000u
#define SNES_CTILE2_CAP  8
#else
#define SNES_CTILE2_PA   0x54A80000u /* enlarged card-tile cache (12 * 320*360*4 = 5.4MB) */
#define SNES_CTILE2_CAP  12
#endif
/* Per-ROM box art, one slot for every game the list holds (128). The gap between the transition
 * buffers above held only 20, and the games past the 20th drew the plain cartridge. The slots sit
 * right above the emulator scratch, in DRAM the preloader leaves free (0x56000000..0x7D000000, oem
 * meminfo), outside the window the LK maps at boot: mapped write-back on the menu's first entry.
 * The rewind ring maps the free DRAM above REWIND_SCRATCH_END (ayaneo_rewind.c), which starts past
 * these 20 MB: at 0x60000000 the slots were inside its arena, which reaches them after about 24
 * minutes of one game and wrote over the tiles. No core touches it, so the tiles survive a game and
 * are not read again on the way back. */
#define SNES_BOXART_PA   0x56000000u
#define SNES_BOXART_SLOT 0x25000u    /* bytes per tile slot (>= 224*224*3) */
#define SNES_BOXART_CAP  128         /* 128 * 0x25000 = 0x1280000 */
#define SNES_BOXART_MAP  0x01400000u /* the mapping: 20 MB, whole 2 MB sections */
/* a box art file is read here before it is spread into its slot: the rest of the mapping past the
 * slots, which nothing else uses, so the art can load while the BIOS intro runs a core */
#define SNES_BOXART_STAGE     (SNES_BOXART_PA + SNES_BOXART_CAP * SNES_BOXART_SLOT)
#define SNES_BOXART_STAGE_MAX (SNES_BOXART_MAP - SNES_BOXART_CAP * SNES_BOXART_SLOT)
#if defined(KONKR_KPA)
/* Konkr: the active (blue) card tiles, one per console type (8 x 320*360*4 = 0x384000), sit
 * above the 3:2 warped wallpaper cache (1800 x 640 x 4 at the 0.75 native scale ends at
 * 0x54465000) and below the menu canvas at 0x54800000: 0x54470000..0x547F4000. A held scroll
 * across mixed console types then never re-renders a focused tile (2.7 ms each). */
#define SNES_FCT_PA      0x54470000u
#define SNES_FCT_CAP     8
#else
#define SNES_FCT_PA      0x55E00000u /* focused (blue) card-body tile (320*360*4 = 450KB) */
#define SNES_FCT_CAP     1
#endif
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

/* System filter (press X to cycle All -> each present system -> All). The menu is fed a
 * FILTERED VIEW of the roster; s_vmap maps a view index back to the full roms[] index so a
 * launch returns the right game. The view name/type/boxart tables are compact copies of the
 * full s_nameptr / s_types / s_boxart_full tables. */
static const char    *s_vnameptr[128];
static unsigned char  s_vtypes[128];
static snes_img_entry s_vboxart[128];
static unsigned short s_vmap[128];
static int            s_vn;                 /* games in the current view */
static int            s_sysfilter = -1;     /* -1 = All, else a GBA_CONSOLE_* value */
static snes_img_entry s_boxart_full[128];   /* full per-ROM boxart, indexed by roms[] */
static unsigned int   s_boxart_key[128];    /* what slot i holds: the ROM's key, 0 = not tried */
volatile unsigned int g_dbg_boxart_ms;      /* oem diag: the last box art load, ms */
static snes_pack      s_boxart_full_pk;
static int            s_have_boxart;
static const snes_img_entry *s_cart_img;    /* placeholder cart tile, for the roster re-set */
static int            s_filter_label_hold;  /* frames left to show the "System: X" toast */

static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Read + inflate the menu pack from the card into SNES_BLOB_PA, open it. 0 = ok. */
static int load_pack(void)
{
	extern fat_vol *gba_sd_menu_vol(void);   /* gba_driver.c: the mounted card */
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	unsigned char hdr[12];
	uint32_t magic, rawlen, complen;
	unsigned char *comp = (unsigned char *)SNES_COMP_PA;
	unsigned long zlen;
	if (!v || fat_open(v, SNES_PACK_PATH, &f) != 0) return -1;
	if (fat_read(&f, 0, hdr, 12) != 12) return -1;
	magic = rd32(hdr); rawlen = rd32(hdr + 4); complen = rd32(hdr + 8);
	if (magic != SNES_MAGIC || rawlen == 0 || rawlen > SNES_RAW_MAX ||
	    complen == 0 || complen > SNES_COMP_MAX || 12u + complen > f.size) return -2;
	if (fat_read(&f, 12, comp, complen) != complen) return -3;
	zlen = complen;
	if (zunzip(comp, &zlen, (void *)SNES_BLOB_PA, (int)rawlen, 0) != 0) return -4;
	if (snes_pack_open(&s_pk, (void *)SNES_BLOB_PA, rawlen) != 0) return -5;
	return 0;
}

/* Menu themes (pack_snes.py --theme): per theme a thumbnail "theme_thumb_NN_<name>", a UI atlas
 * "theme_atlas_NN_<name>" and optionally a home wallpaper "theme_wall_NN_<name>", NN its place in
 * the list. The first theme's atlas and wallpaper are the images every sprite and the wallpaper
 * draw from; the saved theme (the hash of its name, 0 = the first) is applied by pointing those
 * image entries at the theme's pixels, once per pack load and before the menu builds its caches. A
 * theme missing from the pack, or an image of another size, leaves the first theme's. */
static int s_theme_n, s_theme_cur;
static uint32_t s_theme_hash[SNES_THEME_MAX];
static const snes_img_entry *s_theme_thumb[SNES_THEME_MAX], *s_theme_atlas[SNES_THEME_MAX];
static const snes_img_entry *s_theme_wall[SNES_THEME_MAX];
static int s_theme_layout[SNES_THEME_MAX];   /* a theme with a menu of its own (pico_current.c) */
static unsigned int s_theme_beta;              /* the themes still being finished, a bit each */
static int s_keep_focus = -1;   /* the game focused when a theme was picked: kept over the reload */
/* The classic theme's Language page holds the first eight languages, in a grid ordered by row
 * (language01, language05, language02, ...); the others leave the classic theme in English, as its
 * fonts have no glyphs for their scripts. */
static const signed char k_lang_grid[8] = { LANG_EN, LANG_IT, LANG_FR, LANG_NL, LANG_DE, LANG_PT, LANG_ES, LANG_RU };
static const char *const k_lang_locale[8] = { "usa_en", "eur_fr", "eur_de", "eur_es", "eur_it", "eur_nl", "eur_pt", "eur_ru" };
static int lang_grid_of(int lang)
{
	int i;
	for (i = 0; i < 8; i++) if (k_lang_grid[i] == lang) return i;
	return -1;
}
/* relabel an Options row in the language in use when its slot and font take it, else in English */
static void relabel_row(int idx, const char *full, const char *brief)
{
	if (snes_menu_relabel_option(&s_menu, idx, tr(full)) == 0) return;
	if (snes_menu_relabel_option(&s_menu, idx, tr(brief)) == 0) return;
	if (snes_menu_relabel_option(&s_menu, idx, full) != 0) snes_menu_relabel_option(&s_menu, idx, brief);
}
static int theme_id(const char *id, const char *pfx, int *nn, const char **name)
{
	while (*pfx)
		if (*id++ != *pfx++) return 0;
	if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9' || id[2] != '_' || !id[3]) return 0;
	*nn = (id[0] - '0') * 10 + (id[1] - '0');
	*name = id + 3;
	return 1;
}
/* Whether the game being opened carries on from its suspend point (each core asks once as it loads
 * it): always when Resume on Power On brought it back, else as Resume on Game Open says. Holding B
 * at the launch still starts it fresh (the cores check that themselves). */
static int s_launch_resumes;
int gba_launch_continues(void)
{
	extern int ayaneo_get_resume_on_open(void);
	int r = s_launch_resumes || ayaneo_get_resume_on_open();
	s_launch_resumes = 0;
	return r;
}
/* The menu theme in use: the hash of its name, as saved. Nothing saved yet (0) is the Current
 * theme, the default; Classic is saved by its own name once picked. */
unsigned int gba_menu_theme(void)
{
	extern unsigned int ayaneo_get_menu_theme(void);
	unsigned int t = ayaneo_get_menu_theme();
	return t ? t : snes_hash("current");
}
static void theme_apply(void)
{
	uint32_t want = gba_menu_theme(), i;
	int k;
	for (k = 0; k < SNES_THEME_MAX; k++) {
		s_theme_hash[k] = 0; s_theme_thumb[k] = 0; s_theme_atlas[k] = 0; s_theme_wall[k] = 0; s_theme_layout[k] = 0;
	}
	s_theme_n = 0; s_theme_cur = 0; s_theme_beta = 0;
	for (i = 0; i < s_pk.hdr->res_count; i++) {
		const snes_res_entry *e = &s_pk.res[i];
		const char *name;
		int nn;
		if (!e->id_hash || e->type != RES_TEXTURE || e->index >= s_pk.hdr->img_count) continue;
		if (theme_id(snes_str(&s_pk, e->id_str), "theme_thumb_", &nn, &name) && nn < SNES_THEME_MAX) {
			s_theme_thumb[nn] = &s_pk.img[e->index];
			s_theme_hash[nn] = snes_hash(name);
		} else if (theme_id(snes_str(&s_pk, e->id_str), "theme_atlas_", &nn, &name) && nn < SNES_THEME_MAX)
			s_theme_atlas[nn] = &s_pk.img[e->index];
		else if (theme_id(snes_str(&s_pk, e->id_str), "theme_wall_", &nn, &name) && nn < SNES_THEME_MAX)
			s_theme_wall[nn] = &s_pk.img[e->index];
		else if (theme_id(snes_str(&s_pk, e->id_str), "theme_layout_", &nn, &name) && nn < SNES_THEME_MAX)
			s_theme_layout[nn] = 1;
		else if (theme_id(snes_str(&s_pk, e->id_str), "theme_beta_", &nn, &name) && nn < SNES_THEME_MAX)
			s_theme_beta |= 1u << nn;
	}
	while (s_theme_n < SNES_THEME_MAX && s_theme_thumb[s_theme_n] &&
	       (s_theme_atlas[s_theme_n] || s_theme_layout[s_theme_n])) s_theme_n++;
	if (want)
		for (k = 1; k < s_theme_n; k++)
			if (s_theme_hash[k] == want) { s_theme_cur = k; break; }
	if (s_theme_cur > 0 && s_theme_layout[s_theme_cur]) return;   /* drawn by its own menu */
	if (s_theme_cur > 0) {
		snes_img_entry *base = (snes_img_entry *)s_theme_atlas[0];   /* the pack is ours, in DRAM */
		const snes_img_entry *t = s_theme_atlas[s_theme_cur];
		if (t->w == base->w && t->h == base->h && t->flags == base->flags) base->pixels = t->pixels;
		else s_theme_cur = 0;
	}
	if (s_theme_cur > 0 && s_theme_wall[0] && s_theme_wall[s_theme_cur]) {
		snes_img_entry *base = (snes_img_entry *)s_theme_wall[0];
		const snes_img_entry *t = s_theme_wall[s_theme_cur];
		if (t->w == base->w && t->h == base->h && t->flags == base->flags) base->pixels = t->pixels;
	}
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
/* Keep the menu's layout (aspect) and native scale in step with the target it renders to; a
 * change invalidates the chrome, wallpaper and tile caches (they are built per aspect/scale). */
static void menu_sync_target(const snes_target *t)
{
	int want = snes_aspect_for(t->W, t->H);
	float base = snes_target_base(t);
	if (want != s_menu.aspect || base != (s_menu.base > 0.0f ? s_menu.base : 1.0f)) {
		s_menu.aspect = want; s_menu.base = base; s_menu.chrome_ready = 0;
	}
}
/* One frame of menu input, shared by every menu theme: the buttons and the left stick,
 * debounced, Start+Select as the perf HUD toggle, and the key injected over USB (oem nav). */
void gba_menu_read_input(snes_input *in)
{
	extern unsigned ayaneo_menu_debounce(unsigned raw, unsigned *hist);
	extern unsigned int ayaneo_joypad_dpad(void);
	extern void ayaneo_joypad_poll(void);
	enum { DL=1, DR=2, DU=4, DD=8, DA=16, DB=32, DST=64, DSE=128, DLB=256, DRB=512 };
	static unsigned mhist[3];	/* >= MENU_DEBOUNCE-1 words */
	unsigned raw = 0, deb, jd;
	int cs, csel;
	/* Maximum debounce (the same agreement as the Pico menu): reject contact bounce and line
	 * glitches so no stray move or launch fires. Build a raw mask, debounce, then unpack. */
	ayaneo_joypad_poll();   /* refresh the stick cache once a frame */
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
	/* The left stick moves too: the joypad D-pad bits (JOY_UP=1, DOWN=2, LEFT=4, RIGHT=8) onto
	 * this layout (DL=1, DR=2, DU=4, DD=8); deadzoned, so a centred stick adds nothing. */
	jd = ayaneo_joypad_dpad();
	if (jd & 0x04u) raw |= DL;
	if (jd & 0x08u) raw |= DR;
	if (jd & 0x01u) raw |= DU;
	if (jd & 0x02u) raw |= DD;
	deb = ayaneo_menu_debounce(raw, mhist);
	in->left = !!(deb & DL); in->right = !!(deb & DR);
	in->up = !!(deb & DU); in->down = !!(deb & DD);
	in->a = !!(deb & DA); in->b = !!(deb & DB);
	in->lb = !!(deb & DLB); in->rb = !!(deb & DRB);
	/* Start+Select held together toggles the perf HUD and is consumed so it does not also
	 * launch or sort; pressed alone they behave normally. Once the combo engages, keep
	 * swallowing whichever button lingers until both release, or lifting Select while still
	 * holding Start would fire a launch. */
	cs = !!(deb & DST); csel = !!(deb & DSE);
	if (cs && csel) {
		if (!s_hud_combo) s_show_hud = !s_show_hud;
		s_hud_combo = 1;
		in->start = 0; in->select = 0;
	} else if (s_hud_combo && (cs || csel)) {
		in->start = 0; in->select = 0;   /* combo still releasing */
	} else {
		s_hud_combo = 0;
		in->start = cs; in->select = csel;
	}
	/* fastboot-injected key: OR one button on for g_dbg_key_hold frames. */
	if (g_dbg_key_hold > 0) {
		int *b[10] = { &in->left, &in->right, &in->up, &in->down, &in->a,
			       &in->b, &in->select, &in->start, &in->lb, &in->rb };
		if (g_dbg_key >= 0 && g_dbg_key < 10) *b[g_dbg_key] = 1;
		g_dbg_key_hold--;
	}
}

static void reverse_punch_shrink(void);
/* The closing transition's snapshot and, when set, where its frames are composed (the canvas
 * otherwise): a theme moves them to buffers of its own (gba_menu_reverse_buffers). */
static const uint32_t *s_rev_snap = (const uint32_t *)GBA_REVERSE_SNAP_PA;
static unsigned int *s_rev_dst;
void gba_menu_reverse_buffers(uint32_t *snap, unsigned int *dst)
{
	s_rev_snap = snap ? snap : (const uint32_t *)GBA_REVERSE_SNAP_PA;
	s_rev_dst = dst;
}
/* Load each ROM's box art from the SD card (<rom folder>/boxart/<romstem>.ART) into the box
 * art slots and fill the per-game image table (s_boxart_full). A missing or oversized tile leaves
 * w=0, the game then shows the placeholder. A slot that already holds a ROM's tile (or knows it has
 * none) is not read again, so a menu after a game only loads the art of games that changed place.
 * The raw .ART is read through the deflate staging, free once the menu's pack is loaded.
 * Returns the games that have art. Shared by every menu theme. */
/* The box art is read once by the listing's thread during the BIOS intro (gba_driver.c), and the
 * menu's call then finds every slot already holding its game; one that comes before it is done
 * waits here only for the rest. */
static mutex_t s_boxart_mtx;
static volatile int s_boxart_mtx_up;
int gba_menu_boxart_load(const gba_rom_entry *roms, int nrom)
{
	static int s_boxart_mapped;
	unsigned char *region = (unsigned char *)SNES_BOXART_PA;
	unsigned char *scratch = (unsigned char *)SNES_BOXART_STAGE;
	fat_vol *vol = gba_sd_menu_vol();
	unsigned int t0 = gpt4_get_current_tick();
	int i, any = 0, tot = 0;
	enter_critical_section();
	if (!s_boxart_mtx_up) { mutex_init(&s_boxart_mtx); s_boxart_mtx_up = 1; }
	exit_critical_section();
	mutex_acquire(&s_boxart_mtx);
	{	/* PNGs put on the card since the last power on become .ART first, so their games show
		 * them now (a slot that knows its game has no art is not read again) */
		static int s_imported;
		if (!s_imported && vol) { s_imported = 1; pico_artimport_run(vol, roms, nrom); }
	}
	if (!s_boxart_mapped)
		s_boxart_mapped = arch_mmu_map(SNES_BOXART_PA, SNES_BOXART_PA,
					       MMU_MEMORY_TYPE_NORMAL_WRITE_BACK | MMU_MEMORY_AP_P_RW_U_NA,
					       SNES_BOXART_MAP) == 0;
	s_boxart_full_pk.base = region;
	for (i = 0; i < nrom && i < 128; i++) {
		/* the slot's key: the ROM's console and name. A slot that already holds this ROM's
		 * tile (or knows it has none) is not read again: a menu after a game only loads the
		 * art of games that changed place. */
		unsigned int key = 2166136261u ^ roms[i].type;
		const char *c;
		for (c = roms[i].name; *c; c++) key = (key ^ (unsigned char)*c) * 16777619u;
		key |= 1u;
		if (!vol || !s_boxart_mapped || i >= SNES_BOXART_CAP) { s_boxart_full[i].w = 0; continue; }
		tot++;
		if (s_boxart_key[i] == key) { if (s_boxart_full[i].w) any++; continue; }
		{
			unsigned off = (unsigned)i * SNES_BOXART_SLOT;
			s_boxart_full[i].w = 0;		/* default: no art */
			if (gba_boxart_load_sd(vol, &roms[i], scratch, SNES_BOXART_STAGE_MAX,
					       region + off, SNES_BOXART_SLOT, off,
					       &s_boxart_full[i]) == 0)
				any++;
			s_boxart_key[i] = key;
		}
	}
	g_dbg_boxart_ms = (gpt4_get_current_tick() - t0) / 13000u;
	g_dbg_boxart_ok = (unsigned)any;	/* reported via oem diag bx=ok/tot */
	g_dbg_boxart_tot = (unsigned)tot;
	s_have_boxart = (any > 0);
	mutex_release(&s_boxart_mtx);
	return any;
}

/* Game i's box art (gba_menu_boxart_load): the RGB565 + A8 interleaved pixels and their size,
 * or NULL without art. */
const unsigned char *gba_menu_boxart(int i, unsigned int *w, unsigned int *h)
{
	if (i < 0 || i >= 128 || !s_boxart_full[i].w) return 0;
	if (w) *w = s_boxart_full[i].w;
	if (h) *h = s_boxart_full[i].h;
	return (const unsigned char *)SNES_BOXART_PA + s_boxart_full[i].pixels;
}

/* Present the frame the menu drew into the canvas: the power-hold bar on top, then the present
 * (every frame, at a steady cadence: a skipped present shifts the phase of the next one against
 * the scan and rolls a band through the picture), then power off when the key was held long
 * enough. Shared by every menu theme. */
void gba_menu_present(void)
{
	extern void ayaneo_power_bar_draw(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H);
	extern int ayaneo_power_hold(int menu_open, int use_overlay);
	unsigned int bp, bw, bh;
	unsigned int *bb = ayaneo_canvas_back(&bp, &bw, &bh);
	ayaneo_power_bar_draw(bb, bp, bw, bh);   /* power hold: the bar goes into this frame */
	ayaneo_canvas_present();
	g_dbg_present_cnt++;
	mtk_wdt_restart();
	{	/* LK runs the highest priority thread that is ready, and the menus draw every refresh
		 * with no time to spare before it: the low priority threads (the settings writer, the
		 * card writer, the power LED) ran only once a game left the CPU idle, so a setting
		 * changed in the menu was not on the card until then. A moment each frame while one has
		 * work, and a millisecond now and then for the rest. */
		extern int ayaneo_settings_persist_pending(void);
		extern int card_writer_pending(void) __attribute__((weak));
		static unsigned int n;
		if (ayaneo_settings_persist_pending() || (card_writer_pending && card_writer_pending())) thread_sleep(2);
		else if (++n % 30u == 0) thread_sleep(1);
	}
	if (ayaneo_power_hold(0, 0)) {
		/* Powering off from the menu: the next boot lands on the menu, whatever record a game
		 * left, and a volume or brightness change still waiting for its deferred write is
		 * saved too; the write is waited for (the writer thread only runs while we sleep). */
		extern void ayaneo_resume_clear(void), ayaneo_settings_persist_flush(void), ayaneo_power_off_now(void);
		ayaneo_resume_clear();
		s_av_dirty = 0;
		ayaneo_menu_settings_persist();
		ayaneo_settings_persist_flush();
		ayaneo_power_off_now();
	}
}
/* Launch: the frame in the canvas becomes the frozen menu the game opens out of (the driver's
 * punch-hole), and stays on screen while the ROM loads. */
void gba_menu_launch_snapshot(void)
{
	unsigned int cp, cw, ch;
	unsigned int *cfb = ayaneo_canvas_back(&cp, &cw, &ch);
	memcpy((void *)(uintptr_t)GBA_PUNCH_SNAP_PA, cfb, (size_t)cp * ch * 4);
	gba_punch_ready = 1;
	ayaneo_canvas_present();
}
void gba_menu_av_poll(void) { menu_av_poll(); }
void gba_menu_av_draw(unsigned int *fb, unsigned int pitch, int W, int H) { menu_av_draw(fb, pitch, W, H); }

/* The classic menu's frame as the reveal of the reverse punch-hole. */
static void classic_reveal_frame(void)
{
	unsigned int cp, cw, ch;
	unsigned int *cfb = ayaneo_canvas_back(&cp, &cw, &ch);
	snes_target lt;
	snes_target_fit(&lt, cfb, (int)cp, (int)cw, (int)ch);
	menu_sync_target(&lt);
	if (lt.offy > 0) {
		ayaneo_fill(cfb, cp, 0, 0, (int)cw, lt.offy, 0xFF000000u);
		ayaneo_fill(cfb, cp, 0, lt.offy + snes_target_vh(&lt), (int)cw, lt.offy, 0xFF000000u);
	}
	snes_menu_render(&s_menu, &lt);
}
/* Returning from a closed game: render one menu frame into the canvas (render), keep it as the
 * reveal, then shrink the frozen last game frame into a hole so the menu appears around it, the
 * mirror of the launch transition. Every menu theme passes its own render. */
void gba_menu_reverse_punch(void (*render)(void))
{
	unsigned int cp, cw, ch;
	unsigned int *cfb;
	render();
	cfb = ayaneo_canvas_back(&cp, &cw, &ch);
	memcpy((void *)(uintptr_t)GBA_REVERSE_SNAP_PA, cfb, (size_t)cp * ch * 4);
	reverse_punch_shrink();
}
/* The same with the reveal drawn straight into the transition's snapshot (pitch as the canvas):
 * on the Konkr the frozen game on screen is shown from memory the canvas shares, so drawing the
 * reveal into the canvas showed it for a moment, turned and in halves. */
void gba_menu_reverse_into(void (*render)(uint32_t *snap, unsigned int pitch))
{
	unsigned int cp, cw, ch;
	(void)ayaneo_canvas_back(&cp, &cw, &ch);
	render((uint32_t *)s_rev_snap, s_rev_dst ? cw : cp);
	reverse_punch_shrink();
}
/* the frozen game, prerendered full screen for the closing transition (where g_punch_game_full says) */
static int s_rev_prerendered;
static void reverse_prerender(void)
{
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
	} else if (g_reverse_is_gbc == 1) {
		/* GB/GBC: the display driver bakes the frame with the geometry the live path showed
		 * (integer 6x centred on the pairmini; the logical-screen rect mapped onto the virtual
		 * canvas on the Konkr), so the reverse shrinks exactly what was on screen. */
		extern void ayaneo_gb_punch_prerender(const unsigned short *pix);
		ayaneo_gb_punch_prerender((const unsigned short *)GBA_GAME_FREEZE_PA);
	} else {
		extern void ayaneo_gba_punch_prerender(const unsigned short *pix);
		ayaneo_gba_punch_prerender((const unsigned short *)GBA_GAME_FREEZE_PA);
	}
}
/* Show the frozen game full screen as soon as a game ends, from the menu's own plane. The game's
 * last frames were shown through the resizer; once it is let go the panel shows the frame buffers
 * again, which still held the opening transition's last frame (a stale picture of the launch).
 * Both buffers get the frozen frame, so whichever one the panel lands on is right. */
static void reverse_show_now(void)
{
	extern unsigned int g_punch_game_full;
	unsigned int keep = g_punch_game_full, *prev = 0;
	int k;
	ayaneo_menu_audio_silence();   /* the game has stopped: its ring must not loop meanwhile */
	for (k = 0; k < 2; k++) {
		unsigned int cp, cw, ch;
		unsigned int *cv = ayaneo_canvas_back(&cp, &cw, &ch);
		if (cv != prev) {   /* the Konkr's canvas is one buffer, turned into either frame buffer */
			g_punch_game_full = (unsigned int)(uintptr_t)cv;
			reverse_prerender();
			prev = cv;
		}
		ayaneo_canvas_present();
	}
	g_punch_game_full = keep;
}
/* Put the frozen game on the menu's own plane before anything loads: until then the game is shown
 * from its own buffers, which the menu's pack then lands on. Prerenders it, presents it; the
 * closing transition uses that prerender. */
void gba_menu_reverse_hold(void)
{
	extern unsigned int g_punch_game_full;
	extern void ayaneo_canvas_present_from(const unsigned int *src);
	reverse_prerender();
	s_rev_prerendered = 1;
	ayaneo_canvas_present_from((const unsigned int *)(uintptr_t)g_punch_game_full);
}
static void reverse_punch_shrink(void)
{
	if (!s_rev_prerendered) reverse_prerender();
	s_rev_prerendered = 0;
	{
		int i, N = ayaneo_punch_steps();
		/* Start radius = the canvas half diagonal (820 on 1280x960, 577 on 960x640) so the
		 * circle covers the corners on the first frame whatever the panel. */
		int rmax = 0;
		{ unsigned int p0, w0, h0; (void)ayaneo_canvas_back(&p0, &w0, &h0);
		  { unsigned int d2 = (w0 * w0 + h0 * h0) / 4u, r = 1; while (r * r < d2) r++; rmax = (int)r; } }
		for (i = 1; i <= N; i++) {
			extern unsigned int g_punch_game_full;
			extern void ayaneo_canvas_present_from(const unsigned int *src);
			unsigned int p2, w2, h2;
			unsigned int *db = ayaneo_canvas_back(&p2, &w2, &h2);
			int r = rmax * (N - i) / N;                  /* MAX -> 0: game shrinks */
			if (s_rev_dst) { db = s_rev_dst; p2 = w2; }
			extern void ayaneo_punch_compose(uint32_t *dst, const uint32_t *snap, const uint32_t *game_full,
							 int pitch, int W, int H, int radius);
			ayaneo_punch_compose(db, s_rev_snap, (const uint32_t *)(uintptr_t)g_punch_game_full, (int)p2,
					     (int)w2, (int)h2, r);
			if (s_rev_dst) ayaneo_canvas_present_from(s_rev_dst); else ayaneo_canvas_present();
			mtk_wdt_restart();
		}
	}
}

/* Human label for the system-filter toast. */
static const char *filter_name(int f)
{
	switch (f) {
	case GBA_CONSOLE_GB:      return "Game Boy";
	case GBA_CONSOLE_GBC:     return "Game Boy Color";
	case GBA_CONSOLE_GBA:     return "Game Boy Advance";
	case GBA_CONSOLE_SNES:    return "Super Nintendo";
	case GBA_CONSOLE_GENESIS: return "Mega Drive / Genesis";
	case GBA_CONSOLE_SMS:     return "Master System";
	case GBA_CONSOLE_GG:      return "Game Gear";
	case GBA_CONSOLE_SG:      return "SG-1000";
	default:                  return "All Systems";
	}
}

/* Rebuild the visible roster for the current s_sysfilter (-1 = All, else a GBA_CONSOLE_*)
 * from the full name/type/boxart tables and hand the filtered view to the menu. Copies the
 * boxart img_entry struct only (its pixels stay put in the boxart DRAM region), records the
 * view->roms map, and resets focus to the top of the new list. */
static void apply_filter(int full_n)
{
	int i, k = 0;
	if (full_n > 128) full_n = 128;
	for (i = 0; i < full_n; i++) {
		if (s_sysfilter >= 0 && (int)s_types[i] != s_sysfilter) continue;
		s_vnameptr[k] = s_nameptr[i];
		s_vtypes[k]   = s_types[i];
		s_vboxart[k]  = s_boxart_full[i];
		s_vmap[k]     = (unsigned short)i;
		k++;
	}
	s_vn = k;
	s_menu.focus = 0;
	snes_menu_set_gba_roster(&s_menu, s_vnameptr, s_vn, s_cart_img);
	if (s_have_boxart) snes_menu_set_gba_boxart(&s_menu, s_vboxart, &s_boxart_full_pk);
}

/* X advances the filter to All or the next system that actually has ROMs (skips empty ones). */
static void cycle_filter(int full_n)
{
	static const signed char ord[9] = { -1, GBA_CONSOLE_GB, GBA_CONSOLE_GBC,
		GBA_CONSOLE_GBA, GBA_CONSOLE_SNES, GBA_CONSOLE_GENESIS,
		GBA_CONSOLE_SMS, GBA_CONSOLE_GG, GBA_CONSOLE_SG };
	int cur = 0, i, s;
	for (i = 0; i < 9; i++) if (ord[i] == s_sysfilter) { cur = i; break; }
	for (s = 0; s < 9; s++) {
		int f;
		cur = (cur + 1) % 9;
		f = ord[cur];
		if (f < 0) { s_sysfilter = -1; break; }        /* All is always available */
		{ int j, present = 0;
		  for (j = 0; j < full_n && j < 128; j++) if ((int)s_types[j] == f) { present = 1; break; }
		  if (present) { s_sysfilter = f; break; } }
	}
	apply_filter(full_n);
	s_filter_label_hold = 96;   /* ~1.6s at 60fps */
}

/*
 * Run the SNES-style ROM selector. Returns the chosen ROM index, or -2 if the
 * SNES pack is missing (caller falls back to the plain list).
 */
int gba_snes_menu_run(const gba_rom_entry *roms, int nrom, int start_sel)
{
	const snes_img_entry *cart;
	unsigned int saved_mhz;
#if defined(KONKR_KPA)
	{ extern void ayaneo_joypad_kpa_reset_toggles(void); ayaneo_joypad_kpa_reset_toggles(); }   /* L2/R2 toggles never carry across sessions */
#endif
	int do_reverse = g_reverse_punch;	/* consume now so a failed re-entry (pack missing)
						 * cannot leave a stale reverse armed for next time */
	g_reverse_punch = 0;
	/* Calibrate the analog joypad once here at the carousel (sticks centred / triggers released,
	 * before any game): powers the rail, inits the SGM58031, and samples the rest baseline. */
	{ extern void ayaneo_joypad_calibrate(int force); ayaneo_joypad_calibrate(0); }
	/* Resume on Power On: a ROM suspended with the power key starts straight away, before the
	 * menu paints anything, loads its pack or reads the box art (the core reloads its suspend
	 * state and runs from its first frame; no launch transition). The record is consumed here so
	 * a later power off from the menu boots to the menu. */
	if (!do_reverse) {
		extern int ayaneo_get_resume_on_boot(void);
		extern int ayaneo_resume_take(char *out, int cap);
		char rn[100];
		extern volatile int g_dbg_resume[4];
		g_dbg_resume[3] = ayaneo_get_resume_on_boot();
		if (ayaneo_get_resume_on_boot() && ayaneo_resume_take(rn, sizeof rn)) {
			int k;
			g_dbg_resume[1] = 1;
			ayaneo_menu_settings_persist();   /* the cleared record */
			for (k = 0; k < nrom; k++)
				if (strcmp(roms[k].name, rn) == 0) { g_dbg_resume[2] = k + 1; s_launch_resumes = 1; return k; }
		}
	}
	/* the Current theme is a menu of its own: it runs instead of this one, and hands back here
	 * only when its pack is missing or does not fit the screen */
	{
		extern int g_punch_fade;
		extern unsigned int g_punch_game_full;
		g_punch_fade = 0;   /* this menu opens games through the punch-hole; Current asks for its fade */
		g_punch_game_full = 0x55900000u;   /* and in the default buffers */
		{ extern int g_punch_cached; g_punch_cached = 0; }
		extern int pico_current_run(const gba_rom_entry *roms, int nrom, int start_sel, int do_reverse);
		extern unsigned int ayaneo_get_menu_theme(void);
		if (gba_menu_theme() == snes_hash("current")) {
			int r = pico_current_run(roms, nrom, start_sel, do_reverse);
			if (r != -2) return r;
		}
	}
	int fade_in = 18;   /* fade the menu in from WHITE on entry over 0.3s (the boot-logo
			     * animation ends on a whiteout, so a white->menu fade is a seamless
			     * handover; was black). matching
			     * the SNES sys_fade IN_DURATION (reveal); the BIOS intro left the
			     * panel black. The first frame is fully black, which also hides the
			     * one-time wallpaper/chrome cache build hitch. */
	if (do_reverse) fade_in = 0;   /* the reverse punch IS the transition - no white wash */
	/* "BIOS Intro Background: Black": the intro ends on black, so the handover paints black and the
	 * menu fades in from black instead */
	unsigned int handover = ayaneo_get_intro_black() ? 0xFF000000u : 0xFFFFFFFFu;

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
		if (handover == 0xFFFFFFFFu) ayaneo_display_prepare_white(); else ayaneo_display_prepare();
		wfb = ayaneo_canvas_back(&wp, &ww, &wh);
		ayaneo_fill(wfb, wp, 0, 0, (int)ww, (int)wh, handover);
		ayaneo_canvas_present();
	}

	if (load_pack() != 0) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }
	{	/* the language: the authored tables in it where the theme has them, and GammaOS's words */
		unsigned int lp, lw, lh;
		int lang = pico_lang();
		ayaneo_canvas_back(&lp, &lw, &lh);
		pico_lang_load(lw, lh);
		if (lang >= 0 && lang < 8) snes_pack_set_locale(&s_pk, k_lang_locale[lang]);
	}
	theme_apply();   /* the saved theme's atlas, before the menu builds its caches from it */

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
	s_cart_img = cart;
	/* The roster itself is set by apply_filter() below, AFTER the boxart table loads, so the
	 * initial All-systems view and every later X filter change go through one path. */
	/* console-type badges (GB / GBC / GBA / SNES / Genesis / SMS / GG / SG logo bottom-right of each card) */
	snes_menu_set_console_badges(&s_menu, s_vtypes,
				     snes_res_img(&s_pk, snes_hash("logo_gb")),
				     snes_res_img(&s_pk, snes_hash("logo_gbc")),
				     snes_res_img(&s_pk, snes_hash("logo_gba")),
				     snes_res_img(&s_pk, snes_hash("logo_snes")),
				     snes_res_img(&s_pk, snes_hash("logo_genesis")),
				     snes_res_img(&s_pk, snes_hash("logo_sms")),
				     snes_res_img(&s_pk, snes_hash("logo_gg")),
				     snes_res_img(&s_pk, snes_hash("logo_sg")));
	snes_menu_set_themes(&s_menu, s_theme_n, s_theme_thumb, s_theme_cur);   /* Display: the Theme row */
	s_menu.theme_beta = s_theme_beta;
	/* initial focus (start_sel) is applied after apply_filter() below, which resets focus */

	/* Card-tile cache: cap SNES_CTILE2_CAP slots. Without boxart every GBA card body
	 * is the identical placeholder and the engine keeps its single-shared-tile fast
	 * path (only slot 0 used); with per-ROM boxart each card is distinct, so the extra
	 * slots absorb a scroll without thrashing. */
	{
		static int s_ctile_gi[SNES_CTILE2_CAP];
		snes_menu_set_ctile(&s_menu, (uint32_t *)SNES_CTILE2_PA, s_ctile_gi, SNES_CTILE2_CAP);
	}
	snes_menu_set_fct(&s_menu, (uint32_t *)SNES_FCT_PA, SNES_FCT_CAP);
	snes_menu_set_wp43(&s_menu, (uint32_t *)SNES_WP43_PA);

	gba_menu_boxart_load(roms, nrom);

	/* Build the initial (All systems) view and hand it to the menu. Every X press re-runs
	 * apply_filter() to swap in a single-system view (see cycle_filter). */
	s_sysfilter = -1;
	apply_filter(nrom);
	if (s_keep_focus >= 0) { start_sel = s_keep_focus; s_keep_focus = -1; }   /* back from a theme change */
	if (start_sel >= 0 && start_sel < s_vn) s_menu.focus = start_sel;

	/* Repurpose the top two cosmetic Options toggles as functional, persisted
	 * audio-mute settings: relabel them (fall back to a shorter label if the
	 * authored pool slot is too small) and seed the toggle state from the saved
	 * values. setting2 stays the untouched third cosmetic toggle. */
	relabel_row(0, "Mute BIOS Audio", "Mute BIOS");
	relabel_row(1, "Mute Menu Audio", "Mute Menu");
	snes_menu_set_language(&s_menu, lang_grid_of(pico_lang()));
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
		snes_target t0;
		unsigned int *fb0 = ayaneo_canvas_back(&pitch0, &W0, &H0);
		snes_target_fit(&t0, fb0, (int)pitch0, (int)W0, (int)H0);
		menu_sync_target(&t0);   /* 4:3 / 3:2 / 16:9 and the native scale (0.75 on the 960x640 Konkr canvas) */
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
	if (do_reverse) { g_dbg_reverse_ran++; gba_menu_reverse_punch(classic_reveal_frame); }

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

		gba_menu_read_input(&in);

		/* entry gate: while a launch button (A / Start) is still held from the close, the
		 * launch or a theme reload, force it off so no launch edge fires; clear once both
		 * release. After the injected key, which it covers like a real one. */
		if (launch_gate) {
			if (in.a || in.start) { in.a = 0; in.start = 0; }
			else launch_gate = 0;
		}

		g_menu_live = 1;
		menu_av_poll();   /* volume / brightness keys (+ deferred persist) */

		/* X cycles the system filter (All -> each present system -> All). Edge-detected with a
		 * short cooldown so contact bounce cannot skip a system within one press. */
		{
			static int px_last, x_cd;
			int xnow = PRESSED(K_X);
			if (x_cd > 0) x_cd--;
			if (xnow && !px_last && x_cd == 0) { cycle_filter(nrom); x_cd = 8; }
			px_last = xnow;
		}

		snes_target_fit(&t, fb, (int)pitch, (int)W, (int)H);
		menu_sync_target(&t);
		/* clear letterbox bars (wallpaper covers the design rows) */
		if (t.offy > 0) {
			ayaneo_fill(fb, pitch, 0, 0, (int)W, t.offy, 0xFF000000u);
			ayaneo_fill(fb, pitch, 0, t.offy + snes_target_vh(&t), (int)W, t.offy, 0xFF000000u);
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
					ayaneo_menu_settings_persist();	/* to the card, off this thread */
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
			if (g_dbg_peak_reset) { g_dbg_peak_reset = 0; peak_us = 0; peak_hold = 0; g_dbg_menu_frames = 0; g_dbg_menu_long = 0; }
			if (lus) { g_dbg_menu_frames++; if (lus > 20000u) g_dbg_menu_long++; }
			if (rus > peak_us) { peak_us = rus; peak_hold = 120; }
			else if (peak_hold) peak_hold--; else peak_us = rus;   /* decay after ~2s */
			g_dbg_render_us = rus; g_dbg_peak_us = peak_us; g_dbg_fps = fps;
			(void)s_show_hud;
		}
		if (fade_in > 0) {   /* fade in from white (or black) (255 -> 0) over 18 frames = 0.3s */
			ayaneo_fill_blend(fb, pitch, 0, 0, (int)W, (int)H, handover,
					  (fade_in * 255) / 18);
			fade_in--;
		}
		menu_av_draw(fb, pitch, (int)W, (int)H);   /* volume/brightness OSD bar */
		if (s_filter_label_hold > 0) {   /* transient "System: X" toast, top-left, drop shadow for legibility */
			char lbl[48]; int k = 0, j = 0;
			const char *pre = "System: ", *nm = filter_name(s_sysfilter);
			while (pre[k] && k < 40) { lbl[k] = pre[k]; k++; }
			while (nm[j] && k < 46) lbl[k++] = nm[j++];
			lbl[k] = 0;
			ayaneo_text(fb, pitch, 26, 20, 3, 0xFF000000u, lbl);   /* shadow */
			ayaneo_text(fb, pitch, 24, 18, 3, 0xFFFFFFFFu, lbl);
			s_filter_label_hold--;
		}
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
			for (i = 0; i < s_vn && i < 128; i++)   /* search the current view (index space the menu uses) */
				if (s_vtypes[i] == GBA_CONSOLE_SNES && dbg_name_match(s_vnameptr[i], g_dbg_snes_name)) { s_menu.launch = i; g_dbg_snes_chosen = s_vnameptr[i]; break; }
		}
		/* Display > Theme picked another theme: save it (written before the card is read again)
		 * and hand back -3, the caller runs the menu again, which loads the pack in the new theme */
		{
			int tr = snes_menu_take_theme(&s_menu);
			if (tr > 0 || (tr == 0 && s_theme_cur != 0)) {
				extern void ayaneo_set_menu_theme(unsigned int hash);
				extern void ayaneo_settings_persist_flush(void);
				if (tr < s_theme_n) {
					ayaneo_set_menu_theme(s_theme_hash[tr]);   /* Classic too, by its name: 0 is the default */
					ayaneo_menu_settings_persist();
					ayaneo_settings_persist_flush();
					s_keep_focus = s_menu.focus;   /* a view index, like start_sel below */
					snes_audio_init(&s_mix);
					ayaneo_menu_audio_silence();
					ayaneo_set_cpu_mhz(saved_mhz);
					return -3;
				}
			}
		}
		/* Language picked on its page: save it, and run the menu again in it, as for a theme */
		if (s_menu.lang_sel >= 0 && s_menu.lang_sel < 8 && k_lang_grid[s_menu.lang_sel] != pico_lang()) {
			extern void ayaneo_settings_persist_flush(void);
			pico_lang_set(k_lang_grid[s_menu.lang_sel]);
			ayaneo_menu_settings_persist();
			ayaneo_settings_persist_flush();
			s_keep_focus = s_menu.focus;
			snes_audio_init(&s_mix);
			ayaneo_menu_audio_silence();
			ayaneo_set_cpu_mhz(saved_mhz);
			return -3;
		}
		launch = snes_menu_take_launch(&s_menu);
		/* the menu returns a VIEW index (current filter); map it back to the full roms[] index */
		if (launch >= 0 && launch < s_vn) launch = (int)s_vmap[launch]; else launch = -1;
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
			snes_target_fit(&lt, cfb, (int)cp, (int)cw, (int)ch);
			menu_sync_target(&lt);
			if (lt.offy > 0) {
				ayaneo_fill(cfb, cp, 0, 0, (int)cw, lt.offy, 0xFF000000u);
				ayaneo_fill(cfb, cp, 0, lt.offy + snes_target_vh(&lt), (int)cw, lt.offy, 0xFF000000u);
			}
			snes_menu_render(&s_menu, &lt);
			gba_menu_launch_snapshot();   /* the frozen menu, on screen during the ROM load */
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
		gba_menu_present();
	}
	}
}
