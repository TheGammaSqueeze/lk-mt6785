/*
 * SNES-Classic-mini-style carousel menu for the GBA-from-SD flow. Replaces the
 * plain gba_sd_rom_select list. Assets (parallax wallpaper, GBA-cartridge tile,
 * BMFont, SFX/music) live in a snespack blob packed into boot_b at MENU_OFF by
 * tools/ayaneo/gba/build_menu_boot_b.py; the imported emu/gba/menu render engine
 * (snes_render/snes_pack) draws it with NEON hardware FP.
 *
 * gba_menu_run() renders the carousel to the LK canvas each frame, handles D-pad
 * nav with a smooth tween, and returns the picked ROM index (A). If the pack is
 * not present in boot_b it returns -2 so the caller falls back to the plain list
 * (never-brick: a missing menu asset must not break game selection).
 */
#include "menu/snes_pack.h"
#include "menu/snes_render.h"
#include "sd_fat.h"           /* gba_rom_entry */

/* ---- LK / driver primitives (externs; no LK headers pulled in here) ---- */
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern unsigned menu_keys(void);                 /* MK_* bit mask (gba_driver.c) */
extern void ayaneo_fill_blend(unsigned int *buf, unsigned int pitch_w,
			      int x, int y, int w, int h, unsigned int argb, int alpha);
extern void mtk_wdt_restart(void);
extern void thread_sleep(unsigned);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern int  partition_read(const char *name, unsigned long long off, void *buf, unsigned long len);
extern unsigned char *gba_core_scratch_ptr(void);
extern unsigned gba_core_scratch_size(void);

/* mirror of gba_driver.c menu_keys() bits */
enum { MK_UP=1, MK_DOWN=2, MK_LEFT=4, MK_RIGHT=8, MK_A=16, MK_B=32, MK_AYA=64 };

/* boot_b menu asset pack (see build_menu_boot_b.py) */
#define MENU_PART       "boot_b"
#define MENU_OFF        0x01100000ull
#define MENU_MAGIC      0x554E4D47u          /* "GMNU" */
#define MENU_PACK_PA    0x54000000u          /* free mapped-WB DRAM above the 64MB emu arena */

static uint32_t fnv1a(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
	return h;
}
static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

static snes_pack s_pk;
static int s_pk_ok;

/* Read + inflate the menu pack from boot_b into MENU_PACK_PA, open it. 0 = ok. */
static int menu_pack_load(void)
{
	unsigned char hdr[12];
	uint32_t magic, rawlen, complen;
	unsigned char *comp = gba_core_scratch_ptr();
	unsigned long zlen;
	if (s_pk_ok) return 0;
	if (!comp) return -1;
	if (partition_read(MENU_PART, MENU_OFF, hdr, 12) != 12) return -1;
	magic = rd32(hdr); rawlen = rd32(hdr + 4); complen = rd32(hdr + 8);
	if (magic != MENU_MAGIC || rawlen == 0 || complen == 0 ||
	    complen > gba_core_scratch_size())
		return -1;
	if (partition_read(MENU_PART, MENU_OFF + 12, comp, complen) != (long)complen)
		return -1;
	zlen = complen;
	if (zunzip(comp, &zlen, (void *)MENU_PACK_PA, (int)rawlen, 0) != 0) return -1;
	if (snes_pack_open(&s_pk, (void *)MENU_PACK_PA, rawlen) != 0) return -1;
	s_pk_ok = 1;
	return 0;
}

/* ---- carousel geometry (CLOVER virtual space: origin centre, +Y up, 1280x720) */
#define CARD_SPACING   360.0f    /* x distance between adjacent cards */
#define CARD_CY         40.0f    /* cards sit a little above centre; title below */
#define FOCUS_SCALE     1.15f
#define SIDE_SCALE      0.80f
#define VISIBLE_HALF    3        /* draw up to 3 cards each side of centre */

static float approach(float cur, float tgt, float rate)
{
	float d = tgt - cur;
	cur += d * rate;
	if (d < 0.6f && d > -0.6f) cur = tgt;
	return cur;
}

/* strip a trailing ".gba" for display */
static void disp_name(const char *nm, char *out, int cap)
{
	int L = 0;
	while (nm[L] && L < cap - 1) { out[L] = nm[L]; L++; }
	out[L] = 0;
	if (L >= 4 && out[L-4] == '.' &&
	    (out[L-3]|32)=='g' && (out[L-2]|32)=='b' && (out[L-1]|32)=='a')
		out[L-4] = 0;
}

/*
 * Run the carousel. roms[0..nrom) come from the SD scan. start_sel = initial
 * focus. Returns the chosen index, or -2 if the asset pack is missing (caller
 * falls back to the plain list).
 */
int gba_menu_run(const gba_rom_entry *roms, int nrom, int start_sel)
{
	const snes_spr_entry *wall, *cart;
	uint32_t font, font_big;
	int sel = (start_sel >= 0 && start_sel < nrom) ? start_sel : 0;
	float posf = (float)sel;          /* smooth scroll position (in card units) */
	int fade = 16;                    /* fade in from white, GBA-style */
	int held = 1;                     /* debounce: require release before repeat */

	if (nrom <= 0) return -1;
	if (menu_pack_load() != 0) return -2;

	wall = snes_res_spr(&s_pk, fnv1a("gbamenu/wallpaper.spr"));
	cart = snes_res_spr(&s_pk, fnv1a("gbamenu/cart.spr"));
	font = fnv1a("gbamenu/font");
	font_big = fnv1a("gbamenu/font_big");

	for (;;) {
		unsigned pitch, W, H;
		unsigned int *fb;
		snes_target t;
		unsigned k = menu_keys();
		int i;
		char nm[128];

		/* input (edge-triggered via simple release debounce) */
		if (!k) held = 0;
		if (!held) {
			if (k & (MK_LEFT|MK_UP))    { sel = (sel + nrom - 1) % nrom; held = 1; }
			else if (k & (MK_RIGHT|MK_DOWN)) { sel = (sel + 1) % nrom; held = 1; }
			else if (k & MK_A)          { return sel; }
		}

		/* tween the scroll toward the selected card, shortest wrap direction */
		{
			float tgt = (float)sel;
			float d = tgt - posf;
			if (d > nrom / 2.0f) tgt -= nrom;
			else if (d < -nrom / 2.0f) tgt += nrom;
			posf = approach(posf, tgt, 0.22f);
			if (posf < 0) posf += nrom;
			if (posf >= nrom) posf -= nrom;
		}

		fb = ayaneo_canvas_back(&pitch, &W, &H);
		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2; t.offy = ((int)H - SNES_VH) / 2;
		snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);

		/* wallpaper: stretch-fill the virtual area (parallax added later) */
		if (wall)
			snes_blit_tex(&t, &s_pk, &s_pk.img[wall->img], 0.0f, 0.0f,
				      (float)SNES_VW, (float)SNES_VH, 1.0f);
		else
			snes_fill_quad(&t, 0, 0, SNES_VW, SNES_VH, 0.10f, 0.08f, 0.22f, 1.0f);

		/* filmstrip of cards, farthest first so the focused one paints on top */
		for (i = VISIBLE_HALF; i >= -VISIBLE_HALF; i--) {
			int idx = sel + i; float rel;
			while (idx < 0) idx += nrom;
			while (idx >= nrom) idx -= nrom;
			/* card i's offset from centre using the tweened position */
			rel = (float)sel + (float)i - posf;
			{
				float cx = rel * CARD_SPACING;
				float f = 1.0f - (rel < 0 ? -rel : rel);   /* 1 at centre -> 0 */
				float sc = SIDE_SCALE + (FOCUS_SCALE - SIDE_SCALE) * (f < 0 ? 0 : f);
				float a = 0.55f + 0.45f * (f < 0 ? 0 : f);
				if (cx < -820 || cx > 820) continue;
				if (cart)
					snes_blit_spr(&t, &s_pk, cart, cx, CARD_CY, sc, a);
				/* small title on the card label for near cards */
				if (f > 0.25f && nrom) {
					disp_name(roms[idx].name, nm, sizeof nm);
					snes_draw_text(&t, &s_pk, font, cx, CARD_CY + 10 * sc,
						       0.55f * sc, 0xFF203040u, 1 /*centre*/, nm);
				}
			}
		}

		/* focused game title, large, below the strip */
		disp_name(roms[sel].name, nm, sizeof nm);
		snes_draw_text(&t, &s_pk, font_big, 0.0f, -250.0f, 1.0f, 0xFFFFFFFFu, 1, nm);
		snes_draw_text(&t, &s_pk, font, 0.0f, -315.0f, 0.7f, 0xFF9FB4E0u, 1,
			       "Left/Right: browse    A: play");

		if (fade > 0) {
			ayaneo_fill_blend(fb, pitch, 0, 0, (int)W, (int)H, 0xFFFFFFFFu,
					  (255 * fade) / 16);
			fade--;
		}
		ayaneo_canvas_present();
		mtk_wdt_restart();
		thread_sleep(16);
	}
}
