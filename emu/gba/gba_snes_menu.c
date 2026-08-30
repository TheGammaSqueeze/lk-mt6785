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
#include "sd_fat.h"                 /* gba_rom_entry */

/* ---- LK / driver primitives (externs; no LK headers pulled in here) ---- */
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern void ayaneo_fill_blend(unsigned int *buf, unsigned int pitch_w,
			      int x, int y, int w, int h, unsigned int argb, int alpha);
extern void mtk_wdt_restart(void);
extern void thread_sleep(unsigned);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern int  partition_read(const char *name, unsigned long long off, void *buf, unsigned long len);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern void ayaneo_display_prepare(void);
extern void ayaneo_gbc_audio_init(void);
extern int  ayaneo_menu_audio_room(void);
extern void ayaneo_menu_audio_submit(const short *stereo, unsigned frames);
extern void ayaneo_menu_audio_silence(void);
extern int  ayaneo_present_skip_framedone;         /* 0 = present blocks on vsync */

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
#define PRESSED(g) (mt_get_gpio_in(GP(g)) == 0)

extern int pmic_detect_powerkey(void);
extern void mt_power_off(void);

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
#define SNES_CTILE_PA  0x54000000u   /* card-tile cache (<=64 * 320*360*4 = 29.5MB) */
/* the decompressed blob must stay strictly inside [BLOB_PA, HOME_PA); cap it 2MB
 * short of the 24MB region so it can never overrun the home node pool */
#define SNES_RAW_MAX   ((SNES_HOME_PA - SNES_BLOB_PA) - 2u * 1024 * 1024)  /* 22MB */
#define SNES_COMP_MAX  (8u  * 1024 * 1024)
#define HOME_CAP       (16u * 1024 * 1024 / (unsigned)sizeof(snes_rnode))
#define BG_CAP         (2u  * 1024 * 1024 / (unsigned)sizeof(snes_rnode))

static snes_pack s_pk;
static snes_menu s_menu;
static snes_mixer s_mix;
static short s_mixbuf[16384 * 2];

/* roster name storage (stripped ".gba"), + a pointer table for the menu */
static char        s_names[128][128];
static const char *s_nameptr[128];

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
static void clean_name(const char *nm, char *dst)
{
	int L = 0;
	while (nm[L] && L < 127) { dst[L] = nm[L]; L++; }
	dst[L] = 0;
	if (L >= 4 && dst[L-4] == '.' && (dst[L-3]|32) == 'g' &&
	    (dst[L-2]|32) == 'b' && (dst[L-1]|32) == 'a') { L -= 4; dst[L] = 0; }
	for (;;) {
		int e = L, c;
		while (e > 0 && dst[e-1] == ' ') e--;          /* trailing spaces */
		if (e <= 0) break;
		c = dst[e-1];
		if (c == ')' || c == ']') {                    /* a trailing tag group */
			int open = (c == ')') ? '(' : '[', j = e - 1, depth = 0;
			while (j >= 0) {
				if (dst[j] == c) depth++;
				else if (dst[j] == open && --depth == 0) break;
				j--;
			}
			if (j <= 0) { L = e; break; }          /* no opener / whole name */
			L = j; dst[L] = 0;                     /* cut before the group */
		} else { L = e; dst[L] = 0; break; }
	}
	if (L == 0) { dst[0] = 0; }                            /* keep at least "" */
}

/* Build the display-name table from the SD roster (cleaned titles). */
static void build_names(const gba_rom_entry *roms, int nrom)
{
	int i;
	for (i = 0; i < nrom && i < 128; i++) {
		clean_name(roms[i].name, s_names[i]);
		if (s_names[i][0] == 0) {                       /* pathological all-tag name */
			int L = 0; const char *nm = roms[i].name;
			while (nm[L] && L < 127) { s_names[i][L] = nm[L]; L++; }
			s_names[i][L] = 0;                       /* fall back to the raw name */
		}
		s_nameptr[i] = s_names[i];
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
	while ((h = snes_menu_next_sound(&s_menu)) != 0)
		play_sound(h, 0);
	need = ayaneo_menu_audio_room();
	if (need > 16384) need = 16384;
	if (need > 0) {
		snes_audio_mix(&s_mix, s_mixbuf, (unsigned)need);
		ayaneo_menu_audio_submit(s_mixbuf, (unsigned)need);
	}
}

/*
 * Run the SNES-style ROM selector. Returns the chosen ROM index, or -2 if the
 * SNES pack is missing (caller falls back to the plain list).
 */
int gba_snes_menu_run(const gba_rom_entry *roms, int nrom, int start_sel)
{
	const snes_img_entry *cart;
	int pwr_armed = 0;
	int fade_in = 18;   /* fade the menu in from black on entry over 0.3s, matching
			     * the SNES sys_fade IN_DURATION (reveal); the BIOS intro left the
			     * panel black. The first frame is fully black, which also hides the
			     * one-time wallpaper/chrome cache build hitch. */

	if (nrom <= 0) return -1;
	if (load_pack() != 0) return -2;

	if (snes_menu_init(&s_menu, &s_pk, (snes_rnode *)SNES_HOME_PA, HOME_CAP,
			   (snes_rnode *)SNES_BG_PA, BG_CAP, (uint32_t *)SNES_WP_PA,
			   (uint32_t *)SNES_CHROME_PA) != 0)
		return -2;

	build_names(roms, nrom);
	cart = snes_res_img(&s_pk, snes_hash("gba_cart"));
	snes_menu_set_gba_roster(&s_menu, s_nameptr, nrom, cart);
	if (start_sel >= 0 && start_sel < nrom) s_menu.focus = start_sel;

	/* card-tile cache: one CT tile per game in the free window at 0x54000000 */
	{
		static int s_ctile_gi[64];
		int cap = nrom; if (cap > 64) cap = 64; if (cap < 1) cap = 1;
		snes_menu_set_ctile(&s_menu, (uint32_t *)SNES_CTILE_PA, s_ctile_gi, cap);
	}

	ayaneo_set_cpu_mhz(2100);   /* max big-core OPP for render headroom */
	/* Present-pacing: blocking config_input on the panel FRAME_DONE (skip=0), exactly
	 * like the flicker-free GBA game in normal play. The present itself paces the loop
	 * to the panel refresh; the loop adds NO extra wait/timer (that overran a refresh
	 * and dropped frames = the movement flicker). */
	ayaneo_present_skip_framedone = 0;
	/* Re-own the panel in the clean canvas/double-buffer state. The BIOS-logo intro
	 * ran between the boot flow's display_prepare and here and left the layer in its
	 * own scan-out mode; without this the two canvas buffers can present
	 * inconsistently (flicker). This zeroes both buffers + reconfigures the layer. */
	ayaneo_display_prepare();
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

	for (;;) {
		unsigned int pitch, W, H;
		unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
		snes_target t;
		snes_input in;
		int launch;

		in.left = PRESSED(K_LEFT); in.right = PRESSED(K_RIGHT);
		in.up = PRESSED(K_UP); in.down = PRESSED(K_DOWN);
		in.a = PRESSED(K_A); in.b = PRESSED(K_B);
		in.start = PRESSED(K_START); in.select = PRESSED(K_SELECT);

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

		snes_menu_update(&s_menu, &in, 1.0f / 60.0f);
		snes_menu_render(&s_menu, &t);
		if (fade_in > 0) {   /* fade in from black (255 -> 0) over 18 frames = 0.3s */
			ayaneo_fill_blend(fb, pitch, 0, 0, (int)W, (int)H, 0xFF000000u,
					  (fade_in * 255) / 18);
			fade_in--;
		}
		pump_audio();

		launch = snes_menu_take_launch(&s_menu);
		if (launch >= 0 && launch < nrom) {
			int f;
			/* Launch transition: keep rendering the menu (frozen on the picked
			 * card) with a growing black overlay while the confirm SFX plays, so it
			 * fades out to black -> game instead of freezing then cutting. Matches
			 * the SNES-Classic launch feel. ~12 frames = ~0.2s at 60fps. */
			for (f = 0; f < 12; f++) {
				unsigned int lp, lw, lh;
				unsigned int *lfb = ayaneo_canvas_back(&lp, &lw, &lh);
				snes_target lt;
				lt.fb = lfb; lt.pitch = lp; lt.W = (int)lw; lt.H = (int)lh;
				lt.offx = ((int)lw - SNES_VW) / 2; lt.offy = ((int)lh - SNES_VH) / 2;
				snes_target_view(&lt, 1.0f, 1.0f, 0.0f, 0.0f);
				if (lt.offy > 0) {
					ayaneo_fill(lfb, lp, 0, 0, (int)lw, lt.offy, 0xFF000000u);
					ayaneo_fill(lfb, lp, 0, lt.offy + SNES_VH, (int)lw, lt.offy, 0xFF000000u);
				}
				snes_menu_render(&s_menu, &lt);
				ayaneo_fill_blend(lfb, lp, 0, 0, (int)lw, (int)lh, 0xFF000000u,
						  (f * 255) / 11);
				pump_audio();
				ayaneo_canvas_present();   /* blocks on vsync (skip=0) */
				mtk_wdt_restart();
			}
			/* Stop the BGM and zero the WHOLE audio ring before handing off: no one
			 * feeds the AFE ring while the game ROM loads/decompresses, and the DMA
			 * loops the entire 341ms ring, so a submit-at-write-cursor silence tail
			 * is not enough - it would still wrap and replay older BGM frames. Clear
			 * the mixer voices and wipe the whole ring so the DMA loops silence. */
			snes_audio_init(&s_mix);
			ayaneo_menu_audio_silence();
			return launch;
		}

		/* Present exactly like the (flicker-free) GBA game does in normal play:
		 * skip_framedone=0, so ayaneo_canvas_present() -> config_input BLOCKS on the
		 * panel FRAME_DONE, locking the loop to the panel refresh (one buffer shown
		 * per vsync). An earlier version stacked a wait_frame_done + a 13MHz timer
		 * FLOOR on top of this - that adds delay AFTER the vsync block, so the loop
		 * overruns a refresh and drops frames = the periodic flicker on movement.
		 * Do NOT add extra pacing here; the single blocking present is the pacing. */
		ayaneo_canvas_present();
		mtk_wdt_restart();
		{
			int p = pmic_detect_powerkey();
			if (!p) pwr_armed = 1; else if (pwr_armed) mt_power_off();
		}
	}
}
