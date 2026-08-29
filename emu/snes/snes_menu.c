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
/* sub-phase timers (us) for the two slow OVL-buffer builds, so the device UART log can
 * split them: g_cc_us = build_cardcache {clear, draw, band-scan, unpremult};
 * g_cur_us = render_cursor_layer {clear, focus-card, cursor, unpremult}. */
unsigned g_cc_us[4], g_cur_us[4];
static unsigned g_sub_last;
#define SUB_BEGIN() do { if (g_perf_tick) g_sub_last = g_perf_tick(); } while (0)
#define SUB_END(arr, i) do { if (g_perf_tick) { unsigned n_ = g_perf_tick(); \
	(arr)[i] = (n_ - g_sub_last) / 13u; g_sub_last = n_; } } while (0)

/* ---- 4:3 aspect adaptation (ports static renderer.js setAspect + paint) ----
 * The panel is 1280x960 = 4:3; native design is 1280x720 (16:9). In 4:3 mode
 * (m->aspect) offy is forced to 0 and each view group maps virtual screen (X,Y)
 * to the 960 fb as (vsx*X+vdx, vsy*Y+vdy) via snes_target_view. At 16:9 every
 * group is the (1,1,0,0) no-op and offy=120 letterboxes as before. The centre of
 * the content (virtual y=360) stays at the panel centre 480; zoom is about it. */
enum { VIEW_CONTENT, VIEW_TOP, VIEW_BOTTOM, VIEW_WALL, VIEW_CONTAIN, VIEW_BANNERX };
#define ASP_CONTENT_S 1.18519f    /* (16/9)/(3/2): capped content zoom */
#define ASP_WALL_S    1.75824f    /* 480/273: wallpaper fill zoom */
static void set_view(snes_menu *m, snes_target *t, int group)
{
	if (!m->aspect) { snes_target_view(t, 1.0f, 1.0f, 0.0f, 0.0f); return; }
	switch (group) {
	case VIEW_TOP:
		snes_target_view(t, 1.0f, 1.0f, 0.0f, 0.0f); break;
	case VIEW_BOTTOM:
		snes_target_view(t, 1.0f, 1.0f, 0.0f, 240.0f); break;
	case VIEW_WALL:
		snes_target_view(t, ASP_WALL_S, ASP_WALL_S,
				 640.0f - ASP_WALL_S * 640.0f, 480.0f - ASP_WALL_S * 360.0f); break;
	case VIEW_CONTAIN:
		snes_target_view(t, 1.0f, 1.0f, 0.0f, 120.0f); break;
	case VIEW_BANNERX:
		snes_target_view(t, 1.0f, ASP_CONTENT_S, 0.0f, 480.0f - ASP_CONTENT_S * 360.0f); break;
	default: /* VIEW_CONTENT */
		snes_target_view(t, ASP_CONTENT_S, ASP_CONTENT_S,
				 640.0f - ASP_CONTENT_S * 640.0f, 480.0f - ASP_CONTENT_S * 360.0f); break;
	}
}

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
/* recursively enable/disable every sprite component matching an atlas cell (sx,sy) */
static void set_spr_comp(const snes_pack *pk, snes_scene *s, snes_rnode *n,
			 int sx, int sy, int on)
{
	snes_rnode *ch;
	unsigned i;
	if (!n) return;
	for (i = 0; i < n->def->comp_count; i++) {
		snes_comp *c = (snes_comp *)snes_node_comp(s, n->def, i);
		if (c->type == COMP_SPRITE) {
			const snes_spr_entry *sp = snes_res_spr(pk, ((const snes_comp_visual *)c)->res_hash);
			if (sp && sp->sx == sx && sp->sy == sy) {
				if (on) c->flags |= SNES_COMP_ENABLED;
				else    c->flags &= ~SNES_COMP_ENABLED;
			}
		}
	}
	for (ch = n->child; ch; ch = ch->sib) set_spr_comp(pk, s, ch, sx, sy, on);
}
/* set the pixel height of a sprite/texture comp matching atlas cell (sx,sy). */
static void set_spr_size_h(const snes_pack *pk, snes_scene *s, snes_rnode *n,
			   int sx, int sy, float h)
{
	snes_rnode *ch;
	unsigned i;
	if (!n) return;
	for (i = 0; i < n->def->comp_count; i++) {
		snes_comp *c = (snes_comp *)snes_node_comp(s, n->def, i);
		if (c->type == COMP_SPRITE && (c->flags & SNES_COMP_HAS_SIZE)) {
			const snes_spr_entry *sp = snes_res_spr(pk, ((const snes_comp_visual *)c)->res_hash);
			if (sp && sp->sx == sx && sp->sy == sy)
				((snes_comp_visual *)c)->size_h = h;
		}
	}
	for (ch = n->child; ch; ch = ch->sib) set_spr_size_h(pk, s, ch, sx, sy, h);
}
static void apply_display_state(snes_menu *m);   /* fwd: used by init + update */
static void frame_layout(snes_menu *m);          /* fwd: used by init + update */
static void apply_options_state(snes_menu *m);   /* fwd: used by init + update */
static void apply_language_state(snes_menu *m);  /* fwd: used by init + update */
static const char *lang_name(int idx);           /* fwd: used by render */
static void set_reset_gauge(snes_menu *m, float rate);   /* fwd: used by init */
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
/* 4:3: the neon wallpaper zooms by ASP_WALL_S to fill the whole 960 panel (no
 * black gaps). Inverse-map each of the 960 fb rows + 1280 cols back into the wp
 * cache (x map precomputed once/frame). vdx=640-s*640, vdy=480-s*360. */
/* Intersect a direct-composite (non-blit) row loop [*lo,*hi) with the target's
 * optional multicore scanline band. `abs0` is the absolute panel row of local
 * row 0 (t->offy for offy-relative loops, 0 for panel-absolute). No-op when no
 * band is set (band_y1==0, single-core), so these composites are unchanged on
 * the release path. Needed because these loops bypass blit()'s band clip; under
 * the AYANEO_BIGCORE_EXPT split each core must only touch its own rows or the
 * full-frame wallpaper/chrome would be redrawn in both passes and clobber the
 * other core's band. */
static void band_rows(const snes_target *t, int abs0, int *lo, int *hi)
{
	if (t->band_y1 > t->band_y0) {
		int b0 = t->band_y0 - abs0, b1 = t->band_y1 - abs0;
		if (*lo < b0) *lo = b0;
		if (*hi > b1) *hi = b1;
	}
}
static void draw_wp_43(snes_menu *m, snes_target *t, int scroll_px)
{
	static int xmap[SNES_VW];
	int Y, X, off = ((scroll_px % WP_CACHE_W) + WP_CACHE_W) % WP_CACHE_W;
	float inv = 1.0f / ASP_WALL_S;
	float vdx = 640.0f - ASP_WALL_S * 640.0f, vdy = 480.0f - ASP_WALL_S * 360.0f;
	int Ylo = 0, Yhi = (t->H < 960) ? t->H : 960;
	if (!m->wp_ready) return;
	for (X = 0; X < SNES_VW; X++) {
		int wx = (int)(((float)X - vdx) * inv) + off;
		wx %= WP_CACHE_W; if (wx < 0) wx += WP_CACHE_W;
		xmap[X] = wx;
	}
	band_rows(t, 0, &Ylo, &Yhi);   /* 4:3 wallpaper fills panel-absolute rows */
	for (Y = Ylo; Y < Yhi; Y++) {
		int wy = (int)(((float)Y - vdy) * inv);
		uint32_t *src, *dst = t->fb + (unsigned)Y * t->pitch + t->offx;
		if (wy < 0) wy = 0; if (wy >= WP_CACHE_H) wy = WP_CACHE_H - 1;
		src = m->wp + (unsigned)wy * WP_CACHE_W;
		for (X = 0; X < SNES_VW; X++) dst[X] = src[xmap[X]];
	}
}
static void draw_wp(snes_menu *m, snes_target *t, int scroll_px)
{
	int cy, off = ((scroll_px % WP_CACHE_W) + WP_CACHE_W) % WP_CACHE_W;
	int cylo = 0, cyhi = WP_CACHE_H;
	if (m->aspect) { draw_wp_43(m, t, scroll_px); return; }
	if (!m->wp_ready) return;
	band_rows(t, t->offy, &cylo, &cyhi);   /* rows written are offy+cy */
	for (cy = cylo; cy < cyhi; cy++) {
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
#define CHROME_H 960   /* cache is panel-tall so 4:3 (960) fits; 16:9 uses rows 0..SNES_VH-1 */
static uint16_t s_chrome_run[CHROME_H * CHROME_MAXRUN * 2];  /* (x0,x1) opaque runs/row */
static uint16_t s_chrome_nrun[CHROME_H];
static void build_chrome(snes_menu *m)
{
	snes_target ct = {0};   /* zero-init incl. the band clip */
	int H = m->aspect ? CHROME_H : SNES_VH;
	unsigned i, n = (unsigned)SNES_VW * (unsigned)H;
	if (!m->chrome || !m->homemenu) return;
	for (i = 0; i < n; i++) m->chrome[i] = CHROME_SENTINEL;
	ct.fb = m->chrome; ct.pitch = SNES_VW; ct.W = SNES_VW; ct.H = H;
	ct.offx = 0; ct.offy = 0;
	if (!m->aspect) {
		snes_target_view(&ct, 1.0f, 1.0f, 0.0f, 0.0f);
		snes_render_node(&ct, &m->home, m->homemenu);
	} else {
		/* 4:3: the two SNES bars pin to the new top/bottom edges (VIEW_TOP/BOTTOM)
		 * while everything else zooms about the panel centre (VIEW_CONTENT). Render
		 * the content pass with the bar subtrees disabled (so they leave no ghost at
		 * the content position), then over-render each bar with its own view. */
		snes_rnode *mbu  = m->menubar;                                 /* menubar_upper -> top */
		snes_rnode *mbar = child_named(m->pk, m->homemenu, "menubar"); /* bottom SNES bar */
		snes_rnode *hud  = child_named(m->pk, m->homemenu, "hud");     /* bottom hint bars */
		snes_rnode *gt   = child_named(m->pk, m->homemenu, "gametitle"); /* title bar -> bannerx (drawn live) */
		int e_mbu  = mbu  ? mbu->enabled  : 0;
		int e_mbar = mbar ? mbar->enabled : 0;
		int e_hud  = hud  ? hud->enabled  : 0;
		int e_gt   = gt   ? gt->enabled   : 0;
		if (mbu)  mbu->enabled  = 0;
		if (mbar) mbar->enabled = 0;
		if (hud)  hud->enabled  = 0;
		if (gt)   gt->enabled   = 0;   /* caption_title is 'bannerx' (native width), not content-zoomed */
		set_view(m, &ct, VIEW_CONTENT);
		snes_render_node(&ct, &m->home, m->homemenu);
		if (mbu)  { mbu->enabled  = e_mbu;  set_view(m, &ct, VIEW_TOP);    snes_render_node(&ct, &m->home, mbu); }
		if (mbar) { mbar->enabled = e_mbar; set_view(m, &ct, VIEW_BOTTOM); snes_render_node(&ct, &m->home, mbar); }
		if (hud)  { hud->enabled  = e_hud;  set_view(m, &ct, VIEW_BOTTOM); snes_render_node(&ct, &m->home, hud); }
		if (gt)   gt->enabled   = e_gt;   /* restore; the live BANNERX/CONTENT draw handles it */
	}
	for (i = 0; i < n; i++)
		m->chrome[i] = (m->chrome[i] == CHROME_SENTINEL) ? 0u : (0xFF000000u | m->chrome[i]);
	/* Precompute the opaque horizontal runs per row once, so draw_chrome (called
	 * every frame in the home/resume states) can memcpy the runs instead of doing
	 * a per-pixel alpha branch over 921600 pixels - a big per-frame CPU saving. */
	{
		int y, x, H = m->aspect ? CHROME_H : SNES_VH;
		for (y = 0; y < H; y++) {
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
	int y, i, H = m->aspect ? CHROME_H : SNES_VH;
	int ylo = 0, yhi = H;
	if (!m->chrome_ready) { snes_render_node(t, &m->home, m->homemenu); return; }
	band_rows(t, t->offy, &ylo, &yhi);   /* rows written are offy+y */
	for (y = ylo; y < yhi; y++) {
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
#define CLOSE_DUR  0.20f    /* submenu close slide duration (web moveTo 0.2s inExpo) */
/* inExpo(r) = 2^(10(r-1)) for the submenu/resume close slide (libc-free 2^y for
 * y=10(r-1) in [-10,0]: floor split + a minimax cubic for the [0,1) fraction). */
static float close_ease_inexpo(float r)
{
	float y, f, p;
	int n;
	if (r <= 0.0f) return 0.0f;
	if (r >= 1.0f) return 1.0f;
	y = 10.0f * (r - 1.0f);            /* in [-10, 0) */
	n = (int)y; if (y < (float)n) n--; /* floor (n <= -1) */
	f = y - (float)n;                  /* fraction in [0, 1) */
	p = 1.0f + f * (0.69315f + f * (0.24152f + f * 0.05177f));  /* 2^f */
	return p / (float)(1 << (-n));     /* 2^f * 2^n */
}
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
#define DISP_HOLD_DELAY  0.5f   /* Display mode/frame L/R auto-repeat delay (GUI.H) */
#define DISP_HOLD_RATE   0.2f   /* Display mode/frame L/R auto-repeat interval */
#define SUB_HOLD_DELAY   0.3f   /* Options/Language/Legal list-nav auto-repeat delay */
#define SUB_HOLD_RATE    0.1f   /* Options/Language/Legal list-nav auto-repeat interval */
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

/* outExpo(t) = 1 - 2^(-10t), libc-free (floor-split + minimax cubic for 2^frac). */
static float ease_outexpo(float t)
{
	float y, f, p;
	int n;
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	y = -10.0f * t;                    /* in [-10, 0) */
	n = (int)y; if (y < (float)n) n--;
	f = y - (float)n;
	p = 1.0f + f * (0.69315f + f * (0.24152f + f * 0.05177f));
	return 1.0f - p / (float)(1 << (-n));
}
#define CUR_SLIDE_DUR 0.5f   /* carousel<->menubar cursor slide (web moveTo 0.5s outExpo) */
/* The blue selection cursor slides + resizes between the focused card (246x270,
 * rounded) and the focused menubar item (96x70, square) on Up/Down. Returns 1
 * while sliding (the static card/menubar cursors are suppressed). Ports the
 * CursorManager square<->rounded handoff (moveTo 0.5s outExpo). */
static int draw_focus_slide(snes_menu *m, snes_target *t)
{
	static const uint16_t sq_x[10] = {0, 69,131, 69,137,156,171, 88,137,103};
	static const uint16_t sq_y[10] = {0,847,987,945,941,895,941,895,847,847};
	static const uint16_t rd_x[10] = {0,103,131,122,137,156,171,190,137,145};
	static const uint16_t rd_y[10] = {0,941,987,895,941,895,941,895,847,809};
	const uint16_t *sxa, *sya;
	float p, cardx, mbx, cvx, cvdx, cvdy;
	float cx, cy, cw, ch, mx, my, mw, mh, fx, fy, fw, fh, tx, ty, tw, th;
	if (m->cur_slide_t >= CUR_SLIDE_DUR || !m->card_act) return 0;
	if (m->state != 0 && m->state != 1) return 0;   /* only carousel<->menubar */
	p = ease_outexpo(m->cur_slide_t / CUR_SLIDE_DUR);
	/* the card endpoint is in the CONTENT view group and the menubar endpoint in
	 * TOP (identity); the slide crosses groups, so pre-apply each view transform to
	 * get screen (pre-offy) coords, interpolate, and draw in the identity view. */
	cardx = 640.0f + m->sel_world + m->cont_shift;
	mbx   = 448.0f + 96.0f * (float)m->mb_focus;
	cvx  = m->aspect ? ASP_CONTENT_S : 1.0f;
	cvdx = m->aspect ? 640.0f - ASP_CONTENT_S * 640.0f : 0.0f;
	cvdy = m->aspect ? 480.0f - ASP_CONTENT_S * 360.0f : 0.0f;
	cx = cvx * cardx + cvdx; cy = cvx * 360.0f + cvdy; cw = 246.0f * cvx; ch = 270.0f * cvx;
	mx = mbx; my = 64.0f; mw = 96.0f; mh = 70.0f;
	if (m->state == 1) {            /* up: card -> menubar (square cursor) */
		fx = cx; fy = cy; fw = cw; fh = ch;  tx = mx; ty = my; tw = mw; th = mh;
		sxa = sq_x; sya = sq_y;
	} else {                        /* down: menubar -> card (rounded cursor) */
		fx = mx; fy = my; fw = mw; fh = mh;  tx = cx; ty = cy; tw = cw; th = ch;
		sxa = rd_x; sya = rd_y;
	}
	snes_target_view(t, 1.0f, 1.0f, 0.0f, 0.0f);   /* identity: coords already in screen space */
	draw_cursor_9slice(m, t, fx + (tx - fx) * p, fy + (ty - fy) * p,
			   fw + (tw - fw) * p, fh + (th - fh) * p, sxa, sya, 1.0f, 1.0f,
			   m->card_act->img);
	return 1;
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
	cy -= RESUME_CARD_DY * m->resume_dim;   /* raised behind the Suspend List */
	const snes_img_entry *im = (g && g->thumb_img != 0xFFFF) ? &m->pk->img[g->thumb_img] : 0;
	/* widen the cull when building the pan cache (cache_layer) by one card-slide so
	 * the SETTLED strip is a superset of every card visible at any cont_shift during
	 * the slide - the OVL pan can then show cards that slide in from the edges. */
	{ int cm = t->cache_layer ? (280 + (int)CAR_HGAP) : 280;
	  if (cx < -cm || cx > SNES_VW + cm) return; }
	if (frame) {
		/* the `card` sprite is the screen BACKGROUND (blue when active, dark when
		 * not); draw it first, then the boxart on top. The boxart is scaled
		 * ASPECT-PRESERVING to fit the screen window, NOT stretched: web uses
		 * scale = min(screenX/w, screenX/h, 1) (sys_game_card.loadingDone), which
		 * for a 228x160 thumb is 1 -> native 228x160, centred in the window. The
		 * dark frame is the base; the blue frame cross-fades in on top (Tween over
		 * REPEAT/0.2s, sys_gametitlelist onElementFocus). */
		/* the blue (active) frame has the same silhouette as the dark one, so
		 * when it is fully opaque it completely covers the dark frame - skip the
		 * redundant dark-frame blit for the focused card. */
		/* Frame colour, split across OVL layers so a dead-zone walk never rebuilds L2.
		 * Ownership is by card, so every card's frame is composited exactly once:
		 *  - L2 body cache (cache_layer, !l3_focus): DARK frame only, for every NON-
		 *    focused card. The focused card is skipped on L2 (see draw_carousel).
		 *  - L3 focus overlay, FOCUSED card (l3_focus, gi==focus): owns its whole body
		 *    (not on L2), so draw it exactly like the reference - an opaque dark base
		 *    unless fully blue, then the blue active frame crossfading in on top. This
		 *    keeps the card's screen background opaque during the 0.2s fade-in.
		 *  - L3 focus overlay, OUTGOING card (l3_focus, gi!=focus): its dark base IS on
		 *    L2, so draw ONLY the fading-out blue frame over it (blue_a=prog>0.003 here).
		 *  - reference / single-buffer (!cache_layer): dark then blue crossfade. */
		if (t->cache_layer && !t->l3_focus) {
			snes_blit_spr_tint(t, m->pk, frame, cx, cy, (m->card_fw / (float)frame->sw) * sc, 1.0f, dim, dim, dim);
		} else if (t->cache_layer && t->l3_focus && gi != m->focus) {
			if (m->card_act && blue_a > 0.003f)
				snes_blit_spr_tint(t, m->pk, m->card_act, cx, cy,
					      (m->card_fw / (float)m->card_act->sw) * sc,
					      blue_a > 1.0f ? 1.0f : blue_a, dim, dim, dim);
		} else {
			if (!(blue_a >= 0.997f && m->card_act))
				snes_blit_spr_tint(t, m->pk, frame, cx, cy, (m->card_fw / (float)frame->sw) * sc, 1.0f, dim, dim, dim);
			if (blue_a > 0.003f && m->card_act)
				snes_blit_spr_tint(t, m->pk, m->card_act, cx, cy,
					      (m->card_fw / (float)m->card_act->sw) * sc,
					      blue_a > 1.0f ? 1.0f : blue_a, dim, dim, dim);
		}
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
		 * is the active focus (state 0 home) AND not mid-slide (the slide cursor
		 * draws it travelling to/from the menubar). Skipped when building the L2
		 * card cache (cache_layer): the pulsing cursor is composited live on the
		 * L3 OVL layer via draw_focus_cursor (see OVL_LAYERS.md). */
		if (blue_a > 0.003f && m->state == 0 && m->cur_slide_t >= CUR_SLIDE_DUR
		    && !t->cache_layer)
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
	/* L2 body cache (cache_layer, !l3_focus): the FOCUSED card is skipped here -
	 * it is composited by the L3 focus overlay (draw_focus_card) alone, so it is
	 * drawn exactly ONCE. Drawing it on both L2 (dark) and L3 (blue) would double-
	 * contribute at every non-opaque pixel (the semi-transparent card screen back-
	 * ground and the sprite's anti-aliased edges), making the composite brighter
	 * than the reference (OVL_LAYERS.md). The L3 overlay owns the focused card's
	 * full opaque body, so nothing is lost by skipping it on L2. */
	int skip_focus = (t->cache_layer && !t->l3_focus);
	if (n <= 0) return;
	/* painter's order: draw non-focused first, focused last (on top) */
	for (j = 0; j < n; j++) {
		float wx, cx, blue_a;
		if (j == m->focus) continue;
		wx = m->sel_world + CAR_HGAP * (float)ring_delta(m->focus, j, n) + m->cont_shift;
		cx = 640.0f + wx;
		{ int cm = t->cache_layer ? (280 + (int)CAR_HGAP) : 280;   /* wider for the pan cache */
		  if (cx < -cm || cx > SNES_VW + cm) continue; }
		blue_a = (j == m->prev_focus) ? prog : 0.0f;   /* outgoing card fades out */
		draw_card(m, t, j, cx, blue_a, ndim);
	}
	/* the selected card stays bright and keeps its blue active frame in resume
	 * (cardColorDark dims only the OTHER cards); just its normal crossfade. */
	if (!skip_focus)
		draw_card(m, t, m->focus, 640.0f + m->sel_world + m->cont_shift, 1.0f - prog, 1.0f);
}

/* ==== OVL hardware-layering support (state-0 idle home 60fps; OVL_LAYERS.md) ====
 * The cursorless card strip is rendered once into an OVL layer (L2) and composited
 * by the display hardware over the wallpaper/chrome framebuffer (L0), so the CPU
 * stops re-blitting ~7 large cards every frame. The colour-pulsing selection cursor
 * is drawn live into a small OVL layer (L3) on top. A signature of all card-strip
 * state drives L2 rebuilds. The layers are built in PREMULTIPLIED alpha (clean
 * source-over) then un-premultiplied to STRAIGHT (coverage) alpha, which is what
 * the MT6785 OVL blender expects (SURFL_EN=0). */

/* signature of every input that changes the cursorless card strip; unchanged
 * frame-to-frame => the strip is static and the L2 cache can be reused as-is. */
static uint32_t cc_signature(snes_menu *m)
{
	union { float f; uint32_t u; } c;
	uint32_t h = 2166136261u;
#define CC_MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
	/* SETTLED signature: what the cont_shift=0, xfade=0 cached strip looks like.
	 * Deliberately EXCLUDES cont_shift and xfade_t/prev_focus - those animate during a
	 * slide but are handled by the OVL dst_x pan + the crossfade snap, so they must NOT
	 * trigger a rebuild (that is what lets a scroll pan instead of re-render). */
	CC_MIX(m->focus); CC_MIX(m->state); CC_MIX(m->sort_rule);
	CC_MIX(m->ngames); CC_MIX(m->aspect);
	c.f = m->sel_world;  CC_MIX(c.u);
	c.f = m->resume_dim; CC_MIX(c.u);
	c.f = m->open_y;     CC_MIX(c.u);
#undef CC_MIX
	return h;
}
uint32_t snes_menu_cardcache_sig(snes_menu *m) { return cc_signature(m); }

/* ---- home-carousel dynamic-state pack/unpack (2-core split over an MMIO channel) ----
 * The per-frame render of the HOME carousel (state==0) depends on exactly these fields;
 * the rest of snes_menu is static after init. cpu0 packs them, publishes over MMIO, and
 * the worker unpacks them into its (static, frozen-snapshot) menu copy before rendering
 * its band. The X-macro keeps pack and unpack in perfect sync. Ints copied raw, floats
 * bit-reinterpreted. See SNES_STATE_NWORDS. */
#define SNES_STATE_FIELDS(Fi, Ff) \
	Fi(state) Fi(disp_cur) Fi(disp_sel) Fi(ngames) Fi(sort_rule) Fi(aspect) \
	Fi(car_navd) Fi(focus) Fi(prev_focus) \
	Ff(open_y) Ff(clock) Ff(cur_slide_t) Ff(scroll) Ff(scr_speed) Ff(scr_dir) \
	Ff(cur_scroll_time) Ff(cur_scroll_spd) Ff(car_x) Ff(sel_world) Ff(cont_shift) \
	Ff(xfade_t) Ff(resume_dim) Ff(screen_oy)

void snes_menu_pack_state(const snes_menu *m, uint32_t *b)
{
	unsigned k = 0;
	union { float f; uint32_t u; } c;
#define PI(name) b[k++] = (uint32_t)m->name;
#define PF(name) c.f = m->name; b[k++] = c.u;
	SNES_STATE_FIELDS(PI, PF)
#undef PI
#undef PF
}
void snes_menu_unpack_state(snes_menu *m, const uint32_t *b)
{
	unsigned k = 0;
	union { float f; uint32_t u; } c;
#define UI(name) m->name = (int)b[k++];
#define UF(name) c.u = b[k++]; m->name = c.f;
	SNES_STATE_FIELDS(UI, UF)
#undef UI
#undef UF
}

/* The FULL focused card (blue active frame + boxart + icon + dots), composited LIVE
 * on L3 over the L2 dark body. Rendering the whole card here (not just the frame)
 * keeps z-order correct (the boxart covers the frame's opaque screen area) and keeps
 * the focus colour off L2, so a dead-zone walk never rebuilds L2 and the 0.2s
 * crossfade stays smooth. Reuses draw_card with t->l3_focus so the blue frame is
 * drawn instead of the dark one. During the crossfade the outgoing card fades too. */
static void draw_focus_card(snes_menu *m, snes_target *t)
{
	int n = m->ngames, save = t->l3_focus;
	float prog, ndim;
	if (m->state != 0 || n <= 0 || !m->card_act) return;
	prog = (m->xfade_t > 0.0f && m->prev_focus != m->focus) ? m->xfade_t / CAR_XFADE : 0.0f;
	ndim = 1.0f - 0.5f * m->resume_dim;
	t->l3_focus = 1;
	if (prog > 0.003f)   /* outgoing card fading out */
		draw_card(m, t, m->prev_focus,
			  640.0f + m->sel_world + CAR_HGAP * (float)ring_delta(m->focus, m->prev_focus, n) + m->cont_shift,
			  prog, ndim);
	draw_card(m, t, m->focus, 640.0f + m->sel_world + m->cont_shift, 1.0f - prog, 1.0f);
	t->l3_focus = save;
}

/* The focused card's rounded selection cursor(s), drawn LIVE each frame (they
 * colour-pulse) into the L3 layer. Mirrors EXACTLY the cursor draws inside
 * draw_carousel/draw_card: the focused card's cursor plus, during the 0.2s
 * selection crossfade, the outgoing card's fading cursor. Only when the carousel
 * itself is focused (state 0) and no card<->menubar slide is running. */
static void draw_focus_cursor(snes_menu *m, snes_target *t)
{
	int n = m->ngames;
	float prog, ndim, cy;
	const snes_spr_entry *frame = m->card_norm ? m->card_norm : m->card_act;
	if (m->state != 0 || m->cur_slide_t < CUR_SLIDE_DUR || n <= 0 || !frame) return;
	prog = (m->xfade_t > 0.0f && m->prev_focus != m->focus) ? m->xfade_t / CAR_XFADE : 0.0f;
	ndim = 1.0f - 0.5f * m->resume_dim;
	cy = CAR_CY - RESUME_CARD_DY * m->resume_dim;
	/* outgoing card's fading cursor (mirrors draw_carousel's non-focused pass) */
	if (prog > 0.003f) {
		float cx = 640.0f + m->sel_world +
			   CAR_HGAP * (float)ring_delta(m->focus, m->prev_focus, n) + m->cont_shift;
		if (cx >= -280 && cx <= SNES_VW + 280)
			draw_card_cursor(m, t, cx, cy, prog > 1.0f ? 1.0f : prog, ndim, frame->img);
	}
	/* focused card's cursor (dim = 1.0, the focused card stays bright) */
	{
		float cx = 640.0f + m->sel_world + m->cont_shift, blue_a = 1.0f - prog;
		if (blue_a > 0.003f)
			draw_card_cursor(m, t, cx, cy, blue_a > 1.0f ? 1.0f : blue_a, 1.0f, frame->img);
	}
}

/* Un-premultiply a rect of a cache layer from premultiplied to STRAIGHT (coverage)
 * alpha, in place. Opaque (a=255) and empty (a=0) pixels are already straight. The
 * straight value is pr*255/a; the divide is by the PER-PIXEL alpha (variable divisor),
 * which is a hardware divide - ~30-40 cycles on the in-order A55 and the dominant cost
 * of the OVL builds (13.5ms in build_cardcache). Replace it with a 256-entry reciprocal
 * table: sr = (pr * recip[a]) >> 16 where recip[a] = round(255*65536/a). Matches the
 * exact divide within +/-1 LSB (edge rounding the layer-verify already tolerates). */
static unsigned g_unp_recip[256];   /* recip[a] = round(255*65536/a); pr*recip[a]>>16 = pr*255/a */
static int g_unp_ready;
static void cache_unpremult(snes_target *t, int x0, int y0, int x1, int y1)
{
	int y, x;
	if (!g_unp_ready) {   /* lazily build the reciprocal table once */
		unsigned a;
		for (a = 1; a < 256; a++) g_unp_recip[a] = (255u * 65536u + a / 2) / a;
		g_unp_ready = 1;
	}
	if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
	if (x1 > t->W) x1 = t->W; if (y1 > t->H) y1 = t->H;
	for (y = y0; y < y1; y++) {
		uint32_t *r = t->fb + (unsigned)y * t->pitch;
		for (x = x0; x < x1; x++) {
			uint32_t c = r[x];
			unsigned a = c >> 24, pr, pg, pb, sr, sg, sb, rc;
			if (!a || a == 255) continue;
			rc = g_unp_recip[a];
			pr = (c >> 16) & 0xff; pg = (c >> 8) & 0xff; pb = c & 0xff;
			sr = (pr * rc + 32768u) >> 16; sg = (pg * rc + 32768u) >> 16; sb = (pb * rc + 32768u) >> 16;
			if (sr > 255) sr = 255; if (sg > 255) sg = 255; if (sb > 255) sb = 255;
			r[x] = (a << 24) | (sr << 16) | (sg << 8) | sb;
		}
	}
}

/* Build the cursorless card strip into the caller's panel-sized L2 buffer (t->fb,
 * t->W x t->H, pitch, offx already set to the FB's; offy the FB's letterbox). The
 * cards are rendered position-identically to the live framebuffer (same VIEW_CONTENT
 * transform + aspect offy handling), then un-premultiplied to straight alpha. The
 * non-empty row band [cc_y0,cc_y1] is recorded so the OVL src ROI can be limited. */
void snes_menu_build_cardcache(snes_menu *m, snes_target *t)
{
	int H = t->H, W = t->W, y, x, y0 = t->H, y1 = -1;
	unsigned i, npix = (unsigned)W * (unsigned)H;
	float save_cs = m->cont_shift, save_xf = m->xfade_t;
	t->cache_layer = 1;
	/* The caller controls the target geometry (offx/offy/W/H/pitch). For the OVL pan
	 * this is a WIDE, band-height buffer with offx=margin so cards that slide in from
	 * the edges are pre-rendered (not clipped) and the layer is panned via src_x; the
	 * caller sets offy to place the card band, so we do NOT override it here. */
	SUB_BEGIN();
	for (i = 0; i < npix; i++) t->fb[i] = 0;
	SUB_END(g_cc_us, 0);
	/* Render the SETTLED strip: cont_shift=0 (the OVL pans the live cont_shift via
	 * dst_x, so the cache is position-independent and only rebuilds when the focus/
	 * order changes, not every frame during a slide) and xfade_t=0 (the 0.2s blue
	 * crossfade is snapped, so a held scroll does not force a rebuild every frame). */
	m->cont_shift = 0.0f; m->xfade_t = 0.0f;
	set_view(m, t, VIEW_CONTENT);
	draw_carousel(m, t);
	m->cont_shift = save_cs; m->xfade_t = save_xf;
	SUB_END(g_cc_us, 1);
	for (y = 0; y < H; y++) {
		const uint32_t *r = t->fb + (unsigned)y * t->pitch;
		for (x = 0; x < W; x++)
			if (r[x] >> 24) { if (y < y0) y0 = y; y1 = y; break; }
	}
	m->cc_y0 = y0; m->cc_y1 = y1;
	SUB_END(g_cc_us, 2);
	if (y1 >= y0) cache_unpremult(t, 0, y0, W, y1 + 1);
	SUB_END(g_cc_us, 3);
}

/* ===== Producer-offload: pre-rendered normal-card tiles (PRODUCER_OFFLOAD.md) =====
 * The dynamic 2-core coherency wall is a HW limit (cpu0->worker stores invisible), but the
 * worker CAN read static pre-bringup data and its writes ARE seen by cpu0. So the worker
 * pre-renders each NORMAL (non-focused) game card into a tile ONCE at boot from the static
 * pack, and cpu0's build_cardcache blits the tiles instead of re-rendering each card (the
 * expensive boxart min-filter scale + several blits) on every nav/settle rebuild.
 * Phase-exact ONLY in NATIVE view (vsx=1) where the settled strip places cards at integer
 * positions; the caller gates on !m->aspect (4:3 bakes a fractional scale). Tile is
 * STRAIGHT-alpha; blit onto the cache_layer strip premultiplies on the fly (source-over). */
void snes_menu_render_card_tile(snes_menu *m, int gi, uint32_t *tile)
{
	snes_target tt;
	unsigned i, npix = (unsigned)CARD_TILE_W * (unsigned)CARD_TILE_H;
	int k; char *z = (char *)&tt;
	float S = m->aspect ? ASP_CONTENT_S : 1.0f;   /* bake the current view scale into the tile */
	for (k = 0; k < (int)sizeof(tt); k++) z[k] = 0;
	tt.fb = tile; tt.pitch = CARD_TILE_W; tt.W = CARD_TILE_W; tt.H = CARD_TILE_H;
	tt.offx = 0; tt.offy = 0;
	/* place the card box centre (virtual cx=0, cy=CAR_CY) at the tile centre, at scale S:
	 * pixel = vs*V + vd, so vdx = TILE_W/2, vdy = TILE_H/2 - S*CAR_CY. */
	tt.vsx = tt.vsy = S;
	tt.vdx = (float)(CARD_TILE_W / 2);
	tt.vdy = (float)(CARD_TILE_H / 2) - S * CAR_CY;
	tt.cache_layer = 1;
	for (i = 0; i < npix; i++) tile[i] = 0;
	/* non-focused L2 card body: dark frame + scaled boxart + player icon + resume dots. */
	draw_card(m, &tt, gi, 0.0f, 0.0f, 1.0f);
	cache_unpremult(&tt, 0, 0, CARD_TILE_W, CARD_TILE_H);   /* premult -> straight for re-blit */
	/* draw_card wrote the tile in FRAMEBUFFER byte-order (0xAARRGGBB); snes_blit_raw reads it
	 * back through the texture path which expects RGBA byte-order, so swap R<->B once now. */
	for (i = 0; i < npix; i++) {
		uint32_t p = tile[i];
		tile[i] = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
	}
}

/* like draw_carousel's cache-layer pass but BLITS the pre-rendered (already view-scaled) tile
 * for each non-focused card at the card's VIEW-TRANSFORMED centre, into a vsx=1 target (the
 * tile carries the scale). Focused card is skipped (composited on L3 as usual). */
static void draw_carousel_tiled(snes_menu *m, snes_target *t, const uint32_t *tiles)
{
	int n = m->ngames, j;
	float S = m->aspect ? ASP_CONTENT_S : 1.0f;
	/* VIEW_CONTENT offsets - IDENTITY in native (set_view returns 1,1,0,0 when !aspect) */
	float vdx = m->aspect ? (640.0f - S * 640.0f) : 0.0f;
	float vdy = m->aspect ? (480.0f - S * 360.0f) : 0.0f;
	float cys = S * CAR_CY + vdy;
	if (n <= 0) return;
	for (j = 0; j < n; j++) {
		float wx, cx;
		if (j == m->focus) continue;
		wx = m->sel_world + CAR_HGAP * (float)ring_delta(m->focus, j, n) + m->cont_shift;
		cx = 640.0f + wx;
		{ int cm = t->cache_layer ? (280 + (int)CAR_HGAP) : 280;
		  if (cx < -cm || cx > SNES_VW + cm) continue; }
		snes_blit_raw(t, tiles + (unsigned)j * (unsigned)(CARD_TILE_W * CARD_TILE_H),
			      CARD_TILE_W, CARD_TILE_H, S * cx + vdx, cys);
	}
}

/* tiled equivalent of snes_menu_build_cardcache for the non-resume settled strip (caller
 * ensures m->resume_dim==0). EXACT in native; in 4:3 the tile is re-sampled at a fractional
 * position (host-measured near-exact). The target view is forced to identity: the tiles
 * already carry the aspect scale, and positions are pre-transformed by draw_carousel_tiled. */
void snes_menu_build_cardcache_tiled(snes_menu *m, snes_target *t, const uint32_t *tiles)
{
	int H = t->H, W = t->W, y, x, y0 = t->H, y1 = -1;
	unsigned i, npix = (unsigned)W * (unsigned)H;
	float save_cs = m->cont_shift, save_xf = m->xfade_t;
	t->cache_layer = 1;
	for (i = 0; i < npix; i++) t->fb[i] = 0;
	m->cont_shift = 0.0f; m->xfade_t = 0.0f;
	snes_target_view(t, 1.0f, 1.0f, 0.0f, 0.0f);   /* tiles carry the scale; positions pre-baked */
	draw_carousel_tiled(m, t, tiles);
	m->cont_shift = save_cs; m->xfade_t = save_xf;
	for (y = 0; y < H; y++) {
		const uint32_t *r = t->fb + (unsigned)y * t->pitch;
		for (x = 0; x < W; x++)
			if (r[x] >> 24) { if (y < y0) y0 = y; y1 = y; break; }
	}
	m->cc_y0 = y0; m->cc_y1 = y1;
	if (y1 >= y0) cache_unpremult(t, 0, y0, W, y1 + 1);
}

/* Band-limited variant for the 2-core split: build ONLY buffer rows [r0, r1) of the
 * strip - clear, draw (clipped to the scanline band so draw_carousel writes only those
 * rows), non-empty scan, and un-premultiply, all limited to [r0, r1). Two disjoint calls
 * (e.g. [0, H/2) on cpu0 and [H/2, H) on cpu1) produce a strip pixel-identical to
 * snes_menu_build_cardcache, halving the ~30ms build. Each call records its band's
 * non-empty sub-range into cc_y0/cc_y1; the caller unions the two for the OVL src ROI.
 * Leaves snes_menu_build_cardcache untouched so the single-core menu is unaffected.
 * Host-validated exact (host_render.c "vsplit" mode). */
void snes_menu_build_cardcache_band(snes_menu *m, snes_target *t, int r0, int r1)
{
	int W = t->W, y, x, y0, y1;
	int save_b0 = t->band_y0, save_b1 = t->band_y1;
	float save_cs = m->cont_shift, save_xf = m->xfade_t;
	if (r0 < 0) r0 = 0;
	if (r1 > t->H) r1 = t->H;
	y0 = r1; y1 = r0 - 1;
	t->cache_layer = 1;
	/* clear only this band's rows */
	for (y = r0; y < r1; y++) {
		uint32_t *r = t->fb + (unsigned)y * t->pitch;
		for (x = 0; x < W; x++) r[x] = 0;
	}
	/* scanline band clip: band_y0/band_y1 are absolute fb rows (here == buffer rows),
	 * so draw_carousel writes only rows [r0, r1). set_view does not touch the band. */
	t->band_y0 = r0; t->band_y1 = r1;
	m->cont_shift = 0.0f; m->xfade_t = 0.0f;
	set_view(m, t, VIEW_CONTENT);
	draw_carousel(m, t);
	m->cont_shift = save_cs; m->xfade_t = save_xf;
	t->band_y0 = save_b0; t->band_y1 = save_b1;
	/* non-empty scan within this band */
	for (y = r0; y < r1; y++) {
		const uint32_t *r = t->fb + (unsigned)y * t->pitch;
		for (x = 0; x < W; x++)
			if (r[x] >> 24) { if (y < y0) y0 = y; y1 = y; break; }
	}
	m->cc_y0 = y0; m->cc_y1 = y1;
	if (y1 >= y0) cache_unpremult(t, 0, y0, W, y1 + 1);
}

/* Signature of everything that changes the SETTLED focused-card body (frame+boxart+
 * icons at cont_shift=0). EXCLUDES cont_shift (the pan, handled by the blit shift) and
 * m->clock (only the cursor pulses, drawn live). When unchanged the cached body is reused. */
static uint32_t fcc_signature(snes_menu *m)
{
	union { float f; uint32_t u; } c;
	uint32_t h = 2166136261u;
#define FCC_MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
	FCC_MIX(m->focus); FCC_MIX(m->state); FCC_MIX(m->ngames); FCC_MIX(m->aspect);
	FCC_MIX(m->sort_rule);
	c.f = m->sel_world;  FCC_MIX(c.u);
	c.f = m->resume_dim; FCC_MIX(c.u);
#undef FCC_MIX
	return h ? h : 1u;
}

/* Clear + render the live selection cursor into the caller's panel-sized L3 buffer.
 * A fixed rect covering every cursor position (focused-card cursor + the card<->
 * menubar slide path, both aspects) is cleared each frame - so the double-buffered
 * L3 never trails - then the slide cursor (identity view) OR the static focus
 * cursor (content view) is drawn and un-premultiplied to straight alpha. */
void snes_menu_render_cursor_layer(snes_menu *m, snes_target *t, int full_clear)
{
	/* L3 lives in a DRAM-bandwidth-bound region, so touching the whole 1280x680 cursor
	 * rect every frame (clear + un-premult scan) costs ~13ms on device and blows the
	 * frame budget. Instead touch only the focused (and, mid-crossfade, outgoing) card's
	 * bounding box. Per-L3-buffer previous bbox (pv*) lets the clear also erase where the
	 * card was up to 2 frames ago in THIS double-buffer, so the fading outgoing card
	 * leaves no trail. flip pairs each call with one physical buffer (the driver flips
	 * its L3 buffer once per call, in lockstep). */
	static int pv0x[2], pv0y[2], pv1x[2] = {-1, -1}, pv1y[2] = {-1, -1};
	static int flip;
	int buf = flip & 1, y, x;
	int bx0, by0, bx1, by1;   /* this frame's draw bbox (panel coords) */
	int cx0, cy0, cx1, cy1;   /* clear + un-premult bbox = union(prev[buf], current) */
	flip ^= 1;

	t->cache_layer = 1;
	if (m->aspect) t->offy = 0;

	/* Re-entering the layered state: both L3 buffers hold stale content from before the
	 * excursion, so force a full clear of both by seeding their previous bbox to full. */
	if (full_clear) {
		pv0x[0] = pv0x[1] = SNES_CURSOR_X0; pv0y[0] = pv0y[1] = SNES_CURSOR_Y0;
		pv1x[0] = pv1x[1] = SNES_CURSOR_X1; pv1y[0] = pv1y[1] = SNES_CURSOR_Y1;
	}

	/* This frame's draw bbox. During the menubar<->carousel slide the cursor travels the
	 * full height, so fall back to the whole rect; otherwise it is just the card band. */
	if (m->cur_slide_t < CUR_SLIDE_DUR || m->state != 0 || m->ngames <= 0 || !m->card_act) {
		bx0 = SNES_CURSOR_X0; by0 = SNES_CURSOR_Y0;
		bx1 = SNES_CURSOR_X1; by1 = SNES_CURSOR_Y1;
	} else {
		float vsx = m->aspect ? ASP_CONTENT_S : 1.0f;
		float vdx = m->aspect ? (640.0f - ASP_CONTENT_S * 640.0f) : 0.0f;
		float vdy = m->aspect ? (480.0f - ASP_CONTENT_S * 360.0f) : 0.0f;
		float cyv = CAR_CY - RESUME_CARD_DY * m->resume_dim;
		float hw = m->card_fw * 0.5f * CAR_SC + 40.0f;   /* + cursor/icon/dot slop */
		float hh = m->card_fh * 0.5f * CAR_SC + 40.0f;
		float prog = (m->xfade_t > 0.0f && m->prev_focus != m->focus) ? m->xfade_t / CAR_XFADE : 0.0f;
		float cxc = 640.0f + m->sel_world + m->cont_shift;   /* focused card centre (virtual) */
		int px0 = t->offx + (int)(vsx * (cxc - hw) + vdx);
		int px1 = t->offx + (int)(vsx * (cxc + hw) + vdx) + 1;
		bx0 = px0; bx1 = px1;
		if (prog > 0.003f) {   /* outgoing card, one slot away */
			float cxo = cxc + CAR_HGAP * (float)ring_delta(m->focus, m->prev_focus, m->ngames);
			px0 = t->offx + (int)(vsx * (cxo - hw) + vdx);
			px1 = t->offx + (int)(vsx * (cxo + hw) + vdx) + 1;
			if (px0 < bx0) bx0 = px0; if (px1 > bx1) bx1 = px1;
		}
		by0 = t->offy + (int)(vsx * (cyv - hh) + vdy);
		by1 = t->offy + (int)(vsx * (cyv + hh) + vdy) + 1;
		if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
		if (bx1 > t->W) bx1 = t->W; if (by1 > t->H) by1 = t->H;
		if (bx1 < bx0) bx1 = bx0; if (by1 < by0) by1 = by0;
	}

	/* clear region = union(this buffer's previous draw bbox, this frame's bbox) */
	cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1;
	if (pv1x[buf] >= pv0x[buf]) {
		if (pv0x[buf] < cx0) cx0 = pv0x[buf]; if (pv0y[buf] < cy0) cy0 = pv0y[buf];
		if (pv1x[buf] > cx1) cx1 = pv1x[buf]; if (pv1y[buf] > cy1) cy1 = pv1y[buf];
	}
	if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
	if (cx1 > t->W) cx1 = t->W; if (cy1 > t->H) cy1 = t->H;
	SUB_BEGIN();
	for (y = cy0; y < cy1; y++) {
		uint32_t *r = t->fb + (unsigned)y * t->pitch;
		for (x = cx0; x < cx1; x++) r[x] = 0;
	}
	SUB_END(g_cur_us, 0);

	/* Focused card body (blue frame + boxart + icons) over the L2 dark body. Re-rendering
	 * it every frame is ~4ms on the A55; instead render the SETTLED body once into the fcc
	 * and blit it into L3 shifted by the integer pan. Skipped mid-crossfade (the blue frame
	 * animates) and during the menubar slide (full-rect path), where it renders live. */
	{
		float prog = (m->xfade_t > 0.0f && m->prev_focus != m->focus) ? m->xfade_t / CAR_XFADE : 0.0f;
		int cacheable = (m->fcc && prog <= 0.003f && m->state == 0 && m->ngames > 0
				 && m->card_act && m->cur_slide_t >= CUR_SLIDE_DUR);
#ifdef AYANEO_FASTPAN_L3
		/* FAST-PAN fps cut: while actively scrolling (cont_shift well off centre) the crossfade
		 * would force a ~4.4ms live focused-card body render EVERY frame (fcc not usable mid-
		 * xfade) -> frame overruns a vsync -> the tear-free double-barrier halves it to 15fps.
		 * The moving cards already read clearly from the L2 dark strip, so SKIP the L3 focused
		 * body during a fast pan (draw only the cursor below); the blue highlight settles back
		 * in the instant the scroll stops. Render-cost only - no tearing risk. */
		if ((m->cont_shift > 40.0f || m->cont_shift < -40.0f) && m->state == 0
		    && m->cur_slide_t >= CUR_SLIDE_DUR) {
			m->fcc_ready = 0;   /* body was not drawn; force a rebuild when it settles */
		} else
#endif
		if (cacheable) {
			float vsx = m->aspect ? ASP_CONTENT_S : 1.0f;
			int shift_px = (int)(m->cont_shift * vsx + (m->cont_shift >= 0.0f ? 0.5f : -0.5f));
			uint32_t sig = fcc_signature(m);
			if (!m->fcc_ready || m->fcc_sig != sig) {
				/* rebuild: render the SETTLED (cont_shift=0) body into the fcc at its
				 * canonical bbox (the live bbox shifted back by the pan, + slop so the
				 * per-frame +/-1px rounding of the blit source never reads outside it). */
				snes_target ct = *t;
				float save = m->cont_shift, save_xf = m->xfade_t;
				ct.fb = m->fcc;
				m->cont_shift = 0.0f; m->xfade_t = 0.0f;   /* fully settled body */
				m->fcc_x0 = bx0 - shift_px - 4; m->fcc_x1 = bx1 - shift_px + 4;
				m->fcc_y0 = by0; m->fcc_y1 = by1;
				if (m->fcc_x0 < 0) m->fcc_x0 = 0; if (m->fcc_x1 > t->W) m->fcc_x1 = t->W;
				for (y = m->fcc_y0; y < m->fcc_y1; y++) {
					uint32_t *r = m->fcc + (unsigned)y * t->pitch;
					for (x = m->fcc_x0; x < m->fcc_x1; x++) r[x] = 0;
				}
				set_view(m, &ct, VIEW_CONTENT);
				draw_focus_card(m, &ct);
				m->cont_shift = save; m->xfade_t = save_xf;
				m->fcc_sig = sig; m->fcc_ready = 1;
			}
			/* blit fcc[canonical] -> L3[live], shifted by the integer pan (exact) */
			for (y = by0; y < by1; y++) {
				uint32_t *d = t->fb + (unsigned)y * t->pitch;
				const uint32_t *s = m->fcc + (unsigned)y * t->pitch;
				for (x = bx0; x < bx1; x++) {
					int sx = x - shift_px;
					d[x] = (sx >= m->fcc_x0 && sx < m->fcc_x1) ? s[sx] : 0u;
				}
			}
		} else {
			set_view(m, t, VIEW_CONTENT);
			draw_focus_card(m, t);
			m->fcc_ready = 0;   /* crossfade/slide changed the body; rebuild next time */
		}
	}
	SUB_END(g_cur_us, 1);
	if (!draw_focus_slide(m, t)) {
		set_view(m, t, VIEW_CONTENT);
		draw_focus_cursor(m, t);
	}
	SUB_END(g_cur_us, 2);
	cache_unpremult(t, cx0, cy0, cx1, cy1);
	SUB_END(g_cur_us, 3);

	pv0x[buf] = bx0; pv0y[buf] = by0; pv1x[buf] = bx1; pv1y[buf] = by1;
}

/* ---- bottom thumbnail filmstrip (ports sys_thumbnail_icon: a fixed 21-icon
 * strip, NOT scrolling; icon i at world x=-410+41*i (screen 230+41*i), 32x32
 * bottom-anchored at world y=-191 (screen bottom 551); cursor over selected). */
#define TH_X0      230.0f    /* screen x of icon 0 (640-410) */
#define TH_SPACING 41.0f
#define TH_BOTTOM  551.0f    /* icon bottom edge */
#define TH_SIZE    32.0f
#define TH_CENTER_Y 551.0f   /* icon centre (web world Y=-191, centre anchor) */
static void draw_filmstrip(snes_menu *m, snes_target *t)
{
	int n = m->ngames, i;
	/* the web draws each icon at its NATIVE small.png size (40x28) with a CENTER
	 * anchor, its centre at world Y=-191 -> screen 551 (NOT a forced 32x32 square
	 * bottom-anchored at 535). */
	float ccx, ccy = TH_CENTER_Y;
	if (n <= 0) return;
	for (i = 0; i < n; i++) {
		const snes_game_rec *g = game(m, i);
		const snes_img_entry *im = (g && g->small_img != 0xFFFF) ? &m->pk->img[g->small_img] : 0;
		float cx = TH_X0 + TH_SPACING * (float)i;
		if (im) snes_blit_tex(t, m->pk, im, cx, ccy, (float)im->w, (float)im->h, 1.0f);
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
		/* positive modulo: m->clock can be transiently negative during a
		 * transition, and C's % keeps the sign, so a bare %13 would index
		 * seq[] with a negative subscript (out-of-bounds read). */
		int ti = (int)(m->clock / 0.03333f);
		int fr = seq[((ti % 13) + 13) % 13];
		snes_spr_entry cur = { m->card_act->img, sx[fr], 881, 12, 8, 6, 4 };
		/* sit in the gap between the focused card's bottom (~y498) and the
		 * filmstrip top (~y537): centred ~517 clears the card yet stays above
		 * the strip (the web renders it touching the card - user wants a gap). */
		snes_blit_spr_tint_flip(t, m->pk, &cur, ccx, 517.0f, 3.0f, 1.0f,
					63.0f / 255.0f, 191.0f / 255.0f, 1.0f, 0, 1);
	}
}

/* a horizontal-flow HUD hint row (ports sys_hud): n items of button-icon +
 * localized label, laid left-to-right and centred at cx. Icons are 12/32-px
 * atlas sprites scaled x3. */
typedef struct { int sx, sy, sw, sh; const char *key; } snes_hint;
static void draw_hint_row(snes_menu *m, snes_target *t, const snes_hint *H, int n, float cx, float sc)
{
	const float gap = 6.0f, vgap = 26.0f, y = 604.0f;
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
/* The IP-Notice body as a flat one-line-per-entry list (ports _legalIPTexts): the
 * deduped game copyrights ordered by (sort_publisher, release, sort_title), then a
 * fixed trailing block (Nintendo/trademark/Fontworks notices, EN + FR). Fills out[]
 * (up to max) and returns the line count. Each entry renders on one line, matching
 * the web (no word-wrap), so page-scroll is line-based. */
#define LEGAL_NVIS 18          /* label pool / page N (web _legalWidget N=18, LH=24) */
static const char *g_legal_tail[] = {
	"", "\xC2\xA9""2017 Nintendo", "",
	"Trademarks are property of their respective owners. Nintendo Entertainment System and SUPER NES",
	" are trademarks of Nintendo.",
	"Les marques appartiennent \xC3\xA0 leurs propri\xC3\xA9taires respectifs. Nintendo Entertainment System et SUPER",
	" NES sont des marques de Nintendo. ", "",
	"This product uses certain fonts provided by Fontworks Inc.",
	"Ce produit utilise certaines polices de caract\xC3\xA8res fournies par Fontworks Inc.", "",
	"The software included in this system represents game content and characters from the original games.",
	" The porting of games from their original system may result in minor changes to the game display.",
	"Les logiciels inclus dans cette console reproduisent le contenu des jeux originaux. Le portage de ces",
	" jeux depuis leur console d'origine peut entra\xC3\xAEner des changements d'affichage mineurs.",
};
static int legal_ip_lines(snes_menu *m, const char **out, int max)
{
	const snes_pack *pk = m->pk;
	uint32_t seen[128];
	unsigned short ord[128];
	int n = m->ngames, i, j, ns = 0, nl = 0;
	if (n <= 0) return 0;
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
	for (i = 0; i < n && nl < max; i++) {
		const snes_game_rec *g = game_raw(pk, ord[i]);
		int dup = 0;
		if (!g->copyright) continue;
		for (j = 0; j < ns; j++) if (seen[j] == g->copyright) { dup = 1; break; }
		if (dup) continue;
		seen[ns++] = g->copyright;
		out[nl++] = snes_str(pk, g->copyright);
	}
	for (i = 0; i < (int)(sizeof(g_legal_tail) / sizeof(g_legal_tail[0])) && nl < max; i++)
		out[nl++] = g_legal_tail[i];
	return nl;
}
/* total scrollable lines in the active Legal tab. */
static int legal_line_count(snes_menu *m)
{
	const char *lines[192];
	if (m->legal_tab == 1) return (int)m->pk->hdr->oss_count;   /* OSS licence text */
	return legal_ip_lines(m, lines, 192);
}
/* Update the active tab's scrollbar: light the up/down arrows when more remains
 * that way, and size + place the thumb per the scroll fraction. Ports the
 * sys_copyright_text scrollbar bits of _legalRender (sbMaxH 336, sbYOff 24). */
static void legal_scrollbar(snes_menu *m)
{
	const snes_pack *pk = m->pk;
	snes_rnode *body = m->overlay[3] ? child_named(pk, m->overlay[3], "body") : 0;
	snes_rnode *tab = body ? child_named(pk, body, m->legal_tab == 0 ? "copyright" : "oss") : 0;
	snes_rnode *txt = tab ? child_named(pk, tab, "text") : 0;
	snes_rnode *sb = txt ? child_named(pk, txt, "scrollbar") : 0;
	snes_rnode *bar = sb ? child_named(pk, sb, "bar") : 0;
	snes_rnode *au = sb ? child_named(pk, sb, "arrow_up") : 0;
	snes_rnode *ad = sb ? child_named(pk, sb, "arrow_down") : 0;
	int total = legal_line_count(m), N = LEGAL_NVIS;
	int base = m->legal_scroll, maxs = total - N + 2;   /* web _legalRender: texts-N+2 */
	const float sbMaxH = 336.0f, sbYOff = 24.0f;
	float denom, ff, top, bot, vis, h, pad, f21;
	if (maxs < 0) maxs = 0;
	if (au) set_spr_comp(pk, &m->home, au, 55, 829, base > 0);      /* allow_up_on */
	if (ad) set_spr_comp(pk, &m->home, ad, 19, 829, base < maxs);   /* allow_down_on */
	if (!bar) return;
	denom = (float)total - (float)(N - 1); if (denom < 1.0f) denom = 1.0f;
	ff = (float)base / denom; if (ff < 0.0f) ff = 0.0f; else if (ff > 1.0f) ff = 1.0f;
	top = base < 0 ? 0.0f : (float)base;
	bot = (float)base + (float)(N - 1); if (bot > (float)total + 1.0f) bot = (float)total + 1.0f;
	vis = (bot - top) / (float)(total < 1 ? 1 : total); if (vis > 1.0f) vis = 1.0f;
	h = vis * sbMaxH; pad = h < 32.0f ? 32.0f - h : 0.0f;
	f21 = ff + vis; if (f21 > 1.0f) f21 = 1.0f;
	bar->tf[5] = (-ff * sbMaxH - sbYOff) + h * (-0.5f + ff * f21) + pad * (-0.5f + ff);
	set_spr_size_h(pk, &m->home, bar, 247, 809, h + pad);
}
static void draw_copyright_list(snes_menu *m, snes_target *t)
{
	const snes_pack *pk = m->pk;
	snes_rnode *body = child_named(pk, m->overlay[3], "body");
	snes_rnode *cp = body ? child_named(pk, body, "copyright") : 0;
	snes_rnode *txt = cp ? child_named(pk, cp, "text") : 0;
	/* the Lua overrides the authored s.font placeholder with copyright.fnt
	 * (the only body font that carries every digit/glyph) at runtime. */
	uint32_t font = snes_hash("copyright.fnt");
	const char *lines[192];
	int nl, i;
	float Y0 = 276.0f - m->open_y;   /* follow the panel slide-in (2px up to match web) */
	if (!txt) return;
	if (m->legal_tab == 1) {         /* Open Source Software tab (packed OSS text) */
		int total = (int)pk->hdr->oss_count;
		for (i = 0; ; i++) {
			int idx = m->legal_scroll + i;
			float y = Y0 + (float)i * 24.0f;
			if (idx >= total || y > 625.0f) break;
			snes_draw_text(t, pk, font, 168.0f, y, 1.0f, 0xFFF0F0F0u, 0,
				       snes_oss_line(pk, (uint32_t)idx));
		}
		return;
	}
	nl = legal_ip_lines(m, lines, 192);
	for (i = 0; ; i++) {
		int idx = m->legal_scroll + i;
		float y = Y0 + (float)i * 24.0f;
		if (idx >= nl || y > 625.0f) break;
		snes_draw_text(t, pk, font, 168.0f, y, 1.0f, 0xFFF0F0F0u, 0,
			       lines[idx]);
	}
}
/* Suspend Point List (resume) hint: a single centred "[B] Back". The web renders
 * resumemenu/hud in the 'bottom' aspect group; in 4:3 it lands at screen y~916,
 * below the content-zoomed panel. hud_empty is a horizontal-flow layout of the B
 * icon + "Back" label centred at x640 (refreshHud). Drawn in identity view. */
static void draw_resume_hint(snes_menu *m, snes_target *t)
{
	const float sc = 0.85f, gap = 6.0f, y = 916.0f;
	const char *lab = snes_text(m->pk, "sys_resume_hud_Return");
	float iw = 12.0f * 3.0f, lw, total, x;
	snes_spr_entry ic;
	if (!m->card_act) return;
	lw = snes_text_width(m->pk, m->f_s, sc, lab);
	total = iw + gap + lw;
	x = 640.0f - total / 2.0f;
	ic = (snes_spr_entry){ m->card_act->img, 15, 881, 12, 12, 6, 6 };
	snes_blit_spr(t, m->pk, &ic, x + iw / 2.0f, y, 3.0f, 1.0f);
	snes_draw_text(t, m->pk, m->f_s, x + iw + gap, y - 10.0f, sc, 0xFFF2F2F2u, 0, lab);
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
	draw_hint_row(m, t, H, 4, 626.0f, 1.0f);   /* 1x = integer scale = pixel-sharp text */
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
	draw_hint_row(m, t, H, 4, 626.0f, 0.85f);
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
				/* vertically centre the label in the box: web caption text sits at
				 * ~cy+7 (slightly below the box centre). pen y is the line top, so
				 * offset up ~half the visible text height. */
				snes_draw_text(t, m->pk, fh, cx,
					       cy - (fe ? fe->line_height : 31) * tsc * s * 0.42f,
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
/* the 11 frame-theme thumbnails (folder order); item 0 = "none" (authored item_0). */
static const char *g_frame_thumbs[11] = {
	"frame_thumb_01_ambient", "frame_thumb_02_wire", "frame_thumb_03_crystal",
	"frame_thumb_04_dot", "frame_thumb_05_mosaic", "frame_thumb_06_dot2",
	"frame_thumb_07_wood", "frame_thumb_08_space", "frame_thumb_09_speaker",
	"frame_thumb_10_curtain", "frame_thumb_11_midnight",
};
#define FRAME_COUNT 12          /* none + 11 themes */
#define FRAME_DISP  4           /* visible window width */
/* window slot screen x (slot 0 = the authored item_0 "none" position). */
static const float g_frame_slot_x[FRAME_DISP] = { 347.0f, 541.0f, 737.0f, 933.0f };
static void draw_frame_strip(snes_menu *m, snes_target *t)
{
	float cy = 591.0f - m->open_y;
	int j;
	for (j = 0; j < FRAME_DISP; j++) {
		int i = m->frame_scroll + j;
		float x = g_frame_slot_x[j];
		if (i >= FRAME_COUNT) break;
		if (i >= 1) {           /* item 0 (none) is the authored item_0 at slot 0 */
			const snes_img_entry *im = snes_res_img(m->pk, snes_hash(g_frame_thumbs[i - 1]));
			if (im) snes_blit_tex(t, m->pk, im, x, cy, 172.0f, 98.0f, 1.0f);
		}
		/* applied-frame check marker (atlas 67,809 20x14), centred on the item */
		if (i == m->frame_applied && i >= 1 && m->card_act) {
			snes_spr_entry ck = { m->card_act->img, 67, 809, 20, 14, 10, 7 };
			snes_blit_spr(t, m->pk, &ck, x, cy, 3.0f, 1.0f);
		}
		/* frame-zone cursor: the cyan selection box around the focused item
		 * (same 9-slice as the menubar cursor, sized to the 172x98 frame). */
		if (m->disp_zone == 1 && i == m->frame_sel && m->card_act) {
			static const uint16_t sxa[10] = {0, 69,131, 69,137,156,171, 88,137,103};
			static const uint16_t sya[10] = {0,847,987,945,941,895,941,895,847,847};
			draw_cursor_9slice(m, t, x, cy, 184.0f, 110.0f, sxa, sya, 1.0f, 1.0f, m->card_act->img);
		}
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
/* The Options confirm dialog (sys_dialog) is the FIRST child of option_settings,
 * so the scene render draws it behind the toggle rows. Re-render it on top when
 * open (it slides up from y=-720; opaque base covers the list). */
static void draw_reset_dialog(snes_menu *m, snes_target *t)
{
	snes_rnode *dl, *base;
	if (m->open != 1 || !m->reset_dlg_open || !m->overlay[1]) return;
	dl = desc(m->pk, m->overlay[1], "sys_dialog");
	if (!dl) return;
	/* the dialog's `base` is an empty-sprite solid-colour quad (the renderer skips
	 * those), so paint the opaque black backing (646x374, web-matched) at its world
	 * position before the frame/text/buttons render on top. */
	base = desc(m->pk, dl, "base");
	if (base) {
		float w[6];
		snes_node_world(base, w);
		snes_fill_quad(t, 640.0f + w[2], 360.0f - w[5], 646.0f, 374.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	}
	snes_render_node(t, &m->home, dl);
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
	m->chrome = chrome; m->chrome_ready = 0; m->aspect = 0;
	m->disp_cur = m->disp_sel = 1; m->sub_rep_t = 0.0f; m->sub_rep_ctrl = 0;
	m->disp_zone = 0; m->frame_sel = 0; m->frame_scroll = 0; m->frame_applied = 0;
	m->opt_cur = 0; m->opt_on = 0x7;   /* 3 toggles, all on by default */
	m->lang_cur = m->lang_sel = 0;     /* English (top-left) */
	m->reset_t = 0.0f; m->reset_armed = 0; m->reset_dlg_open = 0;
	m->dlg_focus = 0; m->reset_dlg_y = -720.0f;
	m->legal_tab = 0; m->legal_scroll = 0;
	m->focus = 0; m->car_x = 0; m->car_target = 0;
	m->sel_world = CAR_SLOT_X; m->cont_shift = 0;
	m->prev_focus = 0; m->xfade_t = 0.0f; m->resume_dim = 0.0f;
	m->pl = m->pr = m->pu = m->pd = m->pa = m->pb = m->ps = 0;
	m->sndh = m->sndt = 0; m->wall = 0;
	m->bg_acc = 0.0f; m->scr_speed = BG_DEFAULT_SPEED; m->scr_dir = 1.0f;
	m->cur_scroll_time = 0.0f; m->cur_scroll_spd = 0.0f;
	m->rep_t = 0.0f; m->rep_dir = 0; m->rep_primed = 0;
	m->car_tween = CAR_REPEAT_DELAY;
	m->resume_expl_t = 3.5f;   /* notice starts hidden until the list is opened */

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
		/* emptymode (the "When you reset a game..." hint + an overlapping card
		 * template) is hidden by default; the 4:3 resume render re-enables just the
		 * hint (it sits below the 720 fold in 16:9, so only 4:3 shows it). */
		disable_all_named(pk, m->resume, "emptymode");
		/* the resume hud stacks 4 sub-huds (empty/change/locked/float) at x=0; the
		 * empty state shows only hud_empty ("Back") - hide the others so they do not
		 * overlap into garbled text. Ports hudSelect("empty"). */
		{
			snes_rnode *hud = desc(pk, m->resume, "hud"), *h;
			static const char *others[3] = { "hud_change", "hud_locked", "hud_float" };
			int oi;
			if (hud) for (oi = 0; oi < 3; oi++)
				if ((h = child_named(pk, hud, others[oi]))) h->enabled = 0;
		}
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
		snes_rnode *it;
		if (fn) fn->enabled = 0;               /* hide Frame-line blue bar */
		/* disable the authored `cursor` dots (drawn as radiobtn in render) */
		for (it = el ? el->child : 0; it; it = it->sib) {
			snes_rnode *c2 = child_named(pk, it, "cursor");
			if (c2) c2->enabled = 0;
		}
		/* default cursor + committed mode = 4:3 (visual index 1); apply_display_state
		 * sets cursor_area on disp_cur and radiobtn_on/off per disp_sel. */
		m->disp_cur = m->disp_sel = 1;
		apply_display_state(m);
		frame_layout(m);   /* item_0 / arrows for the frame carousel at rest */
	}
	/* option_languages resting state: `cursor` is the per-item focus ARROW
	 * (shown on all -> hide); enable `cursor_area` (blue box) on the selected
	 * language (English for the default usa_en locale, the top-left item). */
	if (m->overlay[2]) {
		snes_rnode *el = child_named(pk, m->overlay[2], "elements"), *it;
		for (it = el ? el->child : 0; it; it = it->sib) {
			snes_rnode *cur = child_named(pk, it, "cursor");
			snes_rnode *btn = child_named(pk, it, "button");
			if (cur) cur->enabled = 0;          /* hide focus arrow */
			if (btn) btn->enabled = 0;          /* hide authored dot (draw our own) */
		}
		/* cursor + selection default to English (top-left, grid index 0);
		 * apply_language_state shows the cursor blue box, the dot is drawn by render. */
		m->lang_cur = m->lang_sel = 0;
		apply_language_state(m);
	}
	/* option_settings resting state: hide the per-item focus arrows; cursor on the
	 * top row (setting0), all 3 toggles ON. apply_options_state sets cursor_area on
	 * opt_cur and swaps switch_on (481,755) / switch_off (481,737) per opt_on. */
	if (m->overlay[1]) {
		disable_all_named(pk, m->overlay[1], "cursor");
		m->opt_cur = 0; m->opt_on = 0x7;
		apply_options_state(m);
		set_reset_gauge(m, 0.0f);                  /* fill gauge hidden at rest */
		disable_all_named(pk, m->overlay[1], "sys_dialog");   /* confirm dialog hidden */
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
	m->cur_slide_t = CUR_SLIDE_DUR;   /* cursor slide idle at boot */
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
	if (cardShift != 0.0f) m->car_navd = 1;   /* web tween advances from NEXT frame */
}

/* Display screen-mode line: resolve the 3 radio items in visual (left->right)
 * order [CRTFilter(-360), 4:3(0), DotByDot(+360)] by name. */
static void resolve_disp_items(snes_menu *m, snes_rnode *out[3])
{
	snes_rnode *el = m->overlay[0] ? child_named(m->pk, m->overlay[0], "elements") : 0, *it;
	out[0] = out[1] = out[2] = 0;
	for (it = el ? el->child : 0; it; it = it->sib) {
		if      (name_eq(m->pk, it, "item_CRTFilter")) out[0] = it;
		else if (name_eq(m->pk, it, "item_4_3"))       out[1] = it;
		else if (name_eq(m->pk, it, "item_DotByDot"))  out[2] = it;
	}
}
/* Reflect disp_cur (cursor_area blue box) + disp_sel (radiobtn_on filled ring,
 * atlas 113,881; radiobtn_off empty ring 99,881) onto the 3 mode items. */
static void apply_display_state(snes_menu *m)
{
	snes_rnode *it[3];
	int i;
	resolve_disp_items(m, it);
	for (i = 0; i < 3; i++) {
		snes_rnode *ca;
		if (!it[i]) continue;
		ca = child_named(m->pk, it[i], "cursor_area");
		if (ca) ca->enabled = (i == m->disp_cur) && (m->disp_zone == 0);
		set_spr_comp(m->pk, &m->home, it[i], 113, 881, i == m->disp_sel);
		set_spr_comp(m->pk, &m->home, it[i], 99, 881, i != m->disp_sel);
	}
}

/* Frame carousel layout: the authored item_0 ("none") shows only when the scroll
 * window includes index 0; its check marker (67,809) shows when none is applied;
 * its cursor_area shows when none is focused in the frame zone. The scroll arrows
 * light per available scroll. Ports resetFrameScrollDisplay + _updateFrameArrows. */
static void frame_layout(snes_menu *m)
{
	const snes_pack *pk = m->pk;
	snes_rnode *ef = m->overlay[0] ? child_named(pk, m->overlay[0], "elements_frame") : 0;
	snes_rnode *els = ef ? child_named(pk, ef, "elements") : 0;
	snes_rnode *it0 = els ? child_named(pk, els, "item_0") : 0;
	snes_rnode *itf = ef ? child_named(pk, ef, "item_frame") : 0;
	snes_rnode *al = itf ? child_named(pk, itf, "arrow_left") : 0;
	snes_rnode *ar = itf ? child_named(pk, itf, "arrow_right") : 0;
	snes_rnode *r;
	if (it0) {
		it0->enabled = (m->frame_scroll == 0);
		set_spr_comp(pk, &m->home, it0, 67, 809, m->frame_applied == 0);  /* check on none */
		if ((r = child_named(pk, it0, "cursor_area")))
			r->enabled = (m->disp_zone == 1 && m->frame_sel == 0);
	}
	if (al) al->enabled = (m->frame_scroll > 0);
	if (ar) ar->enabled = (m->frame_scroll + FRAME_DISP < FRAME_COUNT);
}
/* Move the frame cursor (no wrap), shifting the scroll window to keep it visible. */
static void frame_move(snes_menu *m, int dir)
{
	int next = m->frame_sel + dir;
	if (next < 0 || next >= FRAME_COUNT) return;
	if (dir < 0 && next < m->frame_scroll) m->frame_scroll = next;
	else if (dir > 0 && next >= m->frame_scroll + FRAME_DISP) m->frame_scroll = next - FRAME_DISP + 1;
	m->frame_sel = next;
	frame_layout(m);
	push_snd(m, m->sfx_move);
}
/* Applying a frame re-skins each mode item's `display` preview with that frame's
 * pre-rendered image (4:3/CRT use the _4_3 preview, DotByDot the _pixel_perfect
 * one). frame 0 = "none" clears the override, restoring the base preview. Ports
 * _updateFramePreviews. */
static const char *g_frame_folders[11] = {
	"01_ambient", "02_wire", "03_crystal", "04_dot", "05_mosaic", "06_dot2",
	"07_wood", "08_space", "09_speaker", "10_curtain", "11_midnight",
};
static void apply_frame_previews(snes_menu *m)
{
	static const char *pfx[3] = {              /* CRTFilter, 4:3, DotByDot */
		"frame_preview_43_", "frame_preview_43_", "frame_preview_pp_",
	};
	snes_rnode *it[3];
	int k;
	resolve_disp_items(m, it);
	for (k = 0; k < 3; k++) {
		snes_rnode *disp;
		const snes_img_entry *im = 0;
		if (!it[k]) continue;
		disp = child_named(m->pk, it[k], "display");
		if (!disp) continue;
		if (m->frame_applied >= 1 && m->frame_applied <= 11) {
			char key[64];
			const char *a = pfx[k], *b = g_frame_folders[m->frame_applied - 1];
			int p = 0;
			while (*a && p < 60) key[p++] = *a++;
			while (*b && p < 63) key[p++] = *b++;
			key[p] = 0;
			im = snes_res_img(m->pk, snes_hash(key));
		}
		disp->vis.ov_img = im;   /* NULL for "none" restores the packed base preview */
	}
}

/* Ports navFire: on a fresh press of any allowed D-pad control (checked in
 * priority order) latch it, seed the delay, and fire; while held, fire every
 * `rate`; on release clear. Returns the fired control (1 up, 2 down, 3 left,
 * 4 right) or 0. allow_ud gates the vertical controls (Display uses L/R only). */
static int sub_navfire(snes_menu *m, const snes_input *in, float dt,
		       int mask, float delay, float rate)
{
	int lvl[5]  = { 0, in->up, in->down, in->left, in->right };
	int prev[5] = { 0, m->pu,  m->pd,    m->pl,    m->pr };
	int order[4], n = 0, i, c;
	if (mask & 1) order[n++] = 1;   /* up */
	if (mask & 2) order[n++] = 2;   /* down */
	if (mask & 4) order[n++] = 3;   /* left */
	if (mask & 8) order[n++] = 4;   /* right */
	for (i = 0; i < n; i++) { c = order[i]; if (lvl[c] && !prev[c]) { m->sub_rep_ctrl = c; m->sub_rep_t = delay; return c; } }
	if (m->sub_rep_ctrl) {
		int allowed = 0;
		c = m->sub_rep_ctrl;
		for (i = 0; i < n; i++) if (order[i] == c) allowed = 1;
		if (allowed && lvl[c]) { m->sub_rep_t -= dt; if (m->sub_rep_t < 0.0f) { m->sub_rep_t = rate; return c; } return 0; }
		if (!lvl[c]) m->sub_rep_ctrl = 0;
	}
	return 0;
}

/* Options list: resolve the 4 nav rows top->bottom [setting0, setting1,
 * setting2, sys_button(reset)] (natural child order = descending world y). */
static void resolve_opt_items(snes_menu *m, snes_rnode *out[4])
{
	static const char *nm[4] = { "setting0", "setting1", "setting2", "sys_button" };
	int i;
	for (i = 0; i < 4; i++) out[i] = m->overlay[1] ? desc(m->pk, m->overlay[1], nm[i]) : 0;
}
/* Reflect opt_cur (cursor_area) + opt_on bits (switch_on 481,755 / switch_off
 * 481,737 under each toggle) onto the Options rows. */
static void apply_options_state(snes_menu *m)
{
	snes_rnode *it[4];
	int i;
	resolve_opt_items(m, it);
	for (i = 0; i < 4; i++) {
		snes_rnode *ca, *bf, *bi;
		int foc = (i == m->opt_cur);
		if (!it[i]) continue;
		ca = child_named(m->pk, it[i], "cursor_area");
		if (ca) ca->enabled = foc;
		/* GUIButton idle/focus frames: the System Reset button shows btn_focus
		 * (its distinct focused colour) when selected - the web _navHighlight. */
		bf = child_named(m->pk, it[i], "btn_focus"); if (bf) bf->enabled = foc;
		bi = child_named(m->pk, it[i], "btn_idle");  if (bi) bi->enabled = !foc;
		if (i < 3) {
			int on = (m->opt_on >> i) & 1;
			set_spr_comp(m->pk, &m->home, it[i], 481, 755, on);
			set_spr_comp(m->pk, &m->home, it[i], 481, 737, !on);
		}
	}
}

/* Language 2D grid, row-major [row*2+col]: col0 (wx -492) = language01/02/03/04
 * top->bottom, col1 (wx 36) = language05/06/07/08. Index 0 = English (top-left). */
static const char *lang_name(int idx)
{
	static const char *nm[8] = { "language01", "language05", "language02", "language06",
				     "language03", "language07", "language04", "language08" };
	return (idx >= 0 && idx < 8) ? nm[idx] : "language01";
}
/* cursor_area (blue box) on the lang_cur cell; the selection dot is custom-drawn
 * in the render by lang_sel, so only the cursor is reflected here. */
static void apply_language_state(snes_menu *m)
{
	snes_rnode *el = m->overlay[2] ? child_named(m->pk, m->overlay[2], "elements") : 0, *it;
	const char *cur = lang_name(m->lang_cur);
	for (it = el ? el->child : 0; it; it = it->sib) {
		snes_rnode *ca = child_named(m->pk, it, "cursor_area");
		if (ca) ca->enabled = name_eq(m->pk, it, cur);
	}
}

/* Options System Reset (sys_button_longpress): scale the fill gauge to `rate`
 * (0..1) and flank it with the L/R end caps. rate <= 0 hides all three. Ports
 * _setResetGauge (gauge w=426, caps w=3). */
#define RESET_LONGPRESS_SEC 5.0f
static void set_reset_gauge(snes_menu *m, float rate)
{
	snes_rnode *sb = m->overlay[1] ? desc(m->pk, m->overlay[1], "sys_button") : 0;
	snes_rnode *g  = sb ? desc(m->pk, sb, "gauge") : 0;
	snes_rnode *gl = sb ? desc(m->pk, sb, "gauge_l") : 0;
	snes_rnode *gr = sb ? desc(m->pk, sb, "gauge_r") : 0;
	if (!g) return;
	if (rate <= 0.0f) { g->enabled = 0; if (gl) gl->enabled = 0; if (gr) gr->enabled = 0; return; }
	g->enabled = 1; if (gl) gl->enabled = 1; if (gr) gr->enabled = 1;
	g->tf[0] = rate;                                  /* setLocalScale(rate, 1) */
	{
		float half = (426.0f * rate) / 2.0f;
		if (gl) gl->tf[2] = -(half + 1.5f);       /* glw/2 = 1.5 */
		if (gr) gr->tf[2] = half + 1.5f;
	}
}
/* reset dialog button focus: swap btn_idle/btn_focus on Cancel vs Reset. */
static void set_dialog_focus(snes_menu *m, int which)
{
	snes_rnode *dl = m->overlay[1] ? desc(m->pk, m->overlay[1], "sys_dialog") : 0;
	snes_rnode *el = dl ? child_named(m->pk, dl, "elements") : 0;
	snes_rnode *dec = el ? child_named(m->pk, el, "btn_decide") : 0;
	snes_rnode *can = el ? child_named(m->pk, el, "btn_cancel") : 0;
	snes_rnode *n;
	if (dec) { if ((n = child_named(m->pk, dec, "btn_idle"))) n->enabled = (which != 1);
		   if ((n = child_named(m->pk, dec, "btn_focus"))) n->enabled = (which == 1); }
	if (can) { if ((n = child_named(m->pk, can, "btn_idle"))) n->enabled = (which != 0);
		   if ((n = child_named(m->pk, can, "btn_focus"))) n->enabled = (which == 0); }
	m->dlg_focus = which;
}
/* open/close the Yes/No confirm dialog (authored off-screen at y=-720, slides up).
 * Reset would reboot the console (out of scope), so both buttons just close. */
static void open_reset_dialog(snes_menu *m)
{
	snes_rnode *dl = m->overlay[1] ? desc(m->pk, m->overlay[1], "sys_dialog") : 0;
	if (!dl) return;
	m->reset_dlg_open = 1;
	dl->enabled = 1;
	m->reset_dlg_y = -720.0f;
	m->reset_dlg_t = 0.0f;
	dl->tf[5] = m->reset_dlg_y;
	set_dialog_focus(m, 0);                            /* Cancel focused (safe default) */
}
static void close_reset_dialog(snes_menu *m)
{
	snes_rnode *dl = m->overlay[1] ? desc(m->pk, m->overlay[1], "sys_dialog") : 0;
	if (dl) dl->enabled = 0;
	m->reset_dlg_open = 0;
	m->reset_armed = 0; m->reset_t = 0.0f;
	set_reset_gauge(m, 0.0f);
}
/* Legal tab switch (IP Notice / Open Source Software): the active tab shows its
 * cursor_area (blue box) + tab_on, the other its tab_off; the matching body is
 * enabled. Scroll resets to the top. Ports legalTab + _legalTabHighlight. */
static void legal_set_tab(snes_menu *m, int tab)
{
	const snes_pack *pk = m->pk;
	snes_rnode *menu = m->overlay[3] ? child_named(pk, m->overlay[3], "menu") : 0;
	snes_rnode *body = m->overlay[3] ? child_named(pk, m->overlay[3], "body") : 0;
	snes_rnode *tabs[2], *bods[2], *r;
	int i;
	tabs[0] = menu ? child_named(pk, menu, "copyright") : 0;
	tabs[1] = menu ? child_named(pk, menu, "oss") : 0;
	bods[0] = body ? child_named(pk, body, "copyright") : 0;
	bods[1] = body ? child_named(pk, body, "oss") : 0;
	for (i = 0; i < 2; i++) {
		int on = (i == tab);
		if (tabs[i]) {
			if ((r = child_named(pk, tabs[i], "cursor_area"))) r->enabled = on;
			if ((r = child_named(pk, tabs[i], "tab_on")))  r->enabled = on;
			if ((r = child_named(pk, tabs[i], "tab_off"))) r->enabled = !on;
		}
		if (bods[i]) bods[i]->enabled = on;
	}
	m->legal_tab = tab;
	m->legal_scroll = 0;
	legal_scrollbar(m);
}

void snes_menu_update(snes_menu *m, const snes_input *in, float dt)
{
	float k;
	int el = in->left && !m->pl, er = in->right && !m->pr;
	m->clock += dt;
	if (m->cur_slide_t < CUR_SLIDE_DUR) m->cur_slide_t += dt;   /* carousel<->menubar cursor slide */
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
			/* the web close is moveTo(...,'inExpo') over 0.2s (2^(10(r-1))): the panel
			 * stays nearly still ~3 frames then accelerates off. r^6 approximates inExpo
			 * libc-free (frame4 .088 vs .099, frame5 .335 vs .314) - much closer than the
			 * old r^4 (~2x too fast mid-slide). close_target: +up submenu (2) / -down (3). */
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
				m->open_y = m->close_target * close_ease_inexpo(r);
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
		if (eu) { m->state = 1; m->cap_t = 0.0f; m->cap_s = 0.0f; m->hl_s = 0.0f; m->cur_slide_t = 0.0f; push_snd(m, m->sfx_up); }
		if (ed && m->resume) {                 /* Down -> suspend-point menu */
			m->resume->enabled = 1;
			/* the panel slides UP from off-bottom into place, easing out (world
			 * -y = screen down); reuse the open_y ease (toward 0). */
			m->open_y = -RESUME_SLIDE;
			m->resume_expl_t = 0.0f;    /* re-arm the "When you reset..." notice hold+fade */
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
		if (ed || eb) { m->state = 0; m->cur_slide_t = 0.0f; push_snd(m, m->sfx_cancel); }
		if (ea && m->overlay[m->mb_focus]) {
			m->open = m->mb_focus;
			m->overlay[m->open]->enabled = 1;
			m->overlay[m->open]->tf[2] = 0;
			m->overlay[m->open]->tf[5] = 0;
			m->sub_rep_ctrl = 0;   /* clear any stale D-pad auto-repeat */
			if (m->open == 0) {    /* Display: cursor opens on the committed mode */
				m->disp_cur = m->disp_sel;
				m->disp_zone = 0;            /* open focused on the mode line */
				m->frame_sel = 0; m->frame_scroll = 0;
				apply_display_state(m);
				frame_layout(m);
			} else if (m->open == 1) {   /* Options: cursor opens on the top row */
				m->opt_cur = 0;
				apply_options_state(m);
				m->reset_armed = 0; m->reset_t = 0.0f;
				close_reset_dialog(m);   /* also hides gauge + dialog node */
			} else if (m->open == 2) {   /* Language: cursor opens on the selection */
				m->lang_cur = m->lang_sel;
				apply_language_state(m);
			} else if (m->open == 3) {   /* Legal: open on the IP Notice tab, top */
				legal_set_tab(m, 0);
			}
			/* the panel slides DOWN from off-top into place, easing out (measured
			 * against the web: the selection box travels ~585px over ~5 frames at
			 * 30fps with per-frame factor ~0.67). Seed the slide offset; the ease
			 * in snes_menu_update brings it to 0. World +y = screen up. */
			m->open_y = OPEN_SLIDE;
			m->state = 2; push_snd(m, m->sfx_decide);
		}
	} else if (m->state == 2) {                /* ---- open submenu ---- */
		/* Display screen (cm1): two zones - the screen-mode radio line and the frame
		 * carousel. Up/Down switch zones (switchDispZone). In the mode line L/R move
		 * the cursor + A commits the mode; in the frame carousel L/R scroll the
		 * selection (frameMove, no wrap) + A applies the frame (moves the check).
		 * Ports updateDisplayNav + navHoriz + frameMove + applyFrame. */
		if (m->open == 0 && !m->closing) {
			if (eu || ed) {                   /* discrete zone switch */
				m->disp_zone ^= 1;
				apply_display_state(m);   /* hides the mode cursor in the frame zone */
				frame_layout(m);
				push_snd(m, m->sfx_move);
			} else if (m->disp_zone == 1) {   /* frame carousel */
				int c = sub_navfire(m, in, dt, 12, DISP_HOLD_DELAY, DISP_HOLD_RATE);
				if (c) frame_move(m, c == 3 ? -1 : 1);
				if (ea) {                 /* applyFrame: re-applying the same frame still plays the SFX */
					m->frame_applied = m->frame_sel;
					frame_layout(m);
					apply_frame_previews(m);  /* re-skin the 3 mode previews */
					push_snd(m, m->sfx_decide);
				}
			} else {                          /* screen-mode line */
				int c = sub_navfire(m, in, dt, 12, DISP_HOLD_DELAY, DISP_HOLD_RATE);
				if (c) {                          /* 3 = left, 4 = right */
					m->disp_cur = (m->disp_cur + (c == 3 ? -1 : 1) + 3) % 3;
					apply_display_state(m);
					push_snd(m, m->sfx_move);
				}
				if (ea) {                 /* selectDisplayModeAtCursor: commits + plays SFX on every A */
					m->disp_sel = m->disp_cur;
					apply_display_state(m);
					push_snd(m, m->sfx_decide);
				}
			}
		}
		/* Options screen (cm2): vertical list of 3 toggle rows + the System Reset
		 * button. Up/Down move the cursor (moveNav, SUB_HOLD auto-repeat, wrap 4);
		 * L/R on a toggle set it on(R)/off(L) (setToggle, no-op if already there);
		 * A flips the focused toggle (activateNav). The reset row is a long-press
		 * gauge (OK held over it fills over RESET_LONGPRESS_SEC) that opens a Yes/No
		 * confirm dialog (sys_button_longpress + sys_dialog, ported below). */
		if (m->open == 1 && !m->closing && m->reset_dlg_open) {
			/* confirm dialog input: L focuses Cancel, R focuses Reset, A commits
			 * (both just close - Reset would reboot, out of scope), B cancels.
			 * Ease the dialog slide-in (-720 -> 0) each frame. */
			/* slide the dialog up from -720 to 0 over 0.2s outExpo (Tween.moveTo,
			 * matches the submenu-open easing) */
			snes_rnode *dl = desc(m->pk, m->overlay[1], "sys_dialog");
			m->reset_dlg_t += dt;
			m->reset_dlg_y = -720.0f + 720.0f * ease_outexpo(m->reset_dlg_t / 0.2f);
			if (m->reset_dlg_y > 0.0f) m->reset_dlg_y = 0.0f;
			if (dl) dl->tf[5] = m->reset_dlg_y;
			if (el && m->dlg_focus != 0) { set_dialog_focus(m, 0); push_snd(m, m->sfx_move); }
			else if (er && m->dlg_focus != 1) { set_dialog_focus(m, 1); push_snd(m, m->sfx_move); }
			else if (ea) { push_snd(m, m->sfx_decide); close_reset_dialog(m); }
			else if (eb) { push_snd(m, m->sfx_cancel); close_reset_dialog(m); }
		} else if (m->open == 1 && !m->closing) {
			int c = sub_navfire(m, in, dt, 15, SUB_HOLD_DELAY, SUB_HOLD_RATE);
			if (c == 1 || c == 2) {           /* up / down: move cursor, wrap 4 */
				m->opt_cur = (m->opt_cur + (c == 1 ? 3 : 1)) % 4;
				apply_options_state(m);
				set_reset_gauge(m, 0.0f); m->reset_armed = 0; m->reset_t = 0.0f;
				push_snd(m, m->sfx_move);
			} else if ((c == 3 || c == 4) && m->opt_cur < 3) {  /* L off / R on */
				int on = (c == 4);
				if (((m->opt_on >> m->opt_cur) & 1) != (unsigned)on) {
					m->opt_on ^= (1u << m->opt_cur);
					apply_options_state(m);
					push_snd(m, m->sfx_decide);
				}
			}
			if (ea && m->opt_cur < 3) {       /* A flips the focused toggle */
				m->opt_on ^= (1u << m->opt_cur);
				apply_options_state(m);
				push_snd(m, m->sfx_decide);
			}
			/* System Reset row: OK held fills the gauge over RESET_LONGPRESS_SEC,
			 * then opens the confirm dialog; releasing early cancels the fill. */
			if (m->opt_cur == 3) {
				if (in->a) {
					if (ea) { m->reset_armed = 1; m->reset_t = 0.0f; }
					else if (m->reset_armed) {
						m->reset_t += dt;
						if (m->reset_t >= RESET_LONGPRESS_SEC) { set_reset_gauge(m, 1.0f); open_reset_dialog(m); }
						else set_reset_gauge(m, m->reset_t / RESET_LONGPRESS_SEC);
					}
				} else if (m->reset_armed || m->reset_t > 0.0f) {
					m->reset_armed = 0; m->reset_t = 0.0f; set_reset_gauge(m, 0.0f);
				}
			}
		}
		/* Language screen (cm3): 2D radio grid (4 rows x 2 cols). U/D/L/R move the
		 * cursor (moveNav, wrap, SUB_HOLD auto-repeat); A selects the focused
		 * language (the radiobtn_on dot moves to it). Ports the Language _buildGrid +
		 * moveNav + selectRadio path. */
		if (m->open == 2 && !m->closing) {
			int c = sub_navfire(m, in, dt, 15, SUB_HOLD_DELAY, SUB_HOLD_RATE);
			if (c) {
				int row = m->lang_cur / 2, col = m->lang_cur % 2;
				if (c == 1)      row = (row + 3) % 4;   /* up */
				else if (c == 2) row = (row + 1) % 4;   /* down */
				else if (c == 3) col = (col + 1) % 2;   /* left/right wrap 2 cols */
				else             col = (col + 1) % 2;
				m->lang_cur = row * 2 + col;
				apply_language_state(m);
				push_snd(m, m->sfx_move);
			}
			if (ea && m->lang_sel != m->lang_cur) {
				m->lang_sel = m->lang_cur;
				push_snd(m, m->sfx_decide);
			}
		}
		/* Legal screen (copyright, open==3): Up/Down page-scroll the active tab's
		 * text (navFire SUB_HOLD), Left/Right switch the IP/OSS tab (discrete). Ports
		 * the copyright dispatch (legalScroll + legalTab). */
		if (m->open == 3 && !m->closing) {
			int c = sub_navfire(m, in, dt, 3, SUB_HOLD_DELAY, SUB_HOLD_RATE);  /* U/D only */
			if (c == 1 || c == 2) {
				int total = legal_line_count(m);
				int maxs = total - LEGAL_NVIS + 2; if (maxs < 0) maxs = 0;  /* web: texts-N+2 */
				int step = LEGAL_NVIS - 1;
				int next = m->legal_scroll + (c == 1 ? -step : step);
				if (next < 0) next = 0; else if (next > maxs) next = maxs;
				if (next != m->legal_scroll) { m->legal_scroll = next; legal_scrollbar(m); }
			}
			if (el && m->legal_tab != 0) { legal_set_tab(m, 0); push_snd(m, m->sfx_move); }
			else if (er && m->legal_tab != 1) { legal_set_tab(m, 1); push_snd(m, m->sfx_move); }
		}
		/* B closes the submenu - unless the reset dialog is open, where B is
		 * consumed above to close just the dialog. */
		if (eb && !m->closing && !(m->open == 1 && m->reset_dlg_open)) {
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
			/* the item is ACTIVE (blue fill + caption + 1.2 icon) only while the
			 * menubar itself is focused (state 1). Opening a submenu deactivates it
			 * (icon -> 1.0, caption out, highlight alpha 0 - controller.openSubmenu);
			 * only the cursor outline stays, drawn live in the state-2 render. */
			int foc = (m->state == 1 && b == m->mb_focus);
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
		if (m->car_navd) {
			/* the frame a nav set cont_shift: the web (Tween.moveTo) does not
			 * advance the fresh tween until the next frame - hold this frame. */
			m->car_navd = 0;
		} else if (m->cont_shift > 0) { m->cont_shift -= rate; if (m->cont_shift < 0) m->cont_shift = 0; }
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
	/* empty-list explanation notice: hold 2s then fade over 1s (showExplanation) */
	if (m->state == 3 && !m->closing && m->resume_expl_t < 3.5f) m->resume_expl_t += dt;
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

	/* 4:3 fills the whole 960 panel (no letterbox); the per-view-group aspect
	 * transform placed by set_view handles the vertical centring instead. */
	if (m->aspect) t->offy = 0;

	/* the chrome cache is built for one aspect; rebuild it when the aspect
	 * changed (build_chrome clears/repaints it with the new view transforms). */
	if (!m->chrome_ready) build_chrome(m);

	/* Submenu (state 2) is a full-screen opaque panel over a black scrim: the
	 * home/wallpaper/carousel behind it are 100% covered, so skip rendering them
	 * entirely. This was ~the whole frame's cost (the carousel even rendered
	 * twice), and starving the single-threaded audio ring feed made SFX loop in
	 * every non-home menu. Just paint the scrim + the panel. */
	if (m->state == 2 && m->open >= 0 && m->overlay[m->open]) {
		/* 4:3: the submenu panel is the 'contain' view group - kept at native 720
		 * and centred in the 960 panel (never zoomed/cropped), so the top/bottom SNES
		 * bars stay visible above and below it. Draw the wallpaper + home chrome (the
		 * two bars pinned to the edges) behind, then the whole panel and all of its
		 * hints/lists/strips in the contain view (they are one 'contain' subtree in
		 * the web, so they move with the panel rather than pinning to the bar edges). */
		if (m->aspect) {
			draw_wp(m, t, (int)m->scroll);
			if (!m->homemenu) snes_render_scene(t, &m->home);
			else              draw_chrome(m, t);
			/* the menubar item that opened this submenu is deactivated (no fill/
			 * caption), but its cyan selection cursor stays on the icon (the web
			 * keeps the cursor); draw it over the rest-state chrome menubar. */
			if (m->card_act && m->mb_focus >= 0 && m->mb_focus < 5) {
				set_view(m, t, VIEW_TOP);
				draw_menubar_cursor(m, t, 448.0f + 96.0f * (float)m->mb_focus,
						    64.0f, 1.0f, m->card_act->img);
			}
			set_view(m, t, VIEW_CONTAIN);
			/* the option scene's own full-native black overlay (contain-scaled) is
			 * the panel's opaque backing - in 16:9 the full-screen scrim provided it,
			 * but contain only covers the centred 720 band, so paint it explicitly to
			 * hide the wallpaper behind the panel (the top/bottom bars still show). */
			snes_fill_quad(t, 640, 360, SNES_VW, SNES_VH, 0.0f, 0.0f, 0.0f, 1.0f);
			m->overlay[m->open]->tf[5] = m->open_y;
			snes_render_node(t, &m->home, m->overlay[m->open]);
			draw_reset_dialog(m, t);
			if (m->open == 3) { draw_copyright_list(m, t); draw_copyright_hints(m, t); }
			else if (m->open == 4) draw_manual_hints(m, t);
			else if (m->open >= 0 && m->open <= 2) draw_option_hints(m, t);
			if (m->open == 0) draw_frame_strip(m, t);
			if (m->open == 2 && m->card_act) {
				snes_rnode *el = child_named(m->pk, m->overlay[2], "elements"), *it;
				for (it = el ? el->child : 0; it; it = it->sib) {
					snes_rnode *btn = child_named(m->pk, it, "button");
					float w[6];
					int on = name_eq(m->pk, it, lang_name(m->lang_sel));
					snes_spr_entry d = { m->card_act->img, (uint16_t)(on ? 113 : 99),
							     881, 12, 12, 6, 6 };
					if (!btn) continue;
					snes_node_world(btn, w);
					snes_blit_spr(t, m->pk, &d, 640.0f + w[2], 360.0f - w[5], 2.4f, 1.0f);
				}
			}
			return;
		}
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
		draw_reset_dialog(m, t);
		if (m->open == 3) { draw_copyright_list(m, t); draw_copyright_hints(m, t); }
		else if (m->open == 4) draw_manual_hints(m, t);
		else if (m->open >= 0 && m->open <= 2) draw_option_hints(m, t);
		if (m->open == 0) draw_frame_strip(m, t);
		if (m->open == 2 && m->card_act) {
			snes_rnode *el = child_named(m->pk, m->overlay[2], "elements"), *it;
			for (it = el ? el->child : 0; it; it = it->sib) {
				snes_rnode *btn = child_named(m->pk, it, "button");
				float w[6];
				int on = name_eq(m->pk, it, lang_name(m->lang_sel));
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
	set_view(m, t, VIEW_CONTENT);
	/* OVL layered L0 pass: the card bodies live on the L2 hardware layer, so skip
	 * them here (draw_carousel would re-blit ~7 cards every frame). See OVL_LAYERS.md. */
	if (!t->ovl_split) draw_carousel(m, t);
	PERF_END(2);
	draw_filmstrip(m, t);
	PERF_END(3);
	if (m->state == 0) { set_view(m, t, VIEW_BOTTOM); draw_hints(m, t); }
	else if (m->state == 1) {
		set_view(m, t, VIEW_TOP);
		if (m->mb_focus >= 0 && m->mb_focus < 5 && m->mb_btn[m->mb_focus]) {
			snes_render_node(t, &m->home, m->mb_btn[m->mb_focus]);
			/* square selection cursor around the focused item's 96x70 button
			 * (icons cells 96px apart, first at x448; the cell drops ~5px). */
			if (m->card_act && m->cur_slide_t >= CUR_SLIDE_DUR)
				draw_menubar_cursor(m, t, 448.0f + 96.0f * (float)m->mb_focus,
						    64.0f, m->hl_s, m->card_act->img);
		}
		/* the caption bubble hangs off the menubar (menubar_upper, 'top' group)
		 * just below the focused icon - draw it in VIEW_TOP so it pins to the top
		 * edge with the icons, not the content-zoomed centre. */
		set_view(m, t, VIEW_TOP);
		draw_menubar_caption(m, t);
		set_view(m, t, VIEW_BOTTOM);
		draw_menubar_hints(m, t);
	}
	/* the blue selection cursor sliding between the focused card and menubar item
	 * on Up/Down (state 0/1); draws in its own identity view, replacing the static
	 * card/menubar cursors which are suppressed while it runs. In the OVL layered L0
	 * pass it is composited on the L3 layer instead (above the L2 cards). */
	if (!t->ovl_split) draw_focus_slide(m, t);

	/* the white title bar (caption_title, 348x22 @3x = 1044x66), raised in resume;
	 * drawn here (not in the chrome cache) so it tracks the game title. */
	set_view(m, t, VIEW_BANNERX);
	if (m->card_act) {
		float ty = 178.0f - RESUME_TITLE_DY * m->resume_dim;
		snes_spr_entry bar = { m->card_act->img, 1, 325, 348, 22, 174, 11 };
		snes_blit_spr(t, m->pk, &bar, 640.0f, ty, 3.0f, 1.0f);
	}
	/* focused game name drawn into the authored title frame (SNES title font). The
	 * web renders the SELECTED game's title SOLID BLACK (onElementFocus setColor
	 * (0,0,0,1)); the authored 0.298 grey is only for unselected games. */
	set_view(m, t, VIEW_CONTENT);
	g = game(m, m->focus);
	if (g)
		snes_draw_text(t, m->pk, m->f_title, 640,
			       152.0f - RESUME_TITLE_DY * m->resume_dim, 1.0f,
			       0xFF000000u, 1, snes_str(m->pk, g->name));

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
		snes_rnode *rbg, *rtail;
		m->resume->tf[5] = m->open_y;   /* slide-up-from-bottom offset */
		/* pin the panel tail/chevron under the SELECTED carousel card (ports
		 * _resumeTail: tail.transform[2] = selected card world x). There are two
		 * resumebg variants (resumebg / resumebg_overwrite) each with a tail; pick
		 * the ENABLED one. Without this the chevron stays at its authored centre -
		 * visible in 4:3 where the card is zoomed. */
		rbg = child_named(m->pk, m->resume, "resumebg");
		if (!rbg || !rbg->enabled) rbg = child_named(m->pk, m->resume, "resumebg_overwrite");
		if (rbg && (rtail = child_named(m->pk, rbg, "resumebg_tail")))
			rtail->tf[2] = m->sel_world;
		if (m->aspect) {
			/* the 960 panel has room for the empty-state hint ("When you reset a
			 * game...") that is below the 720 fold in 16:9: enable just the hint box
			 * (emptymode/explanation), keeping the overlapping card template hidden. */
			snes_rnode *em = desc(m->pk, m->resume, "emptymode"), *expl = 0, *eel;
			if (em) {
				em->enabled = 1;
				expl = child_named(m->pk, em, "explanation");
				if ((eel = child_named(m->pk, em, "elements"))) eel->enabled = 0;
			}
			/* resumemenu is 'content' (panel + slot list zoom with the rest) except
			 * its hud hint bar, which pins to the bottom edge like the home bars. */
			{
			snes_rnode *rhud = child_named(m->pk, m->resume, "hud");
			int e_rhud = rhud ? rhud->enabled : 0;
			if (rhud) rhud->enabled = 0;
			/* render the panel + slots first with the hint hidden, then paint its
			 * opaque black backing (the wdw/base is an empty-sprite quad the renderer
			 * skips), then the hint frame + text on top - so it covers the slots. */
			if (expl) expl->enabled = 0;
			set_view(m, t, VIEW_CONTENT);
			snes_render_node(t, &m->home, m->resume);
			/* the notice holds fully opaque for 2s, then fades to 0 over 1s and
			 * disappears (sys_resume_emptymode.showExplanation). */
			{
				float ea = 1.0f;
				if (m->resume_expl_t > 2.0f) ea = 1.0f - (m->resume_expl_t - 2.0f);
				if (ea < 0.0f) ea = 0.0f;
				if (expl && ea > 0.003f) {
					float w[6], oc = expl->col[3];
					snes_rnode *base = child_named(m->pk, desc(m->pk, expl, "wdw"), "base");
					expl->enabled = 1;
					expl->col[3] = oc * ea;
					if (base) {
						snes_node_world(base, w);
						snes_fill_quad(t, 640.0f + w[2], 360.0f - w[5], 800.0f, 48.0f,
							       0.0f, 0.0f, 0.0f, ea);
					}
					snes_render_node(t, &m->home, expl);
					expl->col[3] = oc;
				} else if (expl) {
					expl->enabled = 0;
				}
			}
			/* the scene hud lays its Back hint out via refreshHud (unavailable
			 * here) and inherits the panel world transform, so it does not land
			 * correctly; draw the "[B] Back" hint manually at the bottom edge. */
			if (rhud) rhud->enabled = e_rhud;
			set_view(m, t, VIEW_TOP);
			/* hideBackElements: the web hides the bottom SNES bezel bar in resume,
			 * so the area below the content-zoomed panel is black (not the cached
			 * chrome's bezel). Paint over the bezel strip, then the Back hint. */
			snes_fill_quad(t, 640.0f, 932.0f, 1280.0f, 56.0f, 0.0f, 0.0f, 0.0f, 1.0f);
			draw_resume_hint(m, t);
			}
		} else {
			snes_render_node(t, &m->home, m->resume);
		}
	}
	PERF_END(4);
}
