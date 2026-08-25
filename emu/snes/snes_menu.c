#include "snes_menu.h"

/* ---- optional per-phase render profiler ----
 * The driver sets g_perf_tick to a free-running counter (13 MHz on device); the
 * home render records each phase's elapsed ticks into g_perf[]. Zero cost when
 * g_perf_tick is NULL (host / release without the hook). */
unsigned (*g_perf_tick)(void) = 0;
unsigned g_perf[8];              /* 0 wp, 1 chrome, 2 carousel, 3 filmstrip, 4 rest */
static unsigned g_perf_last;
#define PERF_BEGIN() do { if (g_perf_tick) g_perf_last = g_perf_tick(); } while (0)
#define PERF_END(i)  do { if (g_perf_tick) { unsigned n_ = g_perf_tick(); \
	g_perf[i] = n_ - g_perf_last; g_perf_last = n_; } } while (0)

/* ---- small helpers ---- */
static const snes_scene_entry *scene_by_name(const snes_pack *p, const char *want)
{
	unsigned i;
	for (i = 0; i < p->hdr->scene_count; i++) {
		const char *a = snes_str(p, p->scene[i].name), *b = want;
		while (*a && *a == *b) { a++; b++; }
		if (*a == *b) return &p->scene[i];
	}
	return 0;
}
static int name_eq(const snes_pack *p, const snes_rnode *n, const char *want)
{
	const char *a = snes_str(p, n->def->name), *b = want;
	if (!a) return 0;
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
static snes_rnode *child_named(const snes_pack *p, snes_rnode *par, const char *nm)
{
	snes_rnode *c;
	if (!par) return 0;
	for (c = par->child; c; c = c->sib)
		if (name_eq(p, c, nm)) return c;
	return 0;
}
/* clear the ENABLED flag on a node's Label comps (Lua disables the authored
 * copyright text labels and draws the list itself). Mutates the DRAM blob. */
static void disable_labels(snes_scene *s, snes_rnode *n)
{
	unsigned i;
	if (!n) return;
	for (i = 0; i < n->def->comp_count; i++) {
		snes_comp *c = (snes_comp *)snes_node_comp(s, n->def, i);
		if (c->type == COMP_LABEL) c->flags &= ~SNES_COMP_ENABLED;
	}
}
/* recursively clear ENABLED on any sprite comp using the atlas frame at (sx,sy).
 * Used to hide one of a GUICheckButton's two authored toggle visuals (checked +
 * unchecked are both authored-on; the engine enables only one per state). */
static void disable_spr_comp(const snes_pack *pk, snes_scene *s, snes_rnode *n,
			     int sx, int sy)
{
	snes_rnode *ch;
	unsigned i;
	if (!n) return;
	for (i = 0; i < n->def->comp_count; i++) {
		snes_comp *c = (snes_comp *)snes_node_comp(s, n->def, i);
		if (c->type == COMP_SPRITE) {
			const snes_spr_entry *sp = snes_res_spr(pk, ((const snes_comp_visual *)c)->res_hash);
			if (sp && sp->sx == sx && sp->sy == sy) c->flags &= ~SNES_COMP_ENABLED;
		}
	}
	for (ch = n->child; ch; ch = ch->sib) disable_spr_comp(pk, s, ch, sx, sy);
}
/* recursively disable every descendant node named nm */
static void disable_all_named(const snes_pack *p, snes_rnode *n, const char *nm)
{
	snes_rnode *c;
	if (!n) return;
	if (name_eq(p, n, nm)) n->enabled = 0;
	for (c = n->child; c; c = c->sib) disable_all_named(p, c, nm);
}
/* recursively find the descendant named nm with the greatest world y (top of a
 * vertical list = the resting-selected item) and return it */
static snes_rnode *top_named(const snes_pack *p, snes_rnode *n, const char *nm,
			     snes_rnode *best, float *besty)
{
	snes_rnode *c;
	if (!n) return best;
	if (name_eq(p, n, nm)) {
		float w[6]; snes_node_world(n, w);
		if (!best || w[5] > *besty) { best = n; *besty = w[5]; }
	}
	for (c = n->child; c; c = c->sib) best = top_named(p, c, nm, best, besty);
	return best;
}
static const snes_game_rec *game_raw(const snes_pack *p, int idx)
{
	return (const snes_game_rec *)(p->base + p->game_offs[idx]);
}
/* game at carousel slot i, honouring the current sort order */
static const snes_game_rec *game(const snes_menu *m, int i)
{
	int n = m->ngames;
	if (n <= 0) return 0;
	i = ((i % n) + n) % n;
	return game_raw(m->pk, m->order[i]);
}
static int str_cmp(const snes_pack *p, uint32_t a, uint32_t b)
{
	const char *x = snes_str(p, a), *y = snes_str(p, b);
	if (!x) x = ""; if (!y) y = "";
	while (*x && *x == *y) { x++; y++; }
	return (int)(unsigned char)*x - (int)(unsigned char)*y;
}
/* order the roster by the current sort rule (stable insertion sort) */
static void apply_sort(snes_menu *m)
{
	const snes_pack *p = m->pk;
	int i, j;
	for (i = 1; i < m->ngames; i++) {
		unsigned short key = m->order[i];
		const snes_game_rec *gk = game_raw(p, key);
		j = i - 1;
		while (j >= 0) {
			const snes_game_rec *gj = game_raw(p, m->order[j]);
			int c;
			switch (m->sort_rule) {
			case 1:  c = str_cmp(p, gj->sort_publisher, gk->sort_publisher); break;
			case 2:  c = (int)gk->players - (int)gj->players; break;  /* most first */
			case 3:  c = (int)gj->release - (int)gk->release; break;  /* oldest first */
			default: c = str_cmp(p, gj->sort_title, gk->sort_title); break;
			}
			if (c <= 0) break;
			m->order[j + 1] = m->order[j];
			j--;
		}
		m->order[j + 1] = key;
	}
}
static void push_snd(snes_menu *m, uint32_t h)
{
	if (!h) return;
	m->sndq[m->sndt & 7] = h;
	m->sndt++;
}
uint32_t snes_menu_next_sound(snes_menu *m)
{
	if (m->sndh == m->sndt) return 0;
	return m->sndq[m->sndh++ & 7];
}

/* ---- wallpaper cache (pre-render once, memcpy scroll per frame) ---- */
static void build_wp(snes_menu *m)
{
	snes_rnode *w = m->wall;
	const snes_img_entry *im = 0;
	const uint8_t *pix;
	unsigned i;
	int sw, sh, bpp, cx, cy, y0, y1;
	float world[6], dw = WP_CACHE_W, dh = 552, fy;
	if (!w) return;
	for (i = 0; i < w->def->comp_count; i++) {
		const snes_comp *c = snes_node_comp(&m->bg, w->def, i);
		if (c->type == COMP_TEXTURE) {
			const snes_comp_visual *cv = (const snes_comp_visual *)c;
			im = snes_res_img(m->pk, cv->res_hash);
			if (c->flags & SNES_COMP_HAS_SIZE) { dw = cv->size_w; dh = cv->size_h; }
			break;
		}
	}
	if (!im) return;
	(void)dw;
	snes_node_world(w, world);
	fy = SNES_VH / 2.0f - world[5];
	y0 = (int)(fy - dh / 2.0f); y1 = (int)(fy + dh / 2.0f);
	if (y0 < 0) y0 = 0; if (y1 > WP_CACHE_H) y1 = WP_CACHE_H;
	pix = snes_img_pixels(m->pk, im); sw = im->w; sh = im->h;
	bpp = (im->flags & SNES_IMG_RGB565) ? 3 : 4;
	for (cy = 0; cy < WP_CACHE_H; cy++) {
		uint32_t *row = m->wp + cy * WP_CACHE_W;
		int ty = 0, band = (cy >= y0 && cy < y1);
		if (band) { ty = (int)((float)(cy - y0) * sh / (y1 - y0)); if (ty >= sh) ty = sh - 1; }
		for (cx = 0; cx < WP_CACHE_W; cx++) {
			int tx, r, g, b;
			const uint8_t *sp;
			if (!band) { row[cx] = 0xFF000000u; continue; }
			tx = (int)((float)cx * sw / WP_CACHE_W); if (tx >= sw) tx = sw - 1;
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
	m->wp_ready = 1;
}
static void draw_wp(snes_menu *m, snes_target *t, int scroll_px)
{
	int cy, off = ((scroll_px % WP_CACHE_W) + WP_CACHE_W) % WP_CACHE_W;
	if (!m->wp_ready) return;
	for (cy = 0; cy < WP_CACHE_H; cy++) {
		uint32_t *src = m->wp + cy * WP_CACHE_W;
		uint32_t *dst = t->fb + (unsigned)(t->offy + cy) * t->pitch + t->offx;
		int first = WP_CACHE_W - off;
		if (first > SNES_VW) first = SNES_VW;
		__builtin_memcpy(dst, src + off, first * 4);
		if (first < SNES_VW) __builtin_memcpy(dst + first, src, (SNES_VW - first) * 4);
	}
}

/* Resolve the authored game-card frame sprites + boxart-area geometry from the
 * sys_game_card.scn prefab, so draw_card can reproduce the cartridge-display
 * look natively. Sprite entries point into the pack (stable after build). */
static snes_rnode s_card_pool[192];
static const snes_spr_entry *node_spr(const snes_pack *p, snes_scene *s, snes_rnode *n)
{
	unsigned i;
	if (!n) return 0;
	for (i = 0; i < n->def->comp_count; i++) {
		const snes_comp *c = snes_node_comp(s, n->def, i);
		if (c->type == COMP_SPRITE) {
			const snes_comp_visual *cv = (const snes_comp_visual *)c;
			return snes_res_spr(p, cv->res_hash);
		}
	}
	return 0;
}
static snes_rnode *desc(const snes_pack *p, snes_rnode *n, const char *nm)
{
	snes_rnode *c, *r;
	if (!n) return 0;
	if (name_eq(p, n, nm)) return n;
	for (c = n->child; c; c = c->sib)
		if ((r = desc(p, c, nm))) return r;
	return 0;
}
static void resolve_card(snes_menu *m)
{
	const snes_scene_entry *ce = scene_by_name(m->pk, "sys_game_card.scn");
	snes_scene cs;
	snes_rnode *root, *act, *nac, *scr;
	m->card_act = m->card_norm = m->card_dot = m->card_dot_on = 0;
	m->card_fw = 252; m->card_fh = 276;
	m->screen_w = 228; m->screen_h = 204; m->screen_oy = 24;
	if (!ce) return;
	root = snes_scene_build(&cs, m->pk, ce, s_card_pool, 192);
	if (!root) return;
	act = desc(m->pk, root, "active");
	nac = desc(m->pk, root, "non_active");
	m->card_act  = node_spr(m->pk, &cs, act ? desc(m->pk, act, "card") : 0);
	m->card_norm = node_spr(m->pk, &cs, nac ? desc(m->pk, nac, "card") : 0);
	m->card_dot  = node_spr(m->pk, &cs, nac ? desc(m->pk, nac, "icon_1") : 0);
	{
		snes_rnode *pi = act ? desc(m->pk, act, "player_icon") : 0;
		m->card_pi = node_spr(m->pk, &cs, pi);   /* comp0 = icon_1P (sy 661) */
		/* card drawn at native scale (sc=1.0), so the player_icon scene world
		 * pos (81,-102) maps 1:1 to the screen offset from the card centre. */
		m->card_pi_wx = 81.0f; m->card_pi_wy = -102.0f;
	}
	scr = desc(m->pk, root, "screen");
	if (scr && scr->def->comp_count) {
		const snes_comp *c = snes_node_comp(&cs, scr->def, 0);
		const snes_comp_visual *cv = (const snes_comp_visual *)c;
		if (c->flags & SNES_COMP_HAS_SIZE) { m->screen_w = cv->size_w; m->screen_h = cv->size_h; }
		m->screen_oy = scr->tf[5];
	}
}

/* ---- static home-chrome cache ----
 * The homemenu chrome (menubar, title frame, bottom bar, filmstrip frames) is
 * identical every frame in the home state but costs ~4 ms to re-blit (large
 * atlas-sampled sprites). Render it ONCE into a screen-space overlay: seed the
 * buffer with a magenta sentinel, paint the chrome, then mark every untouched
 * pixel transparent (alpha 0). Each frame we just copy the covered pixels over
 * the scrolling wallpaper instead of walking the scene. */
#define CHROME_SENTINEL 0xFFFF00FFu
#define CHROME_MAXRUN 48
static uint16_t s_chrome_run[SNES_VH * CHROME_MAXRUN * 2];  /* (x0,x1) opaque runs/row */
static uint16_t s_chrome_nrun[SNES_VH];
static void build_chrome(snes_menu *m)
{
	snes_target ct;
	unsigned i, n = (unsigned)SNES_VW * SNES_VH;
	if (!m->chrome || !m->homemenu) return;
	for (i = 0; i < n; i++) m->chrome[i] = CHROME_SENTINEL;
	ct.fb = m->chrome; ct.pitch = SNES_VW; ct.W = SNES_VW; ct.H = SNES_VH;
	ct.offx = 0; ct.offy = 0;
	snes_render_node(&ct, &m->home, m->homemenu);
	for (i = 0; i < n; i++)
		m->chrome[i] = (m->chrome[i] == CHROME_SENTINEL) ? 0u : (0xFF000000u | m->chrome[i]);
	/* Precompute the opaque horizontal runs per row once, so draw_chrome (called
	 * every frame in the home/resume states) can memcpy the runs instead of doing
	 * a per-pixel alpha branch over 921600 pixels - a big per-frame CPU saving. */
	{
		int y, x;
		for (y = 0; y < SNES_VH; y++) {
			const uint32_t *src = m->chrome + (unsigned)y * SNES_VW;
			int nr = 0, x0 = -1;
			for (x = 0; x < SNES_VW; x++) {
				if (src[x] & 0xFF000000u) { if (x0 < 0) x0 = x; }
				else if (x0 >= 0) {
					if (nr < CHROME_MAXRUN) { s_chrome_run[(y*CHROME_MAXRUN+nr)*2]=(uint16_t)x0; s_chrome_run[(y*CHROME_MAXRUN+nr)*2+1]=(uint16_t)x; nr++; }
					x0 = -1;
				}
			}
			if (x0 >= 0 && nr < CHROME_MAXRUN) { s_chrome_run[(y*CHROME_MAXRUN+nr)*2]=(uint16_t)x0; s_chrome_run[(y*CHROME_MAXRUN+nr)*2+1]=(uint16_t)SNES_VW; nr++; }
			s_chrome_nrun[y] = (uint16_t)nr;
		}
	}
	m->chrome_ready = 1;
}
static void draw_chrome(snes_menu *m, snes_target *t)
{
	int y, i;
	if (!m->chrome_ready) { snes_render_node(t, &m->home, m->homemenu); return; }
	for (y = 0; y < SNES_VH; y++) {
		const uint32_t *src = m->chrome + (unsigned)y * SNES_VW;
		uint32_t *dst = t->fb + (unsigned)(t->offy + y) * t->pitch + t->offx;
		const uint16_t *r = &s_chrome_run[(unsigned)y * CHROME_MAXRUN * 2];
		int nr = s_chrome_nrun[y];
		for (i = 0; i < nr; i++) {
			int x0 = r[i*2], x1 = r[i*2+1];
			__builtin_memcpy(dst + x0, src + x0, (unsigned)(x1 - x0) * 4);
		}
	}
}

/* ---- carousel (ring + dead-zone, ports sys_gametitlelist) ----
 * The focused card rests at world x = SLOT_X (screen 247, left-of-centre). Cards
 * lie on a ring: card j at selWorld + HGAP*ringDelta(focus,j). Navigation walks
 * the focused card across the dead zone [-DEAD,+DEAD]; at the edge the whole
 * strip scrolls instead (container shift animates back to 0 at HGAP/REPEAT). */
#define CAR_HGAP   262.0f
#define CAR_SLOT_X (-393.0f)
#define CAR_DEAD   (CAR_HGAP * 1.5f)
#define CAR_REPEAT 0.24f
#define OPEN_SLIDE 688.0f   /* submenu panel slide-in distance (world up, screen px; frame-matched to the web open slide) */
#define CLOSE_DUR  0.22f    /* submenu close slide-up duration (cubic ease-in) */
#define RESUME_SLIDE 688.0f /* resume panel slide-up-from-bottom distance (screen px) */
/* opening the Suspend Point List raises the home content behind it
 * (resumemenu_position position_changer, markers gametitle/cardlist): the game
 * title lifts ~52px and the carousel ~69px. */
#define RESUME_TITLE_DY 52.0f
#define RESUME_CARD_DY  69.0f
#define CAP_DELAY  0.25f    /* MENUBAR_CAPTION_DELAY: wait before the caption scales in */
#define CAR_CY     360.0f                   /* cardlist container world y=0 (+card box offset) */
#define CAR_SC     1.0f                    /* native 252x276 -> ~230px card */

/* ---- carousel D-pad auto-repeat (GUI.H tween timings) ---- */
#define CAR_REPEAT_DELAY 0.22f  /* first held step delay */
#define CAR_REPEAT_RATE  0.06f  /* subsequent held-step interval */
#define CAR_XFADE        0.20f  /* blue selection-frame crossfade duration */

/* focused menubar cell drops this many world-y units below its authored row
 * position at full 1.2x focus (matches the web selection sitting ~5px lower) */
#define MB_FOCUS_DY 5.0f

/* ---- wallpaper parallax (CloverConst.BG.default, ported at 30fps) ---- */
#define BG_STEP            0.03333333f /* fixed 30fps parallax timestep */
#define BG_DEFAULT_SPEED   0.175f      /* DEFAULT_SCROLL_SPEED (anim units/sec) */
#define BG_CURSOR_SPEED    1.75f       /* SCROLL_CURSOR_SPEED (first-press burst) */
#define BG_CURSOR_ACCEL    0.2f        /* SCROLL_CURSOR_ACCEL */
#define BG_CURSOR_DECEL    0.2f        /* SCROLL_CURSOR_DECEL */
#define BG_CURSOR_SECONDS  0.2f        /* SCROLL_CURSOR_SECONDS (burst window) */
#define BG_CURSOR_DELAY    0.05f       /* SCROLL_CURSOR_DELAY */
#define BG_CURSOR_SPEED_MAX 4.0f       /* SCROLL_CURSOR_SPEED_MAX */
/* px advanced per anim-unit: preserves the prior tuned 72 px/s base at the
 * default speed 0.175 (72 / 0.175), so the neutral scroll is unchanged. */
#define BG_PX_PER_UNIT     (30.0f / BG_DEFAULT_SPEED)

static int ring_delta(int a, int b, int n)
{
	int d = (((b - a) % n) + n) % n;
	if (d > n / 2) d -= n;
	return d;
}

/* How far the Suspend Point List is open (0 = closed/just-starting, 1 = settled).
 * The home content (title + carousel) is raised in proportion. */
static float resume_open_frac(snes_menu *m)
{
	float ay, o;
	if (m->state != 3) return 0.0f;
	ay = m->open_y < 0.0f ? -m->open_y : m->open_y;
	o = 1.0f - ay / RESUME_SLIDE;
	return o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
}

/* A 9-slice selection cursor wrapping a w2xh2 box centred at (cx,cy): four 32x32
 * corner sprites + four stretched edges (w-31 / h-31); the centre (slice 5) is
 * transparent. sxa/sya index the atlas rects 1..9. Bright cyan with the engine's
 * 2.25s R,G colour pulse (blue channel stays 1). Ports sys_cursor.moveTo. */
static void draw_cursor_9slice(snes_menu *m, snes_target *t, float cx, float cy,
			       float w2, float h2, const uint16_t *sxa,
			       const uint16_t *sya, float alpha, float dim, uint16_t img)
{
	const float cw = 32.0f;
	const float hw = w2 / 2.0f, hh = h2 / 2.0f;
	const float ew = w2 - cw + 1.0f, eh = h2 - cw + 1.0f;
	float pt = m->clock - 2.25f * (float)((int)(m->clock / 2.25f));
	float v = (pt < 0.5f) ? 0.75f + pt / 0.5f * 0.25f
		: (pt < 1.25f) ? 1.0f
		: (pt < 2.0f) ? 1.0f - (pt - 1.25f) / 0.75f * 0.25f
		: 0.75f;
	float tr = v * dim, tg = v * dim, tb = 1.0f * dim;
	snes_spr_entry s = { img, 0, 0, 32, 32, 16, 16 };
	#define CUR_CORNER(i,px,py) do { s.sx = sxa[i]; s.sy = sya[i]; \
		snes_blit_spr_wh_tint(t, m->pk, &s, cx + (px), cy - (py), cw, cw, alpha, tr, tg, tb); } while (0)
	#define CUR_EDGE(i,px,py,dw,dh) do { s.sx = sxa[i]; s.sy = sya[i]; \
		snes_blit_spr_wh_tint(t, m->pk, &s, cx + (px), cy - (py), (dw), (dh), alpha, tr, tg, tb); } while (0)
	CUR_CORNER(1, -hw,  hh);  CUR_EDGE(2, 0.0f,  hh, ew, cw);  CUR_CORNER(3,  hw,  hh);
	CUR_EDGE(4, -hw, 0.0f, cw, eh);                            CUR_EDGE(6,  hw, 0.0f, cw, eh);
	CUR_CORNER(7, -hw, -hh);  CUR_EDGE(8, 0.0f, -hh, ew, cw);  CUR_CORNER(9,  hw, -hh);
	#undef CUR_CORNER
	#undef CUR_EDGE
}

/* rounded cursor (cursor_rounded / cursor_game_1..9) around the focused card's
 * `cursor` component (246x270). */
static void draw_card_cursor(snes_menu *m, snes_target *t, float cx, float cy,
			     float alpha, float dim, uint16_t img)
{
	static const uint16_t sxa[10] = {0,103,131,122,137,156,171,190,137,145};
	static const uint16_t sya[10] = {0,941,987,895,941,895,941,895,847,809};
	draw_cursor_9slice(m, t, cx, cy, 246.0f, 270.0f, sxa, sya, alpha, dim, img);
}

/* square cursor (cursor_square: cursor_1/3/7/9 corners + cursor_game_2/4/6/8
 * edges) around the focused menubar item's btn_menubar_active (96x70). */
static void draw_menubar_cursor(snes_menu *m, snes_target *t, float cx, float cy,
				float alpha, uint16_t img)
{
	static const uint16_t sxa[10] = {0, 69,131, 69,137,156,171, 88,137,103};
	static const uint16_t sya[10] = {0,847,987,945,941,895,941,895,847,847};
	draw_cursor_9slice(m, t, cx, cy, 96.0f, 70.0f, sxa, sya, alpha, 1.0f, img);
}

/* blue_a = opacity of the active (blue) card frame on top of the normal (dark)
 * one: 0 = unfocused, 1 = focused, in-between = the 0.2s selection crossfade.
 * dim = RGB tint (1 = normal; the resume menu darkens the non-focused cards to
 * 0.5, sys_game_card.cardColorToDark). */
static void draw_card(snes_menu *m, snes_target *t, int gi, float cx, float blue_a, float dim)
{
	const snes_game_rec *g = game(m, gi);
	float cy = CAR_CY, sc = CAR_SC;
	const snes_spr_entry *frame = m->card_norm ? m->card_norm : m->card_act;
	cy -= RESUME_CARD_DY * resume_open_frac(m);   /* raised behind the Suspend List */
	const snes_img_entry *im = (g && g->thumb_img != 0xFFFF) ? &m->pk->img[g->thumb_img] : 0;
	if (cx < -280 || cx > SNES_VW + 280) return;
	if (frame) {
		/* the `card` sprite is the screen BACKGROUND (blue when active, dark when
		 * not); draw it first, then the boxart on top. The boxart is scaled
		 * ASPECT-PRESERVING to fit the screen window, NOT stretched: web uses
		 * scale = min(screenX/w, screenX/h, 1) (sys_game_card.loadingDone), which
		 * for a 228x160 thumb is 1 -> native 228x160, centred in the window. The
		 * dark frame is the base; the blue frame cross-fades in on top (Tween over
		 * REPEAT/0.2s, sys_gametitlelist onElementFocus). */
		snes_blit_spr_tint(t, m->pk, frame, cx, cy, (m->card_fw / (float)frame->sw) * sc, 1.0f, dim, dim, dim);
		if (blue_a > 0.003f && m->card_act)
			snes_blit_spr_tint(t, m->pk, m->card_act, cx, cy,
				      (m->card_fw / (float)m->card_act->sw) * sc,
				      blue_a > 1.0f ? 1.0f : blue_a, dim, dim, dim);
		if (im) {
			float sf = m->screen_w / (float)im->w, sfh = m->screen_w / (float)im->h;
			float bw, bh;
			if (sfh < sf) sf = sfh;
			if (sf > 1.0f) sf = 1.0f;
			bw = im->w * sf * sc; bh = im->h * sf * sc;
			snes_blit_tex_tint(t, m->pk, im, cx, cy - m->screen_oy * sc, bw, bh, 1.0f, dim, dim, dim);
		}
		/* player-count icon (bottom-right): 1P / 2P-simultaneous / 1P-2P, chosen
		 * by players+simultaneous (sys_game_card_show.setPlayers). The player_icon
		 * node authors three sprite comps differing only in atlas sy (icon_1P=661,
		 * icon_1P-2P=645, icon_2P_simultaneous=677); pick the right one. */
		if (m->card_pi && g) {
			snes_spr_entry pi = *m->card_pi;
			pi.sy = (g->players <= 1) ? 661 : (g->simultaneous ? 677 : 645);
			snes_blit_spr_tint(t, m->pk, &pi, cx + m->card_pi_wx * sc,
				      cy - m->card_pi_wy * sc,
				      (m->card_fw / (float)frame->sw) * sc, 1.0f, dim, dim, dim);
		}
		/* suspend-point indicators (resume_icon): 4 empty rings along the card
		 * bottom-left. Resting/no-save state = icon_Resume_off (atlas 505,43,6x6,
		 * pivot 3,3). Scene wx -96,-72,-48,-24 (step 24), wy -108; the scene->
		 * screen mapping is 1:1 (screen offset from card centre == scene coord,
		 * same as the player_icon where 89*0.91 == 81). */
		{
			/* focused (active) card shows the cyan icon_Resume_off_active
			 * (atlas 505,51); unfocused cards the grey icon_Resume_off (505,43). */
			int di;
			uint16_t dsy = (blue_a > 0.5f) ? 51 : 43;
			float rscale = m->card_fw / (float)frame->sw;   /* 18px (web 1:1) */
			for (di = 0; di < 4; di++) {
				snes_spr_entry dot = { frame->img, 505, dsy, 6, 6, 3, 3 };
				float sx = -96.0f + 24.0f * (float)di;
				snes_blit_spr_tint(t, m->pk, &dot, cx + sx,
						   cy + 108.0f, rscale, 1.0f, dim, dim, dim);
			}
		}
		/* rounded selection cursor on the focused card - only while the carousel
		 * is the active focus (state 0 home); in the menubar/submenu states the
		 * cursor travels up to the menubar item, so the card shows none. */
		if (blue_a > 0.003f && m->state == 0)
			draw_card_cursor(m, t, cx, cy, blue_a > 1.0f ? 1.0f : blue_a,
					 dim, frame->img);
	} else {                                 /* fallback: plain framed boxart */
		int foc = blue_a > 0.5f;
		float bw = foc ? 236.0f : 200.0f, bh = bw * 160.0f / 228.0f;
		if (foc) snes_fill_quad(t, cx, cy, bw + 18, bh + 18, 0.20f, 0.55f, 1.0f, 1.0f);
		snes_fill_quad(t, cx, cy, bw + 4, bh + 4, 0.0f, 0.0f, 0.0f, 1.0f);
		if (im) snes_blit_tex(t, m->pk, im, cx, cy, bw, bh, 1.0f);
		else    snes_fill_quad(t, cx, cy, bw, bh, 0.15f, 0.15f, 0.2f, 1.0f);
	}
}
static void draw_carousel(snes_menu *m, snes_target *t)
{
	int n = m->ngames, j;
	/* blue-frame crossfade: on a nav the outgoing card's blue frame fades out
	 * while the incoming one's fades in over CAR_XFADE (sys_gametitlelist). */
	float prog = (m->xfade_t > 0.0f && m->prev_focus != m->focus)
		     ? m->xfade_t / CAR_XFADE : 0.0f;      /* 1 at onset -> 0 when done */
	/* resume menu darkens the NON-focused cards to 0.5 (cardColorToDark), fading
	 * in with resume_dim; the focused card stays full brightness. */
	float ndim = 1.0f - 0.5f * m->resume_dim;
	if (n <= 0) return;
	/* painter's order: draw non-focused first, focused last (on top) */
	for (j = 0; j < n; j++) {
		float wx, cx, blue_a;
		if (j == m->focus) continue;
		wx = m->sel_world + CAR_HGAP * (float)ring_delta(m->focus, j, n) + m->cont_shift;
		cx = 640.0f + wx;
		if (cx < -280 || cx > SNES_VW + 280) continue;
		blue_a = (j == m->prev_focus) ? prog : 0.0f;   /* outgoing card fades out */
		draw_card(m, t, j, cx, blue_a, ndim);
	}
	/* resume also darkens the focused card (~0.73) and drops its blue selection
	 * frame (the highlight moves to the panel); fade both with resume_dim. */
	{
		float fblue = (1.0f - prog) * (1.0f - m->resume_dim);
		float fdim = 1.0f - 0.27f * m->resume_dim;
		draw_card(m, t, m->focus, 640.0f + m->sel_world + m->cont_shift, fblue, fdim);
	}
}

/* ---- bottom thumbnail filmstrip (ports sys_thumbnail_icon: a fixed 21-icon
 * strip, NOT scrolling; icon i at world x=-410+41*i (screen 230+41*i), 32x32
 * bottom-anchored at world y=-191 (screen bottom 551); cursor over selected). */
#define TH_X0      230.0f    /* screen x of icon 0 (640-410) */
#define TH_SPACING 41.0f
#define TH_BOTTOM  551.0f    /* icon bottom edge */
#define TH_SIZE    32.0f
static void draw_filmstrip(snes_menu *m, snes_target *t)
{
	int n = m->ngames, i;
	float ccx, ccy = TH_BOTTOM - TH_SIZE / 2.0f;
	if (n <= 0) return;
	for (i = 0; i < n; i++) {
		const snes_game_rec *g = game(m, i);
		const snes_img_entry *im = (g && g->small_img != 0xFFFF) ? &m->pk->img[g->small_img] : 0;
		float cx = TH_X0 + TH_SPACING * (float)i;
		if (im) snes_blit_tex(t, m->pk, im, cx, ccy, TH_SIZE, TH_SIZE, 1.0f);
	}
	/* Animated downward-chevron cursor over the selected filmstrip icon
	 * (cursor_node: cursor_thumbnails.spriteanim, curor_thumbnail_1/2/3, 12x8,
	 * pivot 6,4). The node is scaled 3x with scaleY -3 (vertical flip), so the
	 * authored up-pointing arrowhead renders pointing DOWN onto the thumbnail.
	 * 13-frame 30fps blink pulsing full(1)->line(3)->full. Atlas: _1=145,881
	 * _2=159,881  _3=173,881. Cyan tint (63,191,255). */
	ccx = TH_X0 + TH_SPACING * (float)m->focus;
	if (m->card_act) {
		static const uint16_t seq[13] = { 0,1,1,1,1,2,2,2,1,1,0,0,0 };
		static const uint16_t sx[3] = { 145, 159, 173 };
		int fr = seq[((int)(m->clock / 0.03333f)) % 13];
		snes_spr_entry cur = { m->card_act->img, sx[fr], 881, 12, 8, 6, 4 };
		snes_blit_spr_tint_flip(t, m->pk, &cur, ccx, 511.0f, 3.0f, 1.0f,
					63.0f / 255.0f, 191.0f / 255.0f, 1.0f, 0, 1);
	}
}

/* a horizontal-flow HUD hint row (ports sys_hud): n items of button-icon +
 * localized label, laid left-to-right and centred at cx. Icons are 12/32-px
 * atlas sprites scaled x3. */
typedef struct { int sx, sy, sw, sh; const char *key; } snes_hint;
static void draw_hint_row(snes_menu *m, snes_target *t, const snes_hint *H, int n, float cx)
{
	const float sc = 0.85f, gap = 6.0f, vgap = 26.0f, y = 604.0f;
	const char *lab[6];
	float iw[6], lw[6], total = 0, x;
	int i;
	if (!m->card_act || n > 6) return;
	for (i = 0; i < n; i++) {
		lab[i] = snes_text(m->pk, H[i].key);
		iw[i] = H[i].sw * 3.0f;
		lw[i] = snes_text_width(m->pk, m->f_s, sc, lab[i]);
		total += iw[i] + gap + lw[i];
	}
	total += vgap * (n - 1);
	x = cx - total / 2.0f;
	for (i = 0; i < n; i++) {
		snes_spr_entry ic = { m->card_act->img, (uint16_t)H[i].sx, (uint16_t)H[i].sy,
				      (uint16_t)H[i].sw, (uint16_t)H[i].sh,
				      (int16_t)(H[i].sw / 2), (int16_t)(H[i].sh / 2) };
		snes_blit_spr(t, m->pk, &ic, x + iw[i] / 2.0f, y, 3.0f, 1.0f);
		snes_draw_text(t, m->pk, m->f_s, x + iw[i] + gap, y - 10.0f, sc,
			       0xFFF2F2F2u, 0, lab[i]);
		x += iw[i] + gap + lw[i] + vgap;
	}
}
/* draw `text` word-wrapped to wrapw px starting at (x,*y), advancing *y by lineh
 * per line (left/top anchored). Ports the copyright body's greedy wrap. */
static void draw_wrapped(snes_menu *m, snes_target *t, uint32_t font, float x, float *y,
			 float sc, uint32_t argb, float wrapw, float lineh, const char *text)
{
	char line[320];
	int ll = 0;
	const char *p = text;
	while (*p) {
		const char *ws = p;
		char trial[320];
		int wl, tl;
		while (*p && *p != ' ') p++;
		wl = (int)(p - ws);
		if (wl > 300) wl = 300;
		tl = ll ? ll + 1 + wl : wl;
		if (tl >= (int)sizeof(trial)) tl = (int)sizeof(trial) - 1;
		if (ll) { __builtin_memcpy(trial, line, ll); trial[ll] = ' '; __builtin_memcpy(trial + ll + 1, ws, wl); }
		else __builtin_memcpy(trial, ws, wl);
		trial[tl] = 0;
		if (ll && snes_text_width(m->pk, font, sc, trial) > wrapw) {
			line[ll] = 0;
			snes_draw_text(t, m->pk, font, x, *y, sc, argb, 0, line);
			*y += lineh;
			__builtin_memcpy(line, ws, wl); ll = wl; line[ll] = 0;
		} else {
			__builtin_memcpy(line, trial, tl); ll = tl;
		}
		while (*p == ' ') p++;
	}
	if (ll) { line[ll] = 0; snes_draw_text(t, m->pk, font, x, *y, sc, argb, 0, line); *y += lineh; }
}
/* the copyright IP-Notice body: the deduped game-copyright list, ordered by
 * (sort_publisher, release, sort_title), word-wrapped into the body area. */
static void draw_copyright_list(snes_menu *m, snes_target *t)
{
	const snes_pack *pk = m->pk;
	snes_rnode *body = child_named(pk, m->overlay[3], "body");
	snes_rnode *cp = body ? child_named(pk, body, "copyright") : 0;
	snes_rnode *txt = cp ? child_named(pk, cp, "text") : 0;
	/* the Lua overrides the authored s.font placeholder with copyright.fnt
	 * (the only body font that carries every digit/glyph) at runtime. */
	uint32_t font = snes_hash("copyright.fnt"), seen[128];
	unsigned short ord[128];
	int n = m->ngames, i, j, ns = 0;
	float y = 276.0f - m->open_y;   /* follow the panel slide-in (2px up to match web) */
	if (!txt || n <= 0) return;
	for (i = 0; i < n; i++) ord[i] = (unsigned short)i;
	for (i = 1; i < n; i++) {                       /* sort (publisher, release, title) */
		unsigned short k = ord[i];
		const snes_game_rec *gk = game_raw(pk, k);
		j = i - 1;
		while (j >= 0) {
			const snes_game_rec *gj = game_raw(pk, ord[j]);
			int c = str_cmp(pk, gj->sort_publisher, gk->sort_publisher);
			if (c == 0) c = (int)gj->release - (int)gk->release;
			if (c == 0) c = str_cmp(pk, gj->sort_title, gk->sort_title);
			if (c <= 0) break;
			ord[j + 1] = ord[j]; j--;
		}
		ord[j + 1] = k;
	}
	for (i = 0; i < n; i++) {
		const snes_game_rec *g = game_raw(pk, ord[i]);
		int dup = 0;
		if (!g->copyright) continue;
		for (j = 0; j < ns; j++) if (seen[j] == g->copyright) { dup = 1; break; }
		if (dup) continue;
		seen[ns++] = g->copyright;
		if (y > 625.0f) break;                  /* clip to the visible body */
		draw_wrapped(m, t, font, 168.0f, &y, 1.0f, 0xFFF0F0F0u, 900.0f, 24.0f,
			     snes_str(pk, g->copyright));
	}
}
/* home carousel hints: Menu / Suspend Point List / Sort / Start Game */
static void draw_hints(snes_menu *m, snes_target *t)
{
	static const snes_hint H[4] = {
		{ 85, 881, 12, 12, "sys_gametitlelist_hud_Menu" },
		{ 57, 881, 12, 12, "sys_gametitlelist_hud_Resume" },
		{ 88, 929, 32, 10, "sys_gametitlelist_hud_Sort" },
		{ 481, 783, 28, 10, "sys_gametitlelist_hud_Decide" },
	};
	draw_hint_row(m, t, H, 4, 626.0f);
}
/* menubar-focused hints: Game List / Select / Back / OK */
static void draw_menubar_hints(snes_menu *m, snes_target *t)
{
	static const snes_hint H[4] = {
		{ 57, 881, 12, 12, "sys_menubarU_hud_Gametitle" },  /* dpad down */
		{ 71, 881, 12, 12, "sys_menubarU_hud_Select" },     /* dpad lateral */
		{ 15, 881, 12, 12, "sys_menubarU_hud_Return" },     /* B */
		{  1, 881, 12, 12, "sys_menubarU_hud_Decide" },     /* A */
	};
	draw_hint_row(m, t, H, 4, 626.0f);
}
/* A rounded-corner rectangle drawn as two overlapping quads (a full-width bar
 * inset vertically + a full-height bar inset horizontally); the union leaves the
 * four corners cut by radius r, matching the caption/caption_edge 9-slice bubble
 * the web builds from authored sprites. */
static void round_rect(snes_target *t, float cx, float cy, float w, float h, float r,
		       float R, float G, float B, float A)
{
	if (r * 2.0f > w) r = w / 2.0f;
	if (r * 2.0f > h) r = h / 2.0f;
	snes_fill_quad(t, cx, cy, w, h - 2.0f * r, R, G, B, A);
	snes_fill_quad(t, cx, cy, w - 2.0f * r, h, R, G, B, A);
}
/* The focused menubar icon's caption bubble ("Display"/"Option"/...). The
 * authored box auto-sizes/positions at runtime (our static render placed it
 * low+mis-sized), so we hid the authored box+label in init and draw it here to
 * match the web exactly: a fixed 161x44 black box with a white border centred on
 * the icon at screen y117, the label centred inside. The authored arrow (kept
 * enabled) still points from the icon down to the box top. */
static void draw_menubar_caption(snes_menu *m, snes_target *t)
{
	snes_rnode *cap, *lbl;
	const snes_comp_label *cl = 0;
	const snes_font_entry *fe;
	const char *txt;
	float w[6], cx, cy = 135.0f, sc = 0.80f;
	unsigned i;
	int b = m->mb_focus;
	if (b < 0 || b >= 5) return;
	cap = m->mb_caption[b];
	if (!cap || !cap->enabled) return;
	lbl = child_named(m->pk, cap, "Label");
	if (!lbl) return;
	for (i = 0; i < lbl->def->comp_count; i++) {
		const snes_comp *c = snes_node_comp(&m->home, lbl->def, i);
		if (c->type == COMP_LABEL) { cl = (const snes_comp_label *)c; break; }
	}
	if (!cl) return;
	snes_node_world(cap, w);
	cx = 640.0f + w[2];
	txt = snes_str(m->pk, cl->text);
	if (txt[0] == '@') txt = snes_text(m->pk, txt + 1);
	/* caption draw-in (sys_menubar_cm.lua): the caption node starts at scale 0
	 * and, after MENUBAR_CAPTION_DELAY, scales uniformly to 1 (Ease.outExpo). The
	 * box (161x44 white border + black fill), the label, and the arrow all scale
	 * together about the box centre. */
	{
		float s = m->cap_s;
		if (s > 0.001f) {
			round_rect(t, cx, cy, 161.0f * s, 44.0f * s, 6.0f * s, 1.0f, 1.0f, 1.0f, 1.0f);
			if (s * 44.0f > 6.0f)
				round_rect(t, cx, cy, 155.0f * s, 38.0f * s, 5.0f * s, 0.0f, 0.0f, 0.0f, 1.0f);
			{
				uint32_t fh = snes_hash("m.font");
				float tsc = sc, tw;
				/* shrink long labels to fit the fixed 161px box (m.font runs
				 * wider than the web caption font, so e.g. "Legal Notices"
				 * would otherwise overflow the bubble). */
				tw = snes_text_width(m->pk, fh, tsc, txt);
				if (tw > 150.0f) tsc = tsc * 150.0f / tw;
				fe = snes_res_font(m->pk, fh);
				snes_draw_text(t, m->pk, fh, cx,
					       cy - (fe ? fe->line_height : 31) * tsc * s * 0.80f,
					       tsc * s, 0xFFF4F4F4u, 1, txt);
			}
		}
	}
	(void)cl;
}
/* full-screen overlay (Legal Notices) header hints: right-aligned row at the
 * top, ending just inside the header frame. */
static void draw_overlay_hints(snes_menu *m, snes_target *t, const snes_hint *H,
			       int n, float x_right, float y, float vgap)
{
	const float sc = 0.9f, gap = 6.0f, iscale = 2.55f;
	const char *lab[6];
	float iw[6], lw[6], total = 0, x;
	int i;
	if (!m->card_act || n > 6) return;
	for (i = 0; i < n; i++) {
		lab[i] = snes_text(m->pk, H[i].key);
		iw[i] = H[i].sw * iscale;
		lw[i] = snes_text_width(m->pk, m->f_s, sc, lab[i]);
		total += iw[i] + gap + lw[i];
	}
	total += vgap * (n - 1);
	x = x_right - total;
	for (i = 0; i < n; i++) {
		snes_spr_entry ic = { m->card_act->img, (uint16_t)H[i].sx, (uint16_t)H[i].sy,
				      (uint16_t)H[i].sw, (uint16_t)H[i].sh,
				      (int16_t)(H[i].sw / 2), (int16_t)(H[i].sh / 2) };
		snes_blit_spr(t, m->pk, &ic, x + iw[i] / 2.0f, y, iscale, 1.0f);
		snes_draw_text(t, m->pk, m->f_s, x + iw[i] + gap, y - 10.0f, sc,
			       0xFFF2F2F2u, 0, lab[i]);
		x += iw[i] + gap + lw[i] + vgap;
	}
}
/* Legal Notices (copyright overlay) header hints: Scroll / Select / Back */
static void draw_copyright_hints(snes_menu *m, snes_target *t)
{
	static const snes_hint H[3] = {
		{ 487, 185, 12, 12, "sys_copyright_hud_Scroll" },  /* vertical dpad */
		{  71, 881, 12, 12, "sys_copyright_hud_Page" },     /* lateral dpad = Select */
		{  15, 881, 12, 12, "sys_copyright_hud_Return" },   /* B = Back */
	};
	draw_overlay_hints(m, t, H, 3, 1152.0f, 74.0f - m->open_y, 26.0f);
}
/* Manuals overlay header hint: Back */
static void draw_manual_hints(snes_menu *m, snes_target *t)
{
	static const snes_hint H[1] = {
		{ 15, 881, 12, 12, "sys_manual_hud_Back" },   /* B = Back */
	};
	draw_overlay_hints(m, t, H, 1, 1180.0f, 74.0f - m->open_y, 26.0f);
}
/* Display panel line2 "Frame" strip: the decorative-frame thumbnails. The Lua's
 * DecorativeFrames lays these out at runtime from frames/<theme>/thumbnail pngs;
 * the packer stores them keyed "frame_thumb_<theme>". Slot 0 (None, the checkmark
 * box) is the authored item_0 (repositioned to the first slot in init); we draw
 * the themed thumbnails at slots 1..3. Native thumb is 172x98, drawn 1:1. */
static void draw_frame_strip(snes_menu *m, snes_target *t)
{
	static const char *themes[3] = {
		"frame_thumb_01_ambient", "frame_thumb_02_wire", "frame_thumb_03_crystal"
	};
	static const float xs[3] = { 541.0f, 737.0f, 933.0f };
	float cy = 591.0f - m->open_y;
	int i;
	for (i = 0; i < 3; i++) {
		const snes_img_entry *im = snes_res_img(m->pk, snes_hash(themes[i]));
		if (im) snes_blit_tex(t, m->pk, im, xs[i], cy, 172.0f, 98.0f, 1.0f);
	}
}
/* Display/Options/Language settings-panel header hints: Select / Back / OK.
 * (Same runtime-collapsed authored hud row as the other overlays.) */
static void draw_option_hints(snes_menu *m, snes_target *t)
{
	static const snes_hint OPT[3] = {
		{ 417, 325, 12, 12, "sys_option_hud_Select" },   /* 4-way dpad */
		{  15, 881, 12, 12, "sys_option_hud_Return" },   /* B = Back */
		{   1, 881, 12, 12, "sys_option_hud_Decide" },   /* A = OK */
	};
	static const snes_hint LANG[3] = {
		{ 417, 325, 12, 12, "sys_language_hud_Select" },
		{  15, 881, 12, 12, "sys_language_hud_Return" },
		{   1, 881, 12, 12, "sys_language_hud_Decide" },
	};
	draw_overlay_hints(m, t, m->open == 2 ? LANG : OPT, 3, 1180.0f, 74.0f - m->open_y, 41.0f);
}

/* ---- public ---- */
int snes_menu_init(snes_menu *m, const snes_pack *pk,
		   snes_rnode *home_pool, unsigned home_cap,
		   snes_rnode *bg_pool, unsigned bg_cap,
		   uint32_t *wp, uint32_t *chrome)
{
	const snes_scene_entry *home, *bg;
	unsigned i;
	/* zero the struct fields we rely on */
	m->pk = pk; m->wp = wp; m->wp_ready = 0; m->scroll = 0;
	m->chrome = chrome; m->chrome_ready = 0;
	m->focus = 0; m->car_x = 0; m->car_target = 0;
	m->sel_world = CAR_SLOT_X; m->cont_shift = 0;
	m->prev_focus = 0; m->xfade_t = 0.0f; m->resume_dim = 0.0f;
	m->pl = m->pr = m->pu = m->pd = m->pa = m->pb = m->ps = 0;
	m->sndh = m->sndt = 0; m->wall = 0;
	m->bg_acc = 0.0f; m->scr_speed = BG_DEFAULT_SPEED; m->scr_dir = 1.0f;
	m->cur_scroll_time = 0.0f; m->cur_scroll_spd = 0.0f;
	m->rep_t = 0.0f; m->rep_dir = 0; m->rep_primed = 0;
	m->car_tween = CAR_REPEAT_DELAY;

	(void)i;
	/* the real home menu (carousel + menubar) is defaultscene.scn, not the
	 * init default_scene (which is the boot/dialog scene sys_boot). */
	home = scene_by_name(pk, "defaultscene.scn");
	if (!home) home = snes_res_scene(pk, pk->init->default_scene_hash);
	if (!home || !snes_scene_build(&m->home, pk, home, home_pool, home_cap))
		return -1;
	bg = scene_by_name(pk, "bg.scn");
	if (bg && snes_scene_build(&m->bg, pk, bg, bg_pool, bg_cap)) {
		snes_rnode *demo = snes_scene_find(&m->bg, "demo_bg");
		if (demo) demo->enabled = 0;
		m->wall = snes_scene_find(&m->bg, "wall");
		build_wp(m);
	}
	/* on the home state Lua shows only `homemenu`; its siblings (copyright,
	 * manual, option_*, resumemenu, unlock_event) are overlays hidden here. */
	{
		static const char *hide[] = { "copyright", "manual", "option_display",
			"option_languages", "option_languages_first", "option_settings",
			"resumemenu", "unlock_event", "sys_autoplay", "dbg_menu", 0 };
		int hi;
		for (hi = 0; hide[hi]; hi++) {
			snes_rnode *o = snes_scene_find(&m->home, hide[hi]);
			if (o) o->enabled = 0;
		}
	}
	/* inside homemenu, the resume/suspend overlay is hidden on the home state */
	{
		static const char *hide2[] = { "sys_resumedummy", "resume_floating",
			"gametitle_label",
			/* the 4 home HUD hints are Lua-spread at runtime; statically they
			 * stack at one point, so hide them and draw our own hint row */
			"hud_gametitle", 0 };
		int hj;
		for (hj = 0; hide2[hj]; hj++) {
			snes_rnode *o = snes_scene_find(&m->home, hide2[hj]);
			if (o) o->enabled = 0;
		}
	}
	/* the game-title bar (gametitle -> caption_title) is drawn dynamically so it
	 * can be raised behind the Suspend Point List; keep it out of the chrome. */
	m->gametitle = snes_scene_find(&m->home, "gametitle");
	if (m->gametitle) m->gametitle->enabled = 0;
	m->homemenu = snes_scene_find(&m->home, "homemenu");
	/* The authored thumbnail cursor (a downward chevron the engine repositions
	 * over the selected filmstrip icon at runtime) is baked static at scene
	 * centre x640,y501 -> it would render "stuck in the middle" in our cached
	 * chrome. Hide the authored node and draw the animated chevron ourselves
	 * (see draw_filmstrip). */
	if (m->homemenu) disable_all_named(pk, m->homemenu, "cursor_node");
	m->menubar = snes_scene_find(&m->home, "menubar_upper");
	/* the real 5-icon row is menubar_upper -> elements -> cm1..cm5 (comps=2);
	 * snes_scene_find would return the hvc_position duplicates, so navigate. */
	{
		static const char *cm[5] = { "sys_menubar_cm1", "sys_menubar_cm2",
			"sys_menubar_cm3", "sys_menubar_cm4", "sys_menubar_cm5" };
		snes_rnode *els = child_named(pk, m->menubar, "elements");
		int b;
		for (b = 0; b < 5; b++) {
			snes_rnode *btn;
			m->mb_btn[b] = els ? child_named(pk, els, cm[b]) : 0;
			btn = child_named(pk, m->mb_btn[b], "button");
			m->mb_active[b] = child_named(pk, btn, "btn_menubar_active");
			m->mb_icon[b] = child_named(pk, btn, "btn_icon");
			m->mb_caption[b] = child_named(pk, m->mb_btn[b], "caption_down");
			if (m->mb_caption[b]) {
				m->mb_caption[b]->enabled = 0;
				/* The bubble box body + label are laid out/auto-sized by the
				 * engine at runtime; our static render places them ~20px low and
				 * mis-sized. Hide those parts (keep the arrow, which is correct)
				 * and draw the bubble synthetically (see draw_menubar_caption). */
				disable_all_named(pk, m->mb_caption[b], "caption");
				disable_all_named(pk, m->mb_caption[b], "caption_l");
				disable_all_named(pk, m->mb_caption[b], "caption_r");
				disable_all_named(pk, m->mb_caption[b], "Label");
			}
			/* the cyan active highlight is authored-on for every button; the
			 * real menu shows it only on the focused icon (and only in the
			 * menubar state), so start them all hidden. */
			if (m->mb_active[b]) m->mb_active[b]->enabled = 0;
			m->mb_scale[b] = 1.0f;
			m->mb_cell_y0[b] = m->mb_btn[b] ? m->mb_btn[b]->tf[5] : 0.0f;
		}
	}
	/* The top bar has a darker inset panel (menubar_U_2, 480px) behind the 5-icon
	 * strip: the full-width menubar_U renders ~189, the panel ~163. Both region
	 * bars (menubar_btn_nes 480px / menubar_btn_hvc 384px) are authored disabled
	 * and enabled by sys_menubarU per console region; this firmware runs the NES
	 * (480px) bar, matching the web reference exactly. Enable it so the chrome
	 * cache picks it up. */
	{
		snes_rnode *bn = snes_scene_find(&m->home, "menubar_btn_nes");
		if (bn) bn->enabled = 1;
	}
	/* per-icon settings overlays (display, options, language, copyright, manual) */
	m->overlay[0] = snes_scene_find(&m->home, "option_display");
	m->overlay[1] = snes_scene_find(&m->home, "option_settings");
	m->overlay[2] = snes_scene_find(&m->home, "option_languages");
	m->overlay[3] = snes_scene_find(&m->home, "copyright");
	m->overlay[4] = snes_scene_find(&m->home, "manual");
	/* manual overlay: hide the runtime-laid-out header Back hint (drawn
	 * synthetically, see draw_manual_hints) so it doesn't collapse/garble. */
	if (m->overlay[4]) disable_all_named(pk, m->overlay[4], "hud_item1_cancel");
	/* The manual body group ("all": the QR + "View..." + URL) renders ~3px right
	 * and ~3.5px low vs the web (the header/title align exactly); nudge the group
	 * back into registration. World +x = screen right, world +y = screen up. */
	if (m->overlay[4]) {
		snes_rnode *all = child_named(pk, m->overlay[4], "all");
		if (all) { all->tf[2] -= 3.0f; all->tf[5] += 3.5f; }
	}
	/* Display/Options/Language panels: same collapsed authored hint row; hide it
	 * (drawn synthetically by draw_option_hints). */
	{
		int oi;
		for (oi = 0; oi <= 2; oi++) if (m->overlay[oi]) {
			disable_all_named(pk, m->overlay[oi], "hud_item1_select");
			disable_all_named(pk, m->overlay[oi], "hud_item2_cancel");
			disable_all_named(pk, m->overlay[oi], "hud_item3_decide");
		}
	}
	m->resume = snes_scene_find(&m->home, "resumemenu");
	/* resume empty (no-save) resting state: hide the overlapping changemode
	 * animation card-sets + the emptymode explanation, leaving the cardlist's
	 * four "No Data" slots (sys_resumemenu empty branch). */
	if (m->resume) {
		disable_all_named(pk, m->resume, "changemode");
		disable_all_named(pk, m->resume, "emptymode");
		/* in the empty state each card hides its whole saved-data view
		 * (saved_card = blue fill + screen + frame + timer + icons), leaving
		 * the recessed slot + the "No Data" empty_label */
		disable_all_named(pk, m->resume, "saved_card");
		/* the header cube icon + "Suspend Point List" title are both authored at
		 * centre 640 (the runtime lays them out as a centred icon-left / text-
		 * right group). Find the title group and shift the cube left + text right
		 * to match web (cube centre ~467, text centre ~668). */
		{
			snes_rnode *st[64]; int sp = 0;
			st[sp++] = m->resume;
			while (sp) {
				snes_rnode *n = st[--sp], *ch, *cube = 0, *txt = 0;
				for (ch = n->child; ch; ch = ch->sib) {
					const char *nm = snes_str(pk, ch->def->name);
					if (nm && !strcmp(nm, "hud_2_label")) {
						unsigned i;
						for (i = 0; i < ch->def->comp_count; i++) {
							const snes_comp *c = snes_node_comp(&m->home, ch->def, i);
							if (c->type == COMP_LABEL) {
								const char *tx = snes_str(pk, ((const snes_comp_label *)c)->text);
								if (tx && !strcmp(tx, "@sys_resume_Title")) txt = ch;
							}
						}
					} else if (nm && !strcmp(nm, "hud_1_sprite")) cube = ch;
				}
				if (txt) { txt->tf[2] += 28.0f; if (cube) cube->tf[2] -= 173.0f; break; }
				for (ch = n->child; ch; ch = ch->sib) if (sp < 64) st[sp++] = ch;
			}
		}
	}
	/* option_display resting state (ports sys_option_display.setup): line 1
	 * (display modes) focused, frame-line focus box hidden, selected mode 4:3
	 * shows its blue selection box (cursor_area). */
	if (m->overlay[0]) {
		snes_rnode *ef = child_named(pk, m->overlay[0], "elements_frame");
		snes_rnode *fn = ef ? child_named(pk, ef, "focused_node") : 0;
		snes_rnode *el = child_named(pk, m->overlay[0], "elements");
		/* Frame strip: the authored None/checkmark box (elements_frame>elements>
		 * item_0) is centred; the web lays it at the first strip slot (screen
		 * x~347 = world -293). Move it there; draw_frame_strip adds the themed
		 * thumbnails at the following slots. */
		{
			snes_rnode *fel = ef ? child_named(pk, ef, "elements") : 0;
			snes_rnode *it0 = fel ? child_named(pk, fel, "item_0") : 0;
			if (it0) it0->tf[2] = -293.0f;
		}
		snes_rnode *sel = el ? child_named(pk, el, "item_4_3") : 0;
		snes_rnode *ca = sel ? child_named(pk, sel, "cursor_area") : 0, *it;
		if (fn) fn->enabled = 0;               /* hide Frame-line blue bar */
		if (ca) ca->enabled = 1;               /* blue box on selected 4:3 */
		/* disable the authored `cursor` dots (drawn as radiobtn in render) */
		for (it = el ? el->child : 0; it; it = it->sib) {
			snes_rnode *c2 = child_named(pk, it, "cursor");
			if (c2) c2->enabled = 0;
		}
		/* aspect-ratio radio: each item authors BOTH radiobtn_off (empty ring)
		 * and radiobtn_on (filled) enabled; the Lua shows _on only on the
		 * selected mode. Disable radiobtn_on (atlas 113,881) on every item
		 * except the selected 4:3 so the others show the empty ring. */
		for (it = el ? el->child : 0; it; it = it->sib) {
			if (!name_eq(pk, it, "item_4_3"))
				disable_spr_comp(pk, &m->home, it, 113, 881);
		}
	}
	/* option_languages resting state: `cursor` is the per-item focus ARROW
	 * (shown on all -> hide); enable `cursor_area` (blue box) on the selected
	 * language (English for the default usa_en locale, the top-left item). */
	if (m->overlay[2]) {
		snes_rnode *el = child_named(pk, m->overlay[2], "elements"), *it;
		for (it = el ? el->child : 0; it; it = it->sib) {
			snes_rnode *cur = child_named(pk, it, "cursor");
			snes_rnode *ca2 = child_named(pk, it, "cursor_area");
			snes_rnode *btn = child_named(pk, it, "button");
			if (cur) cur->enabled = 0;          /* hide focus arrow */
			if (btn) btn->enabled = 0;          /* hide authored dot (draw our own) */
			/* selected = current locale's language; default usa_en = language01
			 * (label key @sys_language_USA_en). Show its blue box. */
			if (ca2 && name_eq(pk, it, "language01")) ca2->enabled = 1;
		}
	}
	/* option_settings resting state: hide the per-item focus arrows, show the
	 * blue box on the top (default-selected) toggle item */
	if (m->overlay[1]) {
		float by = 0;
		snes_rnode *sel = top_named(pk, m->overlay[1], "cursor_area", 0, &by);
		disable_all_named(pk, m->overlay[1], "cursor");
		if (sel) sel->enabled = 1;
		/* each toggle authors BOTH the checked (ON, knob-right, atlas 481,755) and
		 * unchecked (OFF, knob-left, 481,737) sprites enabled -> two knobs. All
		 * settings default ON, so hide the OFF visual. */
		disable_spr_comp(pk, &m->home, m->overlay[1], 481, 737);
	}
	/* copyright panel resting state (IP Notice = the `copyright` tab selected):
	 * blue box on the copyright tab, hide the OSS tab_on + OSS body text */
	if (m->overlay[3]) {
		snes_rnode *menu = child_named(pk, m->overlay[3], "menu");
		snes_rnode *body = child_named(pk, m->overlay[3], "body");
		snes_rnode *cptab = menu ? child_named(pk, menu, "copyright") : 0;
		snes_rnode *osstab = menu ? child_named(pk, menu, "oss") : 0;
		snes_rnode *ossbody = body ? child_named(pk, body, "oss") : 0;
		snes_rnode *r;
		if (cptab) {
			if ((r = child_named(pk, cptab, "cursor_area"))) r->enabled = 1;
			if ((r = child_named(pk, cptab, "tab_off"))) r->enabled = 0;
		}
		if (osstab && (r = child_named(pk, osstab, "tab_on"))) r->enabled = 0;
		if (ossbody) ossbody->enabled = 0;
		/* Lua disables the authored body text labels + draws the list itself */
		if (body) {
			snes_rnode *cpb = child_named(pk, body, "copyright"), *tx;
			if (cpb && (tx = child_named(pk, cpb, "text"))) disable_labels(&m->home, tx);
		}
		/* the authored header hint row is laid out horizontally by the engine at
		 * runtime; the static transforms collapse all three items onto one spot,
		 * so hide them and draw the row synthetically (see draw_overlay_hints). */
		disable_all_named(pk, m->overlay[3], "hud_item1_scroll");
		disable_all_named(pk, m->overlay[3], "hud_item2_page");
		disable_all_named(pk, m->overlay[3], "hud_item3_back");
	}
	m->state = 0; m->mb_focus = 0; m->open = -1;
	m->open_y = 0.0f; m->closing = 0; m->close_t = 0.0f; m->close_target = 0.0f; m->cap_t = 0.0f; m->cap_s = 0.0f; m->hl_s = 0.0f;
	/* roster order (title sort by default) */
	m->ngames = (int)pk->hdr->game_count;
	if (m->ngames > 128) m->ngames = 128;
	for (i = 0; i < (unsigned)m->ngames; i++) m->order[i] = (unsigned short)i;
	m->sort_rule = 0; m->sort_label_t = 0;
	apply_sort(m);
	resolve_card(m);
	/* pre-render the static home chrome into the cache overlay (home state has
	 * no menubar highlight, so this snapshot is valid for home + resume) */
	build_chrome(m);
	m->f_title = snes_hash("title.font");
	m->f_l = snes_hash("l.font");
	m->f_s = snes_hash("s.font");
	/* sound resource hashes (fnv1a of the firmware GUIDs) */
	m->sfx_move   = snes_hash("5d71cecd-38d1-42d6-9f16-77581477eb97"); /* se_sys_cursor */
	m->sfx_decide = snes_hash("e8d6f848-db86-4b5f-9657-8a43a761a8d6"); /* se_sys_click_game */
	m->sfx_cancel = snes_hash("fd22b34f-5b1b-4162-92dd-f63277ca5af4"); /* se_sys_cancel */
	m->sfx_up     = snes_hash("9d492e12-d0fb-489b-8814-ff9a5c84493c"); /* se_sys_up */
	m->bgm        = snes_hash("29593b07-3016-49a0-9e70-c9d651bcafa2"); /* bgm_home */
	return 0;
}

/* ports sys_gametitlelist navigation: walk the focused card across the dead
 * zone; at the edge pin it and scroll the strip (container shift animates back) */
/* CloverScrollBG.ScrollLeft/ScrollRight: kick the parallax cursor burst in the
 * nav direction (dir = -1 left / +1 right), accumulating on rapid repeats. */
static void bg_scroll_kick(snes_menu *m, int dir)
{
	float s = (float)dir;
	if (m->cur_scroll_time == 0.0f) {
		m->cur_scroll_time = BG_CURSOR_SECONDS;
		m->cur_scroll_spd = s * BG_CURSOR_SPEED;
	} else {
		m->cur_scroll_time = BG_CURSOR_SECONDS - BG_CURSOR_DELAY;
		m->cur_scroll_spd += s * BG_CURSOR_ACCEL;
		if (m->cur_scroll_spd > BG_CURSOR_SPEED_MAX) m->cur_scroll_spd = BG_CURSOR_SPEED_MAX;
		else if (m->cur_scroll_spd < -BG_CURSOR_SPEED_MAX) m->cur_scroll_spd = -BG_CURSOR_SPEED_MAX;
	}
	m->scr_dir = s;
}

/* One fixed 30fps parallax step (ports CloverScrollBG.update_scroll). */
static void bg_step(snes_menu *m)
{
	float base = m->scr_dir * BG_DEFAULT_SPEED, target;
	m->cur_scroll_time -= BG_STEP;
	if (m->cur_scroll_time < 0.0f) m->cur_scroll_time = 0.0f;
	if (m->cur_scroll_time > 0.0f &&
	    m->cur_scroll_time < BG_CURSOR_SECONDS - BG_CURSOR_DELAY) {
		target = base + m->cur_scroll_spd;
	} else {
		if (m->cur_scroll_spd > 0.0f) {
			m->cur_scroll_spd -= BG_CURSOR_DECEL;
			if (m->cur_scroll_spd < 0.0f) m->cur_scroll_spd = 0.0f;
		} else if (m->cur_scroll_spd < 0.0f) {
			m->cur_scroll_spd += BG_CURSOR_DECEL;
			if (m->cur_scroll_spd > 0.0f) m->cur_scroll_spd = 0.0f;
		}
		target = base + m->cur_scroll_spd;
	}
	/* SCROLL_SMOOTH lerp toward target by 0.125 each step */
	m->scr_speed += (target - m->scr_speed) * 0.125f;
	m->scroll += BG_STEP * m->scr_speed * BG_PX_PER_UNIT;
}

static void car_navigate(snes_menu *m, int dir)
{
	int n = m->ngames;
	float old_sw = m->sel_world, cardShift, ns;
	if (n <= 0) return;
	m->prev_focus = m->focus;                 /* start the blue-frame crossfade */
	m->xfade_t = CAR_XFADE;
	m->focus = ((m->focus + dir) % n + n) % n;
	bg_scroll_kick(m, dir);
	/* card-slide tween time: first press over REPEAT_DELAY, held over REPEAT_RATE */
	m->car_tween = m->rep_primed ? CAR_REPEAT_RATE : CAR_REPEAT_DELAY;
	push_snd(m, m->sfx_move);
	ns = old_sw + dir * CAR_HGAP;
	if (ns < -CAR_DEAD) ns = -CAR_DEAD; else if (ns > CAR_DEAD) ns = CAR_DEAD;
	m->sel_world = ns;
	cardShift = (m->sel_world - old_sw) - dir * CAR_HGAP;   /* 0 walk, -dir*HGAP scroll */
	m->cont_shift = -cardShift;                            /* animates back to 0 */
}

void snes_menu_update(snes_menu *m, const snes_input *in, float dt)
{
	float k;
	int el = in->left && !m->pl, er = in->right && !m->pr;
	m->clock += dt;
	int eu = in->up && !m->pu, ed = in->down && !m->pd;
	int ea = in->a && !m->pa, eb = in->b && !m->pb;
	/* submenu open/close slide ease (BEFORE input handling so the trigger frame
	 * itself does not ease - matching the web's 1-frame input latency). Open:
	 * slide from off-top (open_y=OPEN_SLIDE) down to rest (0). Close: slide back
	 * up to OPEN_SLIDE then hide. Per-frame factor 0.67 at the web's 30fps. */
	if (m->open_y != 0.0f || m->closing) {
		float ke = 0.67f * (dt / 0.0333f);
		if (ke > 1.0f) ke = 1.0f;
		if (m->closing) {
			/* the web close is a strong ease-in (panel nearly still for ~3 frames
			 * then accelerates off); model as a cubic over CLOSE_DUR. Direction is
			 * close_target: +up (submenu, state 2) or -down (resume, state 3). */
			float r;
			m->close_t += dt;
			r = m->close_t / CLOSE_DUR;
			if (r >= 1.0f) {                       /* fully off-screen: finish */
				if (m->state == 3) {
					if (m->resume) m->resume->enabled = 0;
					m->state = 0;
				} else {
					if (m->open >= 0 && m->overlay[m->open])
						m->overlay[m->open]->enabled = 0;
					m->open = -1; m->state = 1;
				}
				m->closing = 0; m->open_y = 0.0f; m->close_t = 0.0f;
			} else {
				m->open_y = m->close_target * r * r * r * r;
			}
		} else {
			m->open_y -= m->open_y * ke;
			if (m->open_y < 1.0f && m->open_y > -1.0f) m->open_y = 0.0f;
		}
	}
	/* menubar caption: after MENUBAR_CAPTION_DELAY the caption scales 0->1 with an
	 * ease-out (the web uses Tween:scaleTo(caption,0.2,1,1,Ease.outExpo)); the
	 * geometric ease approximates outExpo's fast-rise shape, libm-free. */
	if (m->state == 1) {
		m->cap_t += dt;
		if (m->cap_t >= CAP_DELAY) {
			float ke = 0.55f * (dt / 0.0333f);
			if (ke > 1.0f) ke = 1.0f;
			m->cap_s += (1.0f - m->cap_s) * ke;
			if (m->cap_s > 0.999f) m->cap_s = 1.0f;
		}
		/* the focus highlight (cursor square) scales in fast, no delay (web: the
		 * cyan box grows to ~full in ~2 frames on focus). */
		{
			float kh = 0.8f * (dt / 0.0333f);
			if (kh > 1.0f) kh = 1.0f;
			m->hl_s += (1.0f - m->hl_s) * kh;
			if (m->hl_s > 0.999f) m->hl_s = 1.0f;
		}
	}

	if (m->state == 0) {                       /* ---- home carousel ---- */
		/* D-pad auto-repeat: step immediately on press, then after
		 * REPEAT_DELAY, then every REPEAT_RATE while held (GUI.H timings). */
		int dirnow = in->right ? 1 : (in->left ? -1 : 0);
		if (dirnow == 0) { m->rep_dir = 0; m->rep_t = 0.0f; m->rep_primed = 0; }
		else if (dirnow != m->rep_dir) {
			m->rep_dir = dirnow; m->rep_t = 0.0f; m->rep_primed = 0;
			car_navigate(m, dirnow);
		} else {
			float thresh = m->rep_primed ? CAR_REPEAT_RATE : CAR_REPEAT_DELAY;
			m->rep_t += dt;
			while (m->rep_t >= thresh) {
				m->rep_t -= thresh;
				m->rep_primed = 1;
				thresh = CAR_REPEAT_RATE;
				car_navigate(m, dirnow);
			}
		}
		if (eu) { m->state = 1; m->cap_t = 0.0f; m->cap_s = 0.0f; m->hl_s = 0.0f; push_snd(m, m->sfx_up); }
		if (ed && m->resume) {                 /* Down -> suspend-point menu */
			m->resume->enabled = 1;
			/* the panel slides UP from off-bottom into place, easing out (world
			 * -y = screen down); reuse the open_y ease (toward 0). */
			m->open_y = -RESUME_SLIDE;
			m->state = 3; push_snd(m, m->sfx_decide);
		}
		if (ea) push_snd(m, m->sfx_decide);    /* launch stubbed */
		if (in->select && !m->ps) {            /* cycle roster sort */
			const snes_game_rec *cur = game(m, m->focus);
			m->sort_rule = (m->sort_rule + 1) % 4;
			apply_sort(m);
			/* keep the focused game selected after the reorder */
			if (cur) {
				int k;
				for (k = 0; k < m->ngames; k++)
					if (game(m, k) == cur) { m->focus = k; m->car_x = k; break; }
			}
			m->sel_world = CAR_SLOT_X; m->cont_shift = 0;
			m->sort_label_t = 1.5f;
			push_snd(m, m->sfx_decide);
		}
	} else if (m->state == 1) {                /* ---- menubar row ---- */
		if (el) { m->mb_focus = (m->mb_focus + 4) % 5; m->cap_t = 0.0f; m->cap_s = 0.0f; m->hl_s = 0.0f; push_snd(m, m->sfx_move); }
		if (er) { m->mb_focus = (m->mb_focus + 1) % 5; m->cap_t = 0.0f; m->cap_s = 0.0f; m->hl_s = 0.0f; push_snd(m, m->sfx_move); }
		if (ed || eb) { m->state = 0; push_snd(m, m->sfx_cancel); }
		if (ea && m->overlay[m->mb_focus]) {
			m->open = m->mb_focus;
			m->overlay[m->open]->enabled = 1;
			m->overlay[m->open]->tf[2] = 0;
			m->overlay[m->open]->tf[5] = 0;
			/* the panel slides DOWN from off-top into place, easing out (measured
			 * against the web: the selection box travels ~585px over ~5 frames at
			 * 30fps with per-frame factor ~0.67). Seed the slide offset; the ease
			 * in snes_menu_update brings it to 0. World +y = screen up. */
			m->open_y = OPEN_SLIDE;
			m->state = 2; push_snd(m, m->sfx_decide);
		}
	} else if (m->state == 2) {                /* ---- open submenu ---- */
		/* Language screen (cm3): L/R pick a locale, re-localizing all text live */
		if (m->open == 2 && (el || er)) {
			snes_pack *mp = (snes_pack *)m->pk;
			int nl = (int)mp->hdr->str_count;
			if (nl > 0) {
				mp->locale = (mp->locale + (er ? 1 : nl - 1)) % nl;
				push_snd(m, m->sfx_move);
			}
		}
		if (eb && !m->closing) {
			/* slide the panel back up/off before hiding (see the ease above) */
			m->closing = 1; m->close_target = OPEN_SLIDE; m->close_t = 0.0f;
			push_snd(m, m->sfx_cancel);
		}
	} else {                                   /* ---- resume menu ---- */
		if ((eb || eu) && !m->closing) {
			/* slide the resume panel back down/off before hiding */
			m->closing = 1; m->close_target = -RESUME_SLIDE; m->close_t = 0.0f;
			push_snd(m, m->sfx_cancel);
		}
	}
	m->pl = in->left; m->pr = in->right; m->pu = in->up; m->pd = in->down;
	m->pa = in->a; m->pb = in->b; m->ps = in->select;
	if (m->sort_label_t > 0) m->sort_label_t -= dt;

	/* menubar: cyan highlight + focus-scale (1.2x) only on the focused icon */
	{
		int b;
		float ks = dt * 12.0f; if (ks > 1.0f) ks = 1.0f;
		for (b = 0; b < 5; b++) {
			/* highlight only in the menubar (1) and open-submenu (2) states,
			 * NOT in the resume menu (3) where the menubar isn't focused */
			int foc = ((m->state == 1 || m->state == 2) && b == m->mb_focus);
			if (m->mb_active[b]) {
				m->mb_active[b]->enabled = foc;
				/* scale the highlight in from 0 on focus (authored scale is 1.0);
				 * settled + open-submenu states use full scale. */
				m->mb_active[b]->tf[0] = m->mb_active[b]->tf[4] =
					(foc && m->state == 1) ? m->hl_s : 1.0f;
			}
			if (m->mb_caption[b]) m->mb_caption[b]->enabled = foc;
			if (m->mb_btn[b]) {
				/* sys_menubar_cm.activate scales ONLY the btn_icon to 1.2x
				 * (outExpo); the cyan highlight + button stay at 1.0. */
				float tgt = foc ? 1.2f : 1.0f;
				m->mb_scale[b] += (tgt - m->mb_scale[b]) * ks;
				if (m->mb_icon[b])
					m->mb_icon[b]->tf[0] = m->mb_icon[b]->tf[4] = m->mb_scale[b];
				/* the web drops the focused cell ~5 screen px vs its authored row
				 * position; nudge tf[5] down (world -y) with the grow-in. */
				m->mb_btn[b]->tf[5] = m->mb_cell_y0[b]
					- MB_FOCUS_DY * (m->mb_scale[b] - 1.0f) / 0.2f;
			}
		}
	}

	/* carousel container scroll: animate cont_shift back to 0 at constant rate
	 * (HGAP/REPEAT px/s), matching the linear scroll tween */
	{
		float tw = m->car_tween > 0.0f ? m->car_tween : CAR_REPEAT;
		float rate = (CAR_HGAP / tw) * dt;
		if (m->cont_shift > 0) { m->cont_shift -= rate; if (m->cont_shift < 0) m->cont_shift = 0; }
		else if (m->cont_shift < 0) { m->cont_shift += rate; if (m->cont_shift > 0) m->cont_shift = 0; }
	}
	/* blue selection-frame crossfade timer */
	if (m->xfade_t > 0.0f) { m->xfade_t -= dt; if (m->xfade_t < 0.0f) m->xfade_t = 0.0f; }
	/* resume card-darken fade: ramp in while the suspend list is open (state 3,
	 * not closing), back out on close, over ~0.2s (matches the web fade). */
	{
		int on = (m->state == 3 && !m->closing);
		float step = dt / 0.20f;
		if (on) { m->resume_dim += step; if (m->resume_dim > 1.0f) m->resume_dim = 1.0f; }
		else    { m->resume_dim -= step; if (m->resume_dim < 0.0f) m->resume_dim = 0.0f; }
	}
	/* filmstrip smooth index follow */
	k = dt * 12.0f; if (k > 1.0f) k = 1.0f;
	m->car_x += ((float)m->focus - m->car_x) * k;

	/* wallpaper parallax scroll (ported CloverScrollBG, fixed 30fps step). The
	 * Suspend Point List (state 3) freezes the wallpaper: the device sleeps the
	 * "bg" task and holds the last frame, so we simply stop stepping. */
	if (m->state != 3) {
		m->bg_acc += dt;
		while (m->bg_acc >= BG_STEP) { bg_step(m); m->bg_acc -= BG_STEP; }
	}
}

void snes_menu_render(snes_menu *m, snes_target *t)
{
	const snes_game_rec *g;

	/* Submenu (state 2) is a full-screen opaque panel over a black scrim: the
	 * home/wallpaper/carousel behind it are 100% covered, so skip rendering them
	 * entirely. This was ~the whole frame's cost (the carousel even rendered
	 * twice), and starving the single-threaded audio ring feed made SFX loop in
	 * every non-home menu. Just paint the scrim + the panel. */
	if (m->state == 2 && m->open >= 0 && m->overlay[m->open]) {
		/* While the panel is SLIDING in/out, the web slides the OPAQUE full-screen
		 * panel over the still-visible, undimmed menubar/home: the home shows only in
		 * the strip the panel has vacated (below its moving bottom edge on open, above
		 * on close), and the panel's own dark background covers the rest. Reproduce
		 * that by rendering the home behind, then an opaque black scrim TRANSLATED with
		 * the panel (centre y = 360 - open_y) so it covers exactly the panel's extent
		 * and reveals the home in the vacated strip. Once settled, the full-screen
		 * panel covers everything, so use the cheap fixed black fill (no home
		 * re-render). Only a handful of frames, so the audio-feed cost is negligible. */
		if (m->open_y != 0.0f || m->closing) {
			draw_wp(m, t, (int)m->scroll);
			if (!m->homemenu) snes_render_scene(t, &m->home);
			else              draw_chrome(m, t);
			draw_carousel(m, t);
			draw_filmstrip(m, t);
			if (m->mb_focus >= 0 && m->mb_focus < 5 && m->mb_btn[m->mb_focus])
				snes_render_node(t, &m->home, m->mb_btn[m->mb_focus]);
			draw_menubar_caption(m, t);
			draw_menubar_hints(m, t);
			g = game(m, m->focus);
			if (g)
				snes_draw_text(t, m->pk, m->f_title, 640, 148, 0.85f,
					       0xFF202020u, 1, snes_str(m->pk, g->name));
			snes_fill_quad(t, 640, 360 - m->open_y, SNES_VW, SNES_VH,
				       0.0f, 0.0f, 0.0f, 1.0f);
		} else {
			snes_fill_quad(t, 640, 360, SNES_VW, SNES_VH, 0.0f, 0.0f, 0.0f, 1.0f);
		}
		m->overlay[m->open]->tf[5] = m->open_y;
		snes_render_node(t, &m->home, m->overlay[m->open]);
		if (m->open == 3) { draw_copyright_list(m, t); draw_copyright_hints(m, t); }
		else if (m->open == 4) draw_manual_hints(m, t);
		else if (m->open >= 0 && m->open <= 2) draw_option_hints(m, t);
		if (m->open == 0) draw_frame_strip(m, t);
		if (m->open == 2 && m->card_act) {
			snes_rnode *el = child_named(m->pk, m->overlay[2], "elements"), *it;
			for (it = el ? el->child : 0; it; it = it->sib) {
				snes_rnode *btn = child_named(m->pk, it, "button");
				float w[6];
				int on = name_eq(m->pk, it, "language01");
				snes_spr_entry d = { m->card_act->img, (uint16_t)(on ? 113 : 99),
						     881, 12, 12, 6, 6 };
				if (!btn) continue;
				snes_node_world(btn, w);
				snes_blit_spr(t, m->pk, &d, 640.0f + w[2], 360.0f - w[5], 2.4f, 1.0f);
			}
		}
		return;
	}

	PERF_BEGIN();
	draw_wp(m, t, (int)m->scroll);
	PERF_END(0);
	/* Home + menubar + resume all use the cached home chrome (the full-scene
	 * re-render was a big per-frame cost; the menubar even re-rendered the cards
	 * that draw_carousel then overpaints). The menubar's live bits (focus
	 * highlight + scaled icon) are drawn by re-rendering just the focused button
	 * subtree on top of the chrome below. */
	if (!m->homemenu) snes_render_scene(t, &m->home);
	else              draw_chrome(m, t);
	PERF_END(1);
	draw_carousel(m, t);
	PERF_END(2);
	draw_filmstrip(m, t);
	PERF_END(3);
	if (m->state == 0) draw_hints(m, t);
	else if (m->state == 1) {
		if (m->mb_focus >= 0 && m->mb_focus < 5 && m->mb_btn[m->mb_focus]) {
			snes_render_node(t, &m->home, m->mb_btn[m->mb_focus]);
			/* square selection cursor around the focused item's 96x70 button
			 * (icons cells 96px apart, first at x448; the cell drops ~5px). */
			if (m->card_act)
				draw_menubar_cursor(m, t, 448.0f + 96.0f * (float)m->mb_focus,
						    64.0f, m->hl_s, m->card_act->img);
		}
		draw_menubar_caption(m, t);
		draw_menubar_hints(m, t);
	}

	/* the white title bar (caption_title, 348x22 @3x = 1044x66), raised in resume;
	 * drawn here (not in the chrome cache) so it tracks the game title. */
	if (m->card_act) {
		float ty = 178.0f - RESUME_TITLE_DY * resume_open_frac(m);
		snes_spr_entry bar = { m->card_act->img, 1, 325, 348, 22, 174, 11 };
		snes_blit_spr(t, m->pk, &bar, 640.0f, ty, 3.0f, 1.0f);
	}
	/* focused game name drawn into the authored title frame (SNES title font) */
	g = game(m, m->focus);
	if (g)
		snes_draw_text(t, m->pk, m->f_title, 640,
			       152.0f - RESUME_TITLE_DY * resume_open_frac(m), 1.0f,
			       0xFF4C4C4Cu, 1, snes_str(m->pk, g->name));

	/* sort-rule label, briefly shown after a Select press */
	if (m->sort_label_t > 0) {
		static const char *nm[4] = { "Sort: Title", "Sort: Publisher",
			"Sort: Players", "Sort: Release" };
		snes_fill_quad(t, 640, 210, 360, 40, 0.06f, 0.08f, 0.10f, 0.85f);
		snes_draw_text(t, m->pk, m->f_s, 640, 202, 1.0f, 0xFFE0E8F0u, 1,
			       nm[m->sort_rule & 3]);
	}


	/* (state 2 / submenu is handled by the early-return at the top of render) */
	/* the suspend-point (resume) menu slides up over the home; the web dims
	 * nothing behind it (the menubar + cards stay full brightness), so no scrim. */
	if (m->state == 3 && m->resume) {
		m->resume->tf[5] = m->open_y;   /* slide-up-from-bottom offset */
		snes_render_node(t, &m->home, m->resume);
	}
	PERF_END(4);
}
