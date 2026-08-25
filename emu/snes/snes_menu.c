#include "snes_menu.h"

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
	m->chrome_ready = 1;
}
static void draw_chrome(snes_menu *m, snes_target *t)
{
	int y, x;
	if (!m->chrome_ready) { snes_render_node(t, &m->home, m->homemenu); return; }
	for (y = 0; y < SNES_VH; y++) {
		const uint32_t *src = m->chrome + (unsigned)y * SNES_VW;
		uint32_t *dst = t->fb + (unsigned)(t->offy + y) * t->pitch + t->offx;
		for (x = 0; x < SNES_VW; x++)
			if (src[x] & 0xFF000000u) dst[x] = src[x];
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
#define CAR_CY     368.0f                   /* cardlist container world y=0 (+card box offset) */
#define CAR_SC     0.91f                    /* native 252x276 -> ~230px card */

static int ring_delta(int a, int b, int n)
{
	int d = (((b - a) % n) + n) % n;
	if (d > n / 2) d -= n;
	return d;
}

static void draw_card(snes_menu *m, snes_target *t, int gi, float cx, int foc)
{
	const snes_game_rec *g = game(m, gi);
	float cy = CAR_CY, sc = CAR_SC;
	const snes_spr_entry *frame = foc ? m->card_act : m->card_norm;
	const snes_img_entry *im = (g && g->thumb_img != 0xFFFF) ? &m->pk->img[g->thumb_img] : 0;
	if (cx < -280 || cx > SNES_VW + 280) return;
	if (frame) {
		/* the `card` sprite is the screen BACKGROUND (blue when active, dark when
		 * not); draw it first, then the boxart on top, stretched to the screen
		 * window (228x204) so only the coloured border shows around it. */
		float bw = m->screen_w * sc, bh = m->screen_h * sc;
		snes_blit_spr(t, m->pk, frame, cx, cy, (m->card_fw / (float)frame->sw) * sc, 1.0f);
		if (im) snes_blit_tex(t, m->pk, im, cx, cy - m->screen_oy * sc, bw, bh, 1.0f);
		/* player-count dots along the bottom-left of the card */
		if (m->card_dot && g) {
			int np = g->players > 4 ? 4 : (g->players < 1 ? 1 : g->players), d;
			for (d = 0; d < 4; d++) {
				float dx = cx + (-96 + d * 24) * sc, dy = cy + 108 * sc;
				float a = (d < np) ? 1.0f : 0.3f;
				snes_blit_spr(t, m->pk, m->card_dot, dx, dy,
					      (24.0f / (float)m->card_dot->sw) * sc * 0.7f, a);
			}
		}
	} else {                                 /* fallback: plain framed boxart */
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
	if (n <= 0) return;
	/* painter's order: draw non-focused first, focused last (on top) */
	for (j = 0; j < n; j++) {
		float wx, cx;
		if (j == m->focus) continue;
		wx = m->sel_world + CAR_HGAP * (float)ring_delta(m->focus, j, n) + m->cont_shift;
		cx = 640.0f + wx;
		if (cx < -280 || cx > SNES_VW + 280) continue;
		draw_card(m, t, j, cx, 0);
	}
	draw_card(m, t, m->focus, 640.0f + m->sel_world + m->cont_shift, 1);
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
	/* cursor pin (curor_thumbnail) above the selected icon (cursor_node -> y501) */
	ccx = TH_X0 + TH_SPACING * (float)m->focus;
	if (m->card_act) {
		snes_spr_entry pin = { m->card_act->img, 145, 881, 12, 8, 6, 8 };
		snes_blit_spr(t, m->pk, &pin, ccx, 505.0f, 1.6f, 1.0f);
	}
}

/* a horizontal-flow HUD hint row (ports sys_hud): n items of button-icon +
 * localized label, laid left-to-right and centred at cx. Icons are 12/32-px
 * atlas sprites scaled x3. */
typedef struct { int sx, sy, sw, sh; const char *key; } snes_hint;
static void draw_hint_row(snes_menu *m, snes_target *t, const snes_hint *H, int n, float cx)
{
	const float sc = 0.85f, gap = 6.0f, vgap = 26.0f, y = 592.0f;
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
	m->pl = m->pr = m->pu = m->pd = m->pa = m->pb = m->ps = 0;
	m->sndh = m->sndt = 0; m->wall = 0;

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
	m->homemenu = snes_scene_find(&m->home, "homemenu");
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
			m->mb_caption[b] = child_named(pk, m->mb_btn[b], "caption_down");
			if (m->mb_caption[b]) m->mb_caption[b]->enabled = 0;
			/* the cyan active highlight is authored-on for every button; the
			 * real menu shows it only on the focused icon (and only in the
			 * menubar state), so start them all hidden. */
			if (m->mb_active[b]) m->mb_active[b]->enabled = 0;
			m->mb_scale[b] = 1.0f;
		}
	}
	/* per-icon settings overlays (display, options, language, copyright, manual) */
	m->overlay[0] = snes_scene_find(&m->home, "option_display");
	m->overlay[1] = snes_scene_find(&m->home, "option_settings");
	m->overlay[2] = snes_scene_find(&m->home, "option_languages");
	m->overlay[3] = snes_scene_find(&m->home, "copyright");
	m->overlay[4] = snes_scene_find(&m->home, "manual");
	m->resume = snes_scene_find(&m->home, "resumemenu");
	/* option_display resting state (ports sys_option_display.setup): line 1
	 * (display modes) focused, frame-line focus box hidden, selected mode 4:3
	 * shows its blue selection box (cursor_area). */
	if (m->overlay[0]) {
		snes_rnode *ef = child_named(pk, m->overlay[0], "elements_frame");
		snes_rnode *fn = ef ? child_named(pk, ef, "focused_node") : 0;
		snes_rnode *el = child_named(pk, m->overlay[0], "elements");
		snes_rnode *sel = el ? child_named(pk, el, "item_4_3") : 0;
		snes_rnode *ca = sel ? child_named(pk, sel, "cursor_area") : 0;
		if (fn) fn->enabled = 0;               /* hide Frame-line blue bar */
		if (ca) ca->enabled = 1;               /* blue box on selected 4:3 */
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
	}
	m->state = 0; m->mb_focus = 0; m->open = -1;
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
static void car_navigate(snes_menu *m, int dir)
{
	int n = m->ngames;
	float old_sw = m->sel_world, cardShift, ns;
	if (n <= 0) return;
	m->focus = ((m->focus + dir) % n + n) % n;
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
	int eu = in->up && !m->pu, ed = in->down && !m->pd;
	int ea = in->a && !m->pa, eb = in->b && !m->pb;

	if (m->state == 0) {                       /* ---- home carousel ---- */
		if (el) car_navigate(m, -1);
		if (er) car_navigate(m, 1);
		if (eu) { m->state = 1; push_snd(m, m->sfx_up); }
		if (ed && m->resume) {                 /* Down -> suspend-point menu */
			m->resume->enabled = 1;
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
		if (el) { m->mb_focus = (m->mb_focus + 4) % 5; push_snd(m, m->sfx_move); }
		if (er) { m->mb_focus = (m->mb_focus + 1) % 5; push_snd(m, m->sfx_move); }
		if (ed || eb) { m->state = 0; push_snd(m, m->sfx_cancel); }
		if (ea && m->overlay[m->mb_focus]) {
			m->open = m->mb_focus;
			m->overlay[m->open]->enabled = 1;
			/* authored at the hidden (off-top) position; bring it on-screen to
			 * its shown position (centered). Lua animates this slide-in. */
			m->overlay[m->open]->tf[2] = 0;
			m->overlay[m->open]->tf[5] = 0;
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
		if (eb) {
			if (m->open >= 0 && m->overlay[m->open])
				m->overlay[m->open]->enabled = 0;
			m->open = -1; m->state = 1; push_snd(m, m->sfx_cancel);
		}
	} else {                                   /* ---- resume menu ---- */
		if (eb || eu) {
			if (m->resume) m->resume->enabled = 0;
			m->state = 0; push_snd(m, m->sfx_cancel);
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
			if (m->mb_active[b]) m->mb_active[b]->enabled = foc;
			if (m->mb_caption[b]) m->mb_caption[b]->enabled = foc;
			if (m->mb_btn[b]) {
				float tgt = foc ? 1.2f : 1.0f;
				m->mb_scale[b] += (tgt - m->mb_scale[b]) * ks;
				m->mb_btn[b]->tf[0] = m->mb_btn[b]->tf[4] = m->mb_scale[b];
			}
		}
	}

	/* carousel container scroll: animate cont_shift back to 0 at constant rate
	 * (HGAP/REPEAT px/s), matching the linear scroll tween */
	{
		float rate = (CAR_HGAP / CAR_REPEAT) * dt;
		if (m->cont_shift > 0) { m->cont_shift -= rate; if (m->cont_shift < 0) m->cont_shift = 0; }
		else if (m->cont_shift < 0) { m->cont_shift += rate; if (m->cont_shift > 0) m->cont_shift = 0; }
	}
	/* filmstrip smooth index follow */
	k = dt * 12.0f; if (k > 1.0f) k = 1.0f;
	m->car_x += ((float)m->focus - m->car_x) * k;

	/* wallpaper parallax scroll */
	m->scroll += dt * 60.0f * 1.2f;
}

void snes_menu_render(snes_menu *m, snes_target *t)
{
	const snes_game_rec *g;
	draw_wp(m, t, (int)m->scroll);
	/* authored home chrome. States without a live menubar highlight (home,
	 * resume) use the cached overlay; menubar/submenu render live. */
	if (!m->homemenu)                 snes_render_scene(t, &m->home);
	else if (m->state == 1 || m->state == 2)
		snes_render_node(t, &m->home, m->homemenu);
	else                              draw_chrome(m, t);
	draw_carousel(m, t);
	draw_filmstrip(m, t);
	if (m->state == 0) draw_hints(m, t);
	else if (m->state == 1) draw_menubar_hints(m, t);

	/* focused game name drawn into the authored title frame (SNES title font) */
	g = game(m, m->focus);
	if (g)
		snes_draw_text(t, m->pk, m->f_title, 640, 148, 0.85f, 0xFF202020u, 1,
			       snes_str(m->pk, g->name));

	/* sort-rule label, briefly shown after a Select press */
	if (m->sort_label_t > 0) {
		static const char *nm[4] = { "Sort: Title", "Sort: Publisher",
			"Sort: Players", "Sort: Release" };
		snes_fill_quad(t, 640, 210, 360, 40, 0.06f, 0.08f, 0.10f, 0.85f);
		snes_draw_text(t, m->pk, m->f_s, 640, 202, 1.0f, 0xFFE0E8F0u, 1,
			       nm[m->sort_rule & 3]);
	}

	/* caption name inside the focused menubar icon's bubble (authored label is a
	 * localization key we render blank, so draw the name ourselves) */
	if (m->state == 1 && m->mb_caption[m->mb_focus]) {
		static const char *cap[5] = { "Display", "Options", "Language",
			"Copyright", "Manual" };
		float w[6], cx, cy;
		snes_node_world(m->mb_caption[m->mb_focus], w);
		cx = SNES_VW / 2.0f + w[2];
		cy = SNES_VH / 2.0f - w[5];
		snes_draw_text(t, m->pk, m->f_s, cx, cy + 18, 0.9f, 0xFF202020u, 1,
			       cap[m->mb_focus]);
	}

	/* an opened settings overlay renders on top, over a FULLY black background
	 * (the web settings panels fully hide the home behind them) */
	if (m->state == 2 && m->open >= 0 && m->overlay[m->open]) {
		snes_fill_quad(t, 640, 360, SNES_VW, SNES_VH, 0.0f, 0.0f, 0.0f, 1.0f);
		snes_render_node(t, &m->home, m->overlay[m->open]);
		/* radio dots: draw radiobtn_off (empty) / radiobtn_on (selected) at each
		 * language item's authored dot position (the authored dot is disabled) */
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
	}
	/* the suspend-point (resume) menu, over a strong black scrim */
	if (m->state == 3 && m->resume) {
		snes_fill_quad(t, 640, 360, SNES_VW, SNES_VH, 0.0f, 0.0f, 0.0f, 0.82f);
		snes_render_node(t, &m->home, m->resume);
	}
}
