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
static const snes_game_rec *game(const snes_pack *p, int i)
{
	unsigned n = p->hdr->game_count;
	if (!n) return 0;
	i = ((i % (int)n) + (int)n) % (int)n;
	return (const snes_game_rec *)(p->base + p->game_offs[i]);
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

/* ---- carousel ---- */
#define CAR_HGAP  262.0f
#define CAR_CY    338.0f

static void draw_card(snes_menu *m, snes_target *t, int gi, int focus)
{
	const snes_game_rec *g = game(m->pk, gi);
	float cx = 640.0f + (gi - m->car_x) * CAR_HGAP, cy = CAR_CY;
	int foc = (gi == focus);
	float bw = foc ? 236.0f : 200.0f;
	float bh = bw * 160.0f / 228.0f;
	const snes_img_entry *im = (g && g->thumb_img != 0xFFFF) ? &m->pk->img[g->thumb_img] : 0;
	if (cx < -200 || cx > SNES_VW + 200) return;
	if (foc) snes_fill_quad(t, cx, cy, bw + 18, bh + 18, 0.20f, 0.55f, 1.0f, 1.0f);
	else     snes_fill_quad(t, cx, cy, bw + 10, bh + 10, 0.06f, 0.07f, 0.10f, 1.0f);
	snes_fill_quad(t, cx, cy, bw + 4, bh + 4, 0.0f, 0.0f, 0.0f, 1.0f);
	if (im) snes_blit_tex(t, m->pk, im, cx, cy, bw, bh, 1.0f);
	else    snes_fill_quad(t, cx, cy, bw, bh, 0.15f, 0.15f, 0.2f, 1.0f);
}
static void draw_carousel(snes_menu *m, snes_target *t)
{
	int c = (int)(m->car_x + (m->car_x >= 0 ? 0.5f : -0.5f)), d;
	if ((int)m->pk->hdr->game_count <= 0) return;
	for (d = 5; d >= 1; d--) { draw_card(m, t, c - d, m->focus); draw_card(m, t, c + d, m->focus); }
	draw_card(m, t, m->focus, m->focus);
}

/* ---- public ---- */
int snes_menu_init(snes_menu *m, const snes_pack *pk,
		   snes_rnode *home_pool, unsigned home_cap,
		   snes_rnode *bg_pool, unsigned bg_cap,
		   uint32_t *wp)
{
	const snes_scene_entry *home, *bg;
	unsigned i;
	/* zero the struct fields we rely on */
	m->pk = pk; m->wp = wp; m->wp_ready = 0; m->scroll = 0;
	m->focus = 0; m->car_x = 0; m->car_target = 0;
	m->pl = m->pr = m->pu = m->pd = m->pa = m->pb = 0;
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
			"gametitle_label", 0 };
		int hj;
		for (hj = 0; hide2[hj]; hj++) {
			snes_rnode *o = snes_scene_find(&m->home, hide2[hj]);
			if (o) o->enabled = 0;
		}
	}
	m->homemenu = snes_scene_find(&m->home, "homemenu");
	m->menubar = snes_scene_find(&m->home, "menubar_upper");
	m->f_title = snes_hash("title.font");
	m->f_l = snes_hash("l.font");
	m->f_s = snes_hash("s.font");
	/* sound hashes wired when audio lands */
	m->sfx_move = m->sfx_decide = m->sfx_cancel = m->bgm = 0;
	return 0;
}

void snes_menu_update(snes_menu *m, const snes_input *in, float dt)
{
	float k;
	/* edge-triggered navigation */
	if (in->left && !m->pl) { m->focus--; push_snd(m, m->sfx_move); }
	if (in->right && !m->pr) { m->focus++; push_snd(m, m->sfx_move); }
	m->pl = in->left; m->pr = in->right; m->pu = in->up; m->pd = in->down;
	m->pa = in->a; m->pb = in->b;

	/* smooth carousel scroll toward the focused index */
	k = dt * 12.0f; if (k > 1.0f) k = 1.0f;
	m->car_x += ((float)m->focus - m->car_x) * k;
	if (m->car_x > m->focus - 0.002f && m->car_x < m->focus + 0.002f)
		m->car_x = (float)m->focus;

	/* wallpaper parallax scroll */
	m->scroll += dt * 60.0f * 1.2f;
}

void snes_menu_render(snes_menu *m, snes_target *t)
{
	const snes_game_rec *g;
	draw_wp(m, t, (int)m->scroll);
	/* authored home chrome (menubar, title frame, bottom filmstrip, hud) */
	if (m->homemenu) snes_render_node(t, &m->home, m->homemenu);
	else             snes_render_scene(t, &m->home);
	draw_carousel(m, t);

	/* focused game name drawn into the authored title frame (SNES title font) */
	g = game(m->pk, m->focus);
	if (g)
		snes_draw_text(t, m->pk, m->f_title, 640, 148, 0.85f, 0xFF202020u, 1,
			       snes_str(m->pk, g->name));
}
