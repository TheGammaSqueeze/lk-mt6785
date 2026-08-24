/*
 * LK-side driver for the SNES Classic home-menu port. After the boot animation,
 * LK decompresses the asset pack from boot_b into DRAM, builds the home scene and
 * renders it to the panel. This first milestone is a static (animated-later)
 * render that proves boot_b -> decompress -> pack -> scene -> render -> panel.
 *
 * Reuses the GBC harness (display canvas/present/text, boot_b partition_read,
 * boot hook, watchdog). Provides the ayaneo_gbc_* entry names the boot/charging
 * hooks call, so no hook changes are needed beyond the build gate.
 */
#include <debug.h>
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <part_interface.h>

#include "snes_render.h"

/* ---- LK primitives ---- */
extern void *memset(void *, int, unsigned int);
extern time_t current_time(void);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern void arch_clean_cache_range(unsigned long start, unsigned int len);
extern void mtk_wdt_disable(void);
extern void mtk_wdt_restart(void);
extern void ayaneo_display_prepare(void);
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int scale, unsigned int argb, const char *s);
extern void ayaneo_apply_persisted_brightness(void);
extern int  mt_power_off(void);
extern int  pmic_detect_powerkey(void);

/* ---- config ---- */
#define SNES_PART      "boot_b"
#define SNES_OFF       0x00400000u   /* 4 MB into boot_b (past the anim) */
#define SNES_MAGIC     0x5A534E53u   /* "SNSZ" */
#define SNES_BLOB_PA   0x50000000u   /* decompressed blob */
#define SNES_POOL_PA   0x50C00000u   /* rnode pool (after blob) */
#define SNES_POOL_MAX  (16u * 1024 * 1024 / (unsigned)sizeof(snes_rnode))
#define SNES_COMP_PA   0x51000000u   /* compressed staging */
#define SNES_RAW_MAX   (32u * 1024 * 1024)
#define SNES_COMP_MAX  (16u * 1024 * 1024)

static snes_pack  s_pk;
static snes_scene s_scene;   /* defaultscene (home) */
static snes_scene s_bg;      /* bg.scn (wallpaper) - instantiated by Lua at runtime;
                              * we build it statically so the pipeline shows art */
#define SNES_BG_POOL_PA 0x50E00000u
static snes_rnode *s_wall;   /* neon wallpaper node */

extern void *memcpy(void *, const void *, unsigned int);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);

/* ---- cached scrolling wallpaper ----
 * The neon wall is a single opaque tiled texture that only scrolls horizontally.
 * Rather than per-pixel sample it every frame (the perf killer), pre-render one
 * horizontal period into a cache buffer once, then each frame scroll-copy a
 * screen-width window (memcpy) - turning ~920k float-blit pixels into memcpy. */
#define WP_W 1536              /* one horizontal tile period, in screen px */
#define WP_H SNES_VH
#define SNES_WP_PA 0x52000000u /* 1536*720*4 = 4.4 MB cache in DRAM */
static uint32_t *s_wp = (uint32_t *)SNES_WP_PA;
static int s_wp_ready;
static float s_scroll;

static void build_wp(void)
{
	snes_rnode *w = s_wall;
	const snes_img_entry *im = 0;
	const uint8_t *pix;
	unsigned i;
	int sw, sh, bpp, cx, cy, y0, y1, band;
	float world[6], dw = WP_W, dh = 552, fy;
	if (!w) return;
	for (i = 0; i < w->def->comp_count; i++) {
		const snes_comp *c = snes_node_comp(&s_bg, w->def, i);
		if (c->type == COMP_TEXTURE) {
			const snes_comp_visual *cv = (const snes_comp_visual *)c;
			im = snes_res_img(&s_pk, cv->res_hash);
			if (c->flags & SNES_COMP_HAS_SIZE) { dw = cv->size_w; dh = cv->size_h; }
			break;
		}
	}
	if (!im) return;
	snes_node_world(w, world);
	fy = SNES_VH / 2.0f - world[5];        /* screen y of the wall centre */
	y0 = (int)(fy - dh / 2.0f); y1 = (int)(fy + dh / 2.0f);
	if (y0 < 0) y0 = 0; if (y1 > WP_H) y1 = WP_H;
	pix = snes_img_pixels(&s_pk, im); sw = im->w; sh = im->h;
	bpp = (im->flags & SNES_IMG_RGB565) ? 3 : 4;
	for (cy = 0; cy < WP_H; cy++) {
		uint32_t *row = s_wp + cy * WP_W;
		int ty = 0;
		band = (cy >= y0 && cy < y1);
		if (band) { ty = (int)((float)(cy - y0) * sh / (y1 - y0)); if (ty >= sh) ty = sh - 1; }
		for (cx = 0; cx < WP_W; cx++) {
			int tx, r, g, b;
			const uint8_t *sp;
			if (!band) { row[cx] = 0xFF000000u; continue; }
			tx = (int)((float)cx * sw / WP_W); if (tx >= sw) tx = sw - 1;
			sp = pix + ((unsigned)ty * sw + tx) * bpp;
			if (bpp == 3) {
				unsigned v = sp[0] | (sp[1] << 8);
				r = ((v >> 11) & 0x1f) << 3; r |= r >> 5;
				g = ((v >> 5) & 0x3f) << 2; g |= g >> 6;
				b = (v & 0x1f) << 3; b |= b >> 5;
			} else { r = sp[0]; g = sp[1]; b = sp[2]; }
			row[cx] = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
	}
	s_wp_ready = 1;
}

static void draw_wp(snes_target *t, int scroll_px)
{
	int cy, off = ((scroll_px % WP_W) + WP_W) % WP_W;
	if (!s_wp_ready) { ayaneo_fill(t->fb, t->pitch, 0, 0, t->W, t->H, 0xFF000000u); return; }
	for (cy = 0; cy < WP_H; cy++) {
		uint32_t *src = s_wp + cy * WP_W;
		uint32_t *dst = t->fb + (unsigned)(t->offy + cy) * t->pitch + t->offx;
		int first = WP_W - off;
		if (first > SNES_VW) first = SNES_VW;
		memcpy(dst, src + off, first * 4);
		if (first < SNES_VW) memcpy(dst + first, src, (SNES_VW - first) * 4);
	}
}

static int sstreq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
/* find a scene by basename in the pack scene table. */
static const snes_scene_entry *scene_by_name(const snes_pack *p, const char *nm)
{
	unsigned i;
	for (i = 0; i < p->hdr->scene_count; i++)
		if (sstreq(snes_str(p, p->scene[i].name), nm))
			return &p->scene[i];
	return 0;
}
/* tiny unsigned -> decimal, appended to p, returns end */
static char *u2s(char *p, unsigned v)
{
	char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n];
	return p;
}

/* on-screen status line (UART is unreliable on this device) */
static void dbg(const char *msg)
{
	unsigned int pitch, W, H, i;
	for (i = 0; i < 2; i++) {
		unsigned int *b = ayaneo_canvas_back(&pitch, &W, &H);
		ayaneo_fill(b, pitch, 0, 0, (int)W, 40, 0xFF000000u);
		ayaneo_text(b, pitch, 20, 8, 3, 0xFF40FF60u, msg);
		ayaneo_canvas_present();
	}
}

/* Load + decompress the pack payload from boot_b. Returns 0 ok. */
static int load_pack(void)
{
	unsigned char hdr[12];
	unsigned magic, rawlen, complen;
	unsigned char *comp = (unsigned char *)SNES_COMP_PA;
	unsigned char *blob = (unsigned char *)SNES_BLOB_PA;
	unsigned long zlen;

	if (partition_read(SNES_PART, SNES_OFF, hdr, 12) != 12)
		return -1;
	magic  = (unsigned)hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((unsigned)hdr[3] << 24);
	rawlen = (unsigned)hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((unsigned)hdr[7] << 24);
	complen= (unsigned)hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | ((unsigned)hdr[11] << 24);
	if (magic != SNES_MAGIC || rawlen == 0 || rawlen > SNES_RAW_MAX ||
	    complen == 0 || complen > SNES_COMP_MAX)
		return -2;
	if (partition_read(SNES_PART, SNES_OFF + 12, comp, complen) != (ssize_t)complen)
		return -3;
	zlen = complen;
	if (zunzip(comp, &zlen, blob, (int)rawlen, 0) != 0)
		return -4;
	if (snes_pack_open(&s_pk, blob, rawlen) != 0)
		return -5;
	return 0;
}

/* ---- input (gpio-keys, active-low; same pad map as the GBC/GBA driver) ---- */
extern int mt_set_gpio_mode(unsigned pin, unsigned mode);
extern int mt_set_gpio_dir(unsigned pin, unsigned dir);
extern int mt_set_gpio_pull_enable(unsigned pin, unsigned en);
extern int mt_set_gpio_pull_select(unsigned pin, unsigned sel);
extern int mt_get_gpio_in(unsigned pin);
#define GP(n)      ((n) | 0x80000000u)
#define PRESSED(g) (mt_get_gpio_in(GP(g)) == 0)
#define K_LEFT  78
#define K_RIGHT 80
#define K_UP    89
#define K_DOWN  79
#define K_A     83
#define K_B     82
static void gpio_in_pullup(unsigned g)
{
	mt_set_gpio_mode(GP(g), 0); mt_set_gpio_dir(GP(g), 0);
	mt_set_gpio_pull_enable(GP(g), 1); mt_set_gpio_pull_select(GP(g), 1);
}
static void input_init(void)
{
	gpio_in_pullup(K_LEFT); gpio_in_pullup(K_RIGHT);
	gpio_in_pullup(K_UP); gpio_in_pullup(K_DOWN);
	gpio_in_pullup(K_A); gpio_in_pullup(K_B);
}

/* ---- native home carousel (screen space, tuned to the web reference) ---- */
#define CAR_HGAP   262.0f
#define CAR_CY     338.0f
#define CAR_SLOTS  3            /* draw focus-3 .. focus+3 */
static int s_focus;            /* selected game index */

static const snes_game_rec *game(const snes_pack *p, int i)
{
	unsigned n = p->hdr->game_count;
	if (!n) return 0;
	i = ((i % (int)n) + (int)n) % (int)n;
	return (const snes_game_rec *)(p->base + p->game_offs[i]);
}

static void draw_card(snes_target *t, int d)
{
	const snes_pack *p = &s_pk;
	const snes_game_rec *g = game(p, s_focus + d);
	float cx = 640.0f + d * CAR_HGAP, cy = CAR_CY;
	int foc = (d == 0);
	float bw = foc ? 236.0f : 200.0f;
	float bh = bw * 160.0f / 228.0f;
	const snes_img_entry *im = (g && g->thumb_img != 0xFFFF) ? &p->img[g->thumb_img] : 0;
	if (foc) snes_fill_quad(t, cx, cy, bw + 18, bh + 18, 0.20f, 0.55f, 1.0f, 1.0f);
	else     snes_fill_quad(t, cx, cy, bw + 10, bh + 10, 0.06f, 0.07f, 0.10f, 1.0f);
	snes_fill_quad(t, cx, cy, bw + 4, bh + 4, 0.0f, 0.0f, 0.0f, 1.0f);
	if (im) snes_blit_tex(t, p, im, cx, cy, bw, bh, 1.0f);
	else    snes_fill_quad(t, cx, cy, bw, bh, 0.15f, 0.15f, 0.2f, 1.0f);
}

static void draw_carousel(snes_target *t)
{
	int k;
	if ((int)s_pk.hdr->game_count <= 0) return;
	for (k = CAR_SLOTS; k >= 1; k--) { draw_card(t, -k); draw_card(t, k); }
	draw_card(t, 0);   /* focused on top */
}

static void draw_title(snes_target *t)
{
	const snes_game_rec *g = game(&s_pk, s_focus);
	const char *nm;
	int len = 0;
	unsigned int pitch, W, H;
	unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);   /* same back buffer */
	(void)fb; (void)t;
	if (!g) return;
	nm = snes_str(&s_pk, g->name);
	while (nm[len]) len++;
	/* white title bar + centred name (LK 8x16 font, scale 3) above the carousel */
	ayaneo_fill(t->fb, t->pitch, t->offx + 240, t->offy + 96, SNES_VW - 480, 44, 0xFFF0F0F0u);
	ayaneo_text(t->fb, t->pitch, t->offx + 640 - len * 12, t->offy + 104, 3,
		    0xFF101010u, nm);
}

static int snes_emu_thread(void *arg)
{
	const snes_scene_entry *home;
	snes_rnode *root;
	int r;
	(void)arg;

	ayaneo_display_prepare();
	mtk_wdt_disable();
	dbg("SNES 1: loading pack");

	r = load_pack();
	if (r != 0) {
		char m[32]; m[0]='S';m[1]='N';m[2]='E';m[3]='S';m[4]=' ';m[5]='E';m[6]='R';
		m[7]='R';m[8]=' ';m[9]='0'-r; m[10]=0;   /* r is negative */
		dbg(m);
		for (;;) { mtk_wdt_restart(); thread_sleep(200); }
	}
	dbg("SNES 2: pack open, building scene");

	home = snes_res_scene(&s_pk, s_pk.init->default_scene_hash);
	if (!home) { dbg("SNES ERR: no default scene"); for (;;){ mtk_wdt_restart(); thread_sleep(200);} }
	root = snes_scene_build(&s_scene, &s_pk, home,
				(snes_rnode *)SNES_POOL_PA, SNES_POOL_MAX);
	if (!root) { dbg("SNES ERR: scene build (pool)"); for (;;){ mtk_wdt_restart(); thread_sleep(200);} }

	/* also build bg.scn (the wallpaper) - the firmware instantiates it at runtime;
	 * building it statically lets the pipeline show recognizable art now. */
	{
		const snes_scene_entry *bg = scene_by_name(&s_pk, "bg.scn");
		if (bg) {
			snes_scene_build(&s_bg, &s_pk, bg, (snes_rnode *)SNES_BG_POOL_PA,
					 2u * 1024 * 1024 / (unsigned)sizeof(snes_rnode));
			/* the home menu shows only the neon wallpaper (default_bg->wall);
			 * demo_bg is the Mario attract-mode parallax - disable it. */
			{ snes_rnode *demo = snes_scene_find(&s_bg, "demo_bg");
			  if (demo) demo->enabled = 0; }
			s_wall = snes_scene_find(&s_bg, "wall");
			build_wp();      /* pre-render the neon wallpaper cache */
		}
	}
	input_init();
	ayaneo_set_cpu_mhz(1800);   /* the menu is a full-screen software renderer */
	dbg("SNES 3: home carousel");
	ayaneo_apply_persisted_brightness();

	for (;;) {
		unsigned int pitch, W, H;
		unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
		snes_target t;
		static int lprev, rprev;
		int lnow = PRESSED(K_LEFT), rnow = PRESSED(K_RIGHT);
		char line[40], *p;

		/* input: Left/Right move the selection (edge-triggered) */
		if (lnow && !lprev) s_focus--;
		if (rnow && !rprev) s_focus++;
		lprev = lnow; rprev = rnow;

		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2;
		t.offy = ((int)H - SNES_VH) / 2;
		/* clear only the letterbox bars (the wallpaper covers the 720 region) */
		ayaneo_fill(fb, pitch, 0, 0, (int)W, t.offy, 0xFF000000u);
		ayaneo_fill(fb, pitch, 0, t.offy + SNES_VH, (int)W, t.offy, 0xFF000000u);
		/* cached wallpaper (memcpy scroll) + carousel */
		s_scroll += 1.2f;
		draw_wp(&t, (int)s_scroll);
		draw_carousel(&t);
		draw_title(&t);
		(void)snes_render_debug_markers; (void)u2s;

		/* top + bottom menu-layer bars (native placeholders for the menubar/HUD) */
		ayaneo_fill(fb, pitch, t.offx, t.offy, SNES_VW, 64, 0xC0101418u);
		ayaneo_fill(fb, pitch, t.offx, t.offy + SNES_VH - 56, SNES_VW, 56, 0xC0101418u);
		ayaneo_text(fb, pitch, t.offx + 24, t.offy + SNES_VH - 38, 2, 0xFFC8D0E0u,
			    "Menu    Suspend Point List        SELECT Sort    START Start Game");

		/* footer: game index / count */
		p = line;
		{ const char *s = "SNES  "; while (*s) *p++ = *s++; }
		{ int n = (int)s_pk.hdr->game_count, f = ((s_focus % n) + n) % n;
		  p = u2s(p, (unsigned)(f + 1)); *p++ = '/'; p = u2s(p, (unsigned)n); }
		*p = 0;
		ayaneo_text(fb, pitch, t.offx + SNES_VW - 120, t.offy + 22, 2, 0xFFFFFF00u, line);
		ayaneo_canvas_present();
		mtk_wdt_restart();

		/* power key -> off (so the device can be turned off from the menu) */
		{
			static int armed;
			int p = pmic_detect_powerkey();
			if (!p) armed = 1;
			else if (armed) mt_power_off();
		}
		thread_sleep(16);
	}
	return 0;
}

/* ---- entry points the boot/charging hooks call (names shared with the GBC build) ---- */
void ayaneo_gbc_start(void)
{
	thread_t *t = thread_create("ayaneo_snes", &snes_emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t)
		thread_resume(t);
}

/* charging path: for now just proceed into the menu (no offline-charging UI yet) */
void ayaneo_gbc_charging_screen(void) { }
int  ayaneo_gbc_select_held(void) { return 0; }
