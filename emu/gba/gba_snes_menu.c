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
extern void mtk_wdt_restart(void);
extern void thread_sleep(unsigned);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern int  partition_read(const char *name, unsigned long long off, void *buf, unsigned long len);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern void ayaneo_gbc_audio_init(void);
extern int  ayaneo_menu_audio_room(void);
extern void ayaneo_menu_audio_submit(const short *stereo, unsigned frames);

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

/* DRAM map (WB window [0x50000000,0x56000000) is all free during ROM-select).
 * Generous spacing: the packed blob can be up to 24MB (rgb565 firmware art). */
#define SNES_BLOB_PA   0x50000000u   /* decompressed pack (<=24MB) */
#define SNES_HOME_PA   0x51800000u   /* home rnode pool (16MB) */
#define SNES_BG_PA     0x52800000u   /* bg rnode pool (2MB) */
#define SNES_COMP_PA   0x52A00000u   /* compressed staging (deflate, <=8MB) */
#define SNES_WP_PA     0x53200000u   /* wallpaper cache (1536*720*4 = 4.2MB) */
#define SNES_CHROME_PA 0x53700000u   /* static chrome cache (1280*960*4 = 4.7MB) */
#define SNES_CTILE_PA  0x54000000u   /* card-tile cache (<=64 * 320*360*4 = 29.5MB) */
#define SNES_RAW_MAX   (24u * 1024 * 1024)
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

/* Build the display-name table from the SD roster (strip a trailing ".gba"). */
static void build_names(const gba_rom_entry *roms, int nrom)
{
	int i;
	for (i = 0; i < nrom && i < 128; i++) {
		int L = 0;
		const char *nm = roms[i].name;
		while (nm[L] && L < 127) { s_names[i][L] = nm[L]; L++; }
		s_names[i][L] = 0;
		if (L >= 4 && s_names[i][L-4] == '.' && (s_names[i][L-3]|32) == 'g' &&
		    (s_names[i][L-2]|32) == 'b' && (s_names[i][L-1]|32) == 'a')
			s_names[i][L-4] = 0;
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

	ayaneo_set_cpu_mhz(2000);
	snes_audio_init(&s_mix);
	ayaneo_gbc_audio_init();
	if (s_menu.bgm) play_sound(s_menu.bgm, 1);

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
		pump_audio();

		launch = snes_menu_take_launch(&s_menu);
		if (launch >= 0 && launch < nrom) {
			/* let the confirm SFX and a couple of frames land, then hand off */
			int f;
			for (f = 0; f < 6; f++) { pump_audio(); mtk_wdt_restart(); thread_sleep(16); }
			return launch;
		}

		ayaneo_canvas_present();   /* blocks on vsync -> paces to 60Hz */
		mtk_wdt_restart();
		{
			int p = pmic_detect_powerkey();
			if (!p) pwr_armed = 1; else if (pwr_armed) mt_power_off();
		}
	}
}
