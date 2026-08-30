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

/* deterministic per-game accent colour: hash the name to a hue, convert to rgb
 * (full sat/val). Returns 0..1 components. */
static void accent_rgb(const char *nm, float *r, float *g, float *b)
{
	float h = (float)(fnv1a(nm) % 360u) / 60.0f;   /* hue sector 0..6 */
	float x = 1.0f - (h - (float)(int)h);
	int s = (int)h;
	if (h - (float)s > 0.5f) x = (h - (float)s); else x = 1.0f - (h - (float)s);
	x = 1.0f - x; /* triangular 0..1 */
	switch (s % 6) {
	case 0: *r = 1;  *g = x;  *b = 0;  break;
	case 1: *r = x;  *g = 1;  *b = 0;  break;
	case 2: *r = 0;  *g = 1;  *b = x;  break;
	case 3: *r = 0;  *g = x;  *b = 1;  break;
	case 4: *r = x;  *g = 0;  *b = 1;  break;
	default:*r = 1;  *g = 0;  *b = x;  break;
	}
}

/* "sel/total" position label built without libc (LK render fn is freestanding) */
static void pos_label(int sel, int total, char *out)
{
	char tmp[16]; int n = 0, i, j = 0;
	int a = sel + 1;
	if (a <= 0) tmp[n++] = '0';
	while (a > 0) { tmp[n++] = '0' + (a % 10); a /= 10; }
	for (i = n - 1; i >= 0; i--) out[j++] = tmp[i];
	out[j++] = ' '; out[j++] = '/'; out[j++] = ' ';
	n = 0; a = total; if (a <= 0) tmp[n++] = '0';
	while (a > 0) { tmp[n++] = '0' + (a % 10); a /= 10; }
	for (i = n - 1; i >= 0; i--) out[j++] = tmp[i];
	out[j] = 0;
}

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
		     const gba_rom_entry *roms, int nrom, int sel, float posf,
		     float anim)
{
	/* pulse 0..1 for the focused-card glow (anim is a rising phase in radians) */
	float ph = anim - (float)(int)(anim / 6.2831853f) * 6.2831853f;
	float pulse = 0.5f + 0.5f * (ph < 3.14159f ? (ph / 3.14159f) : (2.0f - ph / 3.14159f));
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

	/* position counter, top-right of the header row */
	{
		char pl[24];
		pos_label(sel, nrom, pl);
		snes_draw_text(t, pk, font, VW - 40.0f, HEADER_Y, 0.6f, 0xFF9FB4E0u, 2, pl);
	}

	/* filmstrip of cards, farthest first so the focused one paints on top */
	for (i = VISIBLE_HALF; i >= -VISIBLE_HALF; i--) {
		int idx = sel + i;
		float rel = (float)sel + (float)i - posf;
		float cx = CENTER_X + rel * CARD_SPACING;
		float af = rel < 0 ? -rel : rel;
		float f = 1.0f - af;                       /* 1 at centre -> 0 */
		float sc = SIDE_SCALE + (FOCUS_SCALE - SIDE_SCALE) * (f < 0 ? 0 : f);
		float a = 0.55f + 0.45f * (f < 0 ? 0 : f);
		float ar, ag, ab;
		while (idx < 0) idx += nrom;
		while (idx >= nrom) idx -= nrom;
		if (cx < -180 || cx > VW + 180) continue;
		accent_rgb(roms[idx].name, &ar, &ag, &ab);
		/* soft glow behind the focused card, pulsing with the accent colour */
		if (f > 0.85f) {
			float gw = 360 * sc * (1.05f + 0.10f * pulse);
			float gh = 300 * sc * (1.05f + 0.10f * pulse);
			snes_fill_quad(t, cx, CARD_CY, gw, gh, ar, ag, ab,
				       0.16f * pulse * f);
		}
		/* drop shadow */
		snes_fill_quad(t, cx + 8 * sc, CARD_CY + 12 * sc,
			       300 * sc, 220 * sc, 0.0f, 0.0f, 0.0f, 0.30f * a);
		if (cart) {
			/* gentle accent tint: keep the cart bright, bias toward its hue */
			float tr = 0.72f + 0.28f * ar, tg = 0.72f + 0.28f * ag,
			      tb = 0.72f + 0.28f * ab;
			snes_blit_spr_tint(t, pk, cart, cx, CARD_CY, sc, a, tr, tg, tb);
		}
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
	int a_held = 1;                   /* A debounce: require release before re-fire */
	int nav_dir = 0;                  /* currently held nav direction (-1/0/+1) */
	int nav_timer = 0;                /* frames until the next auto-repeat step */
	float anim = 0.0f;                /* rising glow-pulse phase (radians) */
	const int NAV_DELAY = 22;         /* frames before auto-repeat kicks in (~0.36s) */
	const int NAV_REPEAT = 6;         /* frames between repeats while held (~0.10s) */

	if (nrom <= 0) return -1;
	if (menu_pack_load() != 0) return -2;

	snes_audio_init(&s_mix);
	play_snd("gbamenu/music", 1 /*loop*/, 180, 1 /*bgm*/);

	for (;;) {
		unsigned pitch, W, H;
		unsigned int *fb;
		snes_target t;
		unsigned k = menu_keys();
		int dir = (k & (MK_LEFT|MK_UP)) ? -1 : (k & (MK_RIGHT|MK_DOWN)) ? 1 : 0;

		/* directional nav: step on fresh press, then auto-repeat (delay -> fast)
		 * while the direction is held so long game lists scroll quickly. */
		if (dir == 0) { nav_dir = 0; nav_timer = 0; }
		else {
			int step = 0;
			if (dir != nav_dir) { step = 1; nav_dir = dir; nav_timer = NAV_DELAY; }
			else if (--nav_timer <= 0) { step = 1; nav_timer = NAV_REPEAT; }
			if (step) {
				sel = (sel + (dir < 0 ? nrom - 1 : 1)) % nrom;
				play_snd("gbamenu/sfx_move", 0, 256, 0);
			}
		}

		/* A = confirm, debounced against a held button */
		if (!(k & MK_A)) a_held = 0;
		if (!a_held && (k & MK_A)) {
			play_snd("gbamenu/sfx_confirm", 0, 256, 0); pump_audio(); return sel;
		}
		pump_audio();

		/* tween the scroll toward the selected card, shortest wrap direction */
		{
			float tgt = (float)sel, d = tgt - posf;
			if (d > nrom / 2.0f) tgt -= nrom;
			else if (d < -nrom / 2.0f) tgt += nrom;
			posf = approach(posf, tgt, 0.30f);
			if (posf < 0) posf += nrom;
			if (posf >= nrom) posf -= nrom;
		}

		fb = ayaneo_canvas_back(&pitch, &W, &H);
		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2; t.offy = ((int)H - SNES_VH) / 2;

		anim += 0.12f;   /* ~1.1s glow cycle at 60fps */
		gba_menu_render(&t, &s_pk, roms, nrom, sel, posf, anim);

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
