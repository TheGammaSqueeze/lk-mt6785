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
#include "menu/snes_audio.h"
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
extern int  ayaneo_menu_audio_room(void);
extern void ayaneo_menu_audio_submit(const short *stereo, unsigned frames);

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

/* ---- audio: BGM loop + move/confirm SFX via the menu mixer ---- */
static snes_mixer s_mix;
static short s_mixbuf[2048 * 2];

static void play_snd(const char *id, int loop, int gain, int is_bgm)
{
	const snes_snd_entry *s = snes_res_snd(&s_pk, fnv1a(id));
	if (!s) return;
	snes_audio_play(&s_mix, (const int16_t *)(s_pk.base + s->pcm), s->frames,
			s->rate, s->loop_start, s->loop_end, loop, gain, is_bgm);
}

/* keep the 48 kHz codec ring fed from the mixer, paced by the AFE read cursor */
static void pump_audio(void)
{
	int room = ayaneo_menu_audio_room();
	while (room > 0) {
		int n = room > 2048 ? 2048 : room;
		snes_audio_mix(&s_mix, s_mixbuf, (unsigned)n);
		ayaneo_menu_audio_submit(s_mixbuf, (unsigned)n);
		room -= n;
	}
}

/* ---- carousel geometry ----
 * The direct-blit space is the 1280x720 virtual area with ORIGIN TOP-LEFT and
 * Y DOWN (world (0,0) -> fb (offx,offy)); screen centre is (640,360). Sprites
 * are centred at the given (cx,cy). */
#define VW              1280.0f
#define VH              720.0f
#define CENTER_X        640.0f
#define CARD_SPACING    360.0f    /* x distance between adjacent cards */
#define CARD_CY         320.0f    /* card centre Y (cards ~220 tall around here) */
#define TITLE_Y         510.0f    /* focused game title, below the strip */
#define HINT_Y          590.0f
#define HEADER_Y         70.0f
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

/* Pure render of one carousel frame into the target (no LK deps) - shared by the
 * on-device loop and the host validation harness. sel = focused index, posf =
 * smooth (tweened) scroll position in card units. */
void gba_menu_render(snes_target *t, const snes_pack *pk,
		     const gba_rom_entry *roms, int nrom, int sel, float posf)
{
	const snes_spr_entry *wall = snes_res_spr(pk, fnv1a("gbamenu/wallpaper.spr"));
	const snes_spr_entry *cart = snes_res_spr(pk, fnv1a("gbamenu/cart.spr"));
	uint32_t font = fnv1a("gbamenu/font"), font_big = fnv1a("gbamenu/font_big");
	int i; char nm[128];

	snes_target_view(t, 1.0f, 1.0f, 0.0f, 0.0f);

	/* wallpaper: tiled parallax scroll. The tile img is IMG_TILE; uvrx/uvry are the
	 * repeat counts (dest / tile size), uvx scrolls slowly with the carousel so the
	 * background drifts opposite the cards for depth. */
	if (wall) {
		const snes_img_entry *wim = &pk->img[wall->img];
		float rx = VW / (float)wim->w, ry = VH / (float)wim->h;
		float uvx = posf * 0.06f, uvy = 0.0f;
		snes_blit_wallpaper(t, pk, wim, CENTER_X, VH / 2.0f, VW, VH,
				    uvx, uvy, rx, ry, 1.0f);
	} else {
		snes_fill_quad(t, CENTER_X, VH / 2.0f, VW, VH, 0.10f, 0.08f, 0.22f, 1.0f);
	}

	snes_draw_text(t, pk, font, CENTER_X, HEADER_Y, 0.8f, 0xFFDCE4FFu, 1,
		       "GBA GAMES");

	/* filmstrip of cards, farthest first so the focused one paints on top */
	for (i = VISIBLE_HALF; i >= -VISIBLE_HALF; i--) {
		int idx = sel + i;
		float rel = (float)sel + (float)i - posf;
		float cx = CENTER_X + rel * CARD_SPACING;
		float af = rel < 0 ? -rel : rel;
		float f = 1.0f - af;                       /* 1 at centre -> 0 */
		float sc = SIDE_SCALE + (FOCUS_SCALE - SIDE_SCALE) * (f < 0 ? 0 : f);
		float a = 0.55f + 0.45f * (f < 0 ? 0 : f);
		while (idx < 0) idx += nrom;
		while (idx >= nrom) idx -= nrom;
		if (cx < -180 || cx > VW + 180) continue;
		if (cart)
			snes_blit_spr(t, pk, cart, cx, CARD_CY, sc, a);
		/* short title on the cartridge label plate for near cards */
		if (f > 0.25f) {
			disp_name(roms[idx].name, nm, sizeof nm);
			snes_draw_text(t, pk, font, cx, CARD_CY - 40 * sc,
				       0.5f * sc, 0xFF203040u, 1, nm);
		}
	}

	/* focused game title (large) + hint, below the strip */
	disp_name(roms[sel].name, nm, sizeof nm);
	snes_draw_text(t, pk, font_big, CENTER_X, TITLE_Y, 1.0f, 0xFFFFFFFFu, 1, nm);
	snes_draw_text(t, pk, font, CENTER_X, HINT_Y, 0.7f, 0xFF9FB4E0u, 1,
		       "Left/Right: browse    A: play");
}

/*
 * Run the carousel. roms[0..nrom) come from the SD scan. start_sel = initial
 * focus. Returns the chosen index, or -2 if the asset pack is missing (caller
 * falls back to the plain list).
 */
int gba_menu_run(const gba_rom_entry *roms, int nrom, int start_sel)
{
	int sel = (start_sel >= 0 && start_sel < nrom) ? start_sel : 0;
	float posf = (float)sel;          /* smooth scroll position (in card units) */
	int fade = 16;                    /* fade in from white, GBA-style */
	int held = 1;                     /* debounce: require release before repeat */

	if (nrom <= 0) return -1;
	if (menu_pack_load() != 0) return -2;

	snes_audio_init(&s_mix);
	play_snd("gbamenu/music", 1 /*loop*/, 180, 1 /*bgm*/);

	for (;;) {
		unsigned pitch, W, H;
		unsigned int *fb;
		snes_target t;
		unsigned k = menu_keys();

		if (!k) held = 0;
		if (!held) {
			if (k & (MK_LEFT|MK_UP))         { sel = (sel + nrom - 1) % nrom; held = 1; play_snd("gbamenu/sfx_move", 0, 256, 0); }
			else if (k & (MK_RIGHT|MK_DOWN)) { sel = (sel + 1) % nrom; held = 1; play_snd("gbamenu/sfx_move", 0, 256, 0); }
			else if (k & MK_A)               { play_snd("gbamenu/sfx_confirm", 0, 256, 0); pump_audio(); return sel; }
		}
		pump_audio();

		/* tween the scroll toward the selected card, shortest wrap direction */
		{
			float tgt = (float)sel, d = tgt - posf;
			if (d > nrom / 2.0f) tgt -= nrom;
			else if (d < -nrom / 2.0f) tgt += nrom;
			posf = approach(posf, tgt, 0.22f);
			if (posf < 0) posf += nrom;
			if (posf >= nrom) posf -= nrom;
		}

		fb = ayaneo_canvas_back(&pitch, &W, &H);
		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2; t.offy = ((int)H - SNES_VH) / 2;

		gba_menu_render(&t, &s_pk, roms, nrom, sel, posf);

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
