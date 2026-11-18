/*
 * pico_classic_igm.c - the in-game menu in the Classic themes' look (pico_classic_igm.h).
 *
 * The sheet is the menu's Options page made into a menu over the game: the same frame (the theme's
 * rounded corner and line, three times their size), the page's icon and name in the large font at
 * its top left, the button hints at its top right, a line under them. Below that the pages sit as
 * tabs, the theme's cyan box (the menubar's cursor, glowing as it does there) on the one shown and
 * gliding to the next; then the rows, the theme's blue bar under the focused one, gliding as the
 * focus moves, the on/off values as the theme's switches and the others between small arrows that
 * nudge the way a value went. It slides down from above the screen with the menu's outExpo ease
 * and goes back up with inExpo; a page change slides the rows sideways and crossfades them.
 *
 * Drawn with the menu's own renderer (snes_render) from the kit: a pack in the menu's format built
 * as a game starts (pico_classic_kit_take), holding the fonts l, m and s (their pages as grey and
 * alpha, two bytes a pixel, since a page keeps a dark edge round each letter) and a small atlas of
 * the sprites below, cut from the theme's atlas. The game keeps running under the sheet, so it is
 * drawn only while something moves (and a few times a second for the cursor's glow), and only the
 * rows of the picture that came out different are handed to the layer.
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "pico_classic_igm.h"
#include "pico_classic_ui.h"
#include "menu/snes_render.h"
#include "menu/gba_name.h"          /* gba_clean_name */
#include "pico_fold.h"              /* the game's name in the theme's letters */
#include "pico_i18n.h"              /* tr(): the menus' languages */

extern unsigned int gpt4_get_current_tick(void);
extern void ayaneo_canvas_dims(unsigned int *cw, unsigned int *ch);
extern void ayaneo_overlay_live(int (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int, int *), int open);
extern unsigned int gba_menu_theme(void);
extern volatile int g_dbg_key, g_dbg_key_hold;
extern volatile unsigned int g_dbg_cigm_us[6];   /* oem diag: what a paint costs */

/* ==== the menu =================================================================================== */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK) || defined(KIT_PA)

#define ROWS     96               /* rows in a page, at most (a core's own options) */
#define FIGS     16               /* figure rows (battery, refresh) kept a second */
#define FW       1152.0f          /* the sheet's width, design units (1280 across) */
#define ROW_P    60.0f            /* the row pitch, as the Options page */
#define OPEN_S   0.42f
#define CLOSE_S  0.24f
#define DIM      176              /* the game behind, darkened to (256 - DIM) / 256 */

typedef struct { int kind, item; const char *label; } cline;
static const char *const k_page[IGM_NPAGES] = { "", "Display", "Sound", "Gameplay", "System", "Core options",
						 "Achievements" };

/* RetroAchievements (pico_ra.c, on the consoles with Wi-Fi; absent elsewhere) */
extern int pico_ra_game_loaded(void) __attribute__((weak));
extern int pico_ra_list(void) __attribute__((weak));
extern const char *pico_ra_row_title(int i) __attribute__((weak));
extern const char *pico_ra_row_value(int i) __attribute__((weak));
extern void pico_ra_paused(void) __attribute__((weak));
static int ra_on(void) { return pico_ra_game_loaded && pico_ra_game_loaded(); }

static snes_pack s_pk;            /* the kit's pack */
static const struct cui_set *s_set; /* its sprites */
static snes_target s_t;
static float s_uh;                /* the screen's height in design units */
static uint32_t s_rowh[1024];     /* each row of the last picture, hashed */
static int s_rowh_ok;
static int s_settled;             /* the last picture had the sheet at rest: all outside it is as it was */
static int s_slide_skip;          /* the slide is drawn every other frame */

static struct {
	const struct igm_core *c;
	int up, closing, latch;
	float show, clock;
	int page, from, dir;
	float page_t;
	int sel[IGM_NPAGES];
	float top[IGM_NPAGES];
	float hy;                     /* the bar, in rows from the list's first (it rides along a scroll) */
	int nudge_row, nudge_dir;
	float nudge_t;
	char toast[96];
	float toast_t;
	unsigned int tick, last_paint, fig_tick;
	int dirty, prev_keys, fig_page, nv;
	float rep[4];
	char title[128];
	int ntab;
	signed char tab[IGM_NPAGES];
	float tab_cx[IGM_NPAGES], tab_w[IGM_NPAGES], tab_sc;
	float box_x, box_w;           /* the tab box, gliding */
	char fig[FIGS][64];
} C;

/* ---- the rows ---- */
static int lines(int page, cline *out)
{
	const struct igm_core *c = C.c;
	int i, n = 0;
	if (page == IGM_CORE) {
		int k = c->opt_n ? c->opt_n() : 0;
		for (i = 0; i < k && n < ROWS; i++, n++) { out[n].kind = IGM_VALUE; out[n].item = i; out[n].label = c->opt_label(i); }
		return n;
	}
	if (page == IGM_ACH) {	/* the set: each achievement, its state on the right (looked at, not changed) */
		static unsigned int built;	/* made again at most once a second */
		static int k;
		unsigned int now = gpt4_get_current_tick();
		if (!built || now - built > 13000000u) { built = now | 1u; k = ra_on() ? pico_ra_list() : 0; }
		for (i = 0; i < k && n < ROWS; i++, n++) { out[n].kind = IGM_INFO; out[n].item = i; out[n].label = pico_ra_row_title(i); }
		return n;
	}
	if (page == IGM_TOP) { out[0].kind = IGM_ACTION; out[0].item = -1; out[0].label = "Resume"; n = 1; }
	for (i = 0; i < c->nrows && n < ROWS; i++) {
		const struct igm_row *r = &c->rows[i];
		cline *l = &out[n];
		if (r->page != page) continue;
		l->kind = r->kind; l->item = r->item;
		if (r->kind == IGM_LINK) {
			if (r->item == IGM_CORE && (!c->opt_n || !c->opt_n())) continue;
			l->label = k_page[r->item < IGM_NPAGES ? r->item : 0];
		} else if (r->item == c->save_item) l->label = "Save state";
		else if (r->item == c->load_item) l->label = "Load state";
		else if (r->item == c->reset_item) l->label = "Reset game";
		else if (r->item == c->exit_item) l->label = "Exit game";
		else l->label = c->label(r->item);
		n++;
	}
	return n;
}
/* what a row shows on its right (English, as the core gives it), or 0; *sw 1/0 for a switch on/off */
static const char *row_value(int page, int row, const cline *l, char *buf, int *sw)
{
	const struct igm_core *c = C.c;
	const char *v = 0;
	*sw = -1;
	buf[0] = 0;
	if (page == IGM_CORE) v = c->opt_value(l->item, buf);
	else if (page == IGM_ACH) return pico_ra_row_value ? pico_ra_row_value(l->item) : 0;
	else if (l->kind == IGM_LINK || l->item < 0) return 0;
	else if ((l->item == c->save_item || l->item == c->load_item) && c->slot_item >= 0) {
		char s[96], w[32], *sp;
		c->value(c->slot_item, s);
		sp = strchr(s, ' ');
		if (sp) {	/* "3 used": the slot, then its word with a capital ("Used") */
			*sp = 0;
			snprintf(w, sizeof w, "%c%s", sp[1] >= 'a' && sp[1] <= 'z' ? sp[1] - 32 : sp[1], sp[1] ? sp + 2 : "");
			snprintf(buf, 96, cui_say(CF_M, "Slot %s \xC2\xB7 %s"), s, cui_say(CF_M, w));
			if (!snes_font_covers(&s_pk, snes_res_font(&s_pk, cui_font(CF_M)), "\xC2\xB7")) {
				char *d = strstr(buf, "\xC2\xB7");   /* a font without the middle dot: a dash */
				if (d) { d[0] = '-'; memmove(d + 1, d + 2, strlen(d + 2) + 1); }
			}
		} else snprintf(buf, 96, cui_say(CF_M, "Slot %s"), s);
		return buf;
	} else if (l->kind == IGM_ACTION) return 0;
	else if (l->kind == IGM_INFO && row < FIGS) {	/* a figure: read at most once a second */
		if (!C.fig[row][0] || C.fig_page != page) c->value(l->item, C.fig[row]);
		v = C.fig[row];
	} else v = c->value(l->item, buf);
	if (v && !strcmp(v, "On")) *sw = 1;
	else if (v && !strcmp(v, "Off")) *sw = 0;
	return v;
}

/* the tab a page is shown under (the core's options under Gameplay when it has no tab of its own) */
static int tab_of(int page)
{
	int i;
	for (i = 0; i < C.ntab; i++) if (C.tab[i] == page) return i;
	for (i = 0; i < C.ntab; i++) if (C.tab[i] == IGM_PLAY) return i;
	return 0;
}
static void tabs_lay_out(void)
{
	const struct igm_core *c = C.c;
	float total = 0, x;
	int i, core = 0;
	C.ntab = 0;
	C.tab[C.ntab++] = IGM_TOP;
	for (i = 0; i < c->nrows; i++) {
		const struct igm_row *r = &c->rows[i];
		if (r->kind != IGM_LINK || r->item >= IGM_NPAGES) continue;
		if (r->item == IGM_CORE) { if (c->opt_n && c->opt_n()) core = 1; continue; }
		if (r->page == IGM_TOP && C.ntab < IGM_NPAGES) C.tab[C.ntab++] = (signed char)r->item;
	}
	if (core && C.ntab < IGM_NPAGES) C.tab[C.ntab++] = IGM_CORE;
	if (ra_on() && C.ntab < IGM_NPAGES) C.tab[C.ntab++] = IGM_ACH;	/* RetroAchievements: the game's set */
	C.tab_sc = 0.8f;
	for (;;) {
		total = 0;
		for (i = 0; i < C.ntab; i++) {
			C.tab_w[i] = (C.tab[i] == IGM_TOP ? 40.0f : cui_text_w(CF_M, C.tab_sc, cui_say(CF_M, k_page[C.tab[i]]))) + 48.0f;
			total += C.tab_w[i];
		}
		total += 12.0f * (C.ntab - 1);
		if (total <= FW - 96 || C.tab_sc < 0.55f) break;
		C.tab_sc -= 0.05f;
	}
	x = 640 - total / 2;
	for (i = 0; i < C.ntab; i++) { C.tab_cx[i] = x + C.tab_w[i] / 2; x += C.tab_w[i] + 12.0f; }
}

int pico_classic_igm_on(void)
{
	unsigned int W, H;
	if (!cui_kit_open(&s_pk, &s_set)) return 0;
	cui_use(&s_pk, s_set, &s_t);
	ayaneo_canvas_dims(&W, &H);
	pico_lang_load(W, H);   /* a game started straight from power on: the menu never read it */
	cui_lang_fonts();        /* the language's letters the fonts lack */
	return 1;
}

void pico_classic_igm_open(const struct igm_core *c)
{
	static uint32_t s_game;
	unsigned int W, H;
	uint32_t h = 2166136261u;
	const char *g = c->game ? c->game : "";
	int i;
	char clean[128];
	C.c = c;
	ayaneo_canvas_dims(&W, &H);
	s_uh = (float)H * 1280.0f / (float)W;
	{	/* the rows a page shows: the sheet as tall as the screen allows */
		float fh = s_uh - 64 > 780 ? 780 : s_uh - 64;
		C.nv = (int)((fh - 258) / ROW_P) + 1;
		if (C.nv < 3) C.nv = 3;
	}
	for (i = 0; g[i] && i < 256; i++) h = (h ^ (uint8_t)g[i]) * 16777619u;
	if (h != s_game) {	/* another game: from the top; the same one: the page and row left */
		s_game = h;
		C.page = C.from = IGM_TOP;
		memset(C.sel, 0, sizeof C.sel);
		memset(C.top, 0, sizeof C.top);
	} else C.from = C.page;
	C.up = 1; C.closing = 0; C.latch = 0; C.show = 0; C.dir = 1; C.page_t = 1;
	C.hy = (float)C.sel[C.page];
	C.nudge_t = 1; C.toast_t = 99; C.toast[0] = 0;
	C.tick = gpt4_get_current_tick(); C.last_paint = 0; C.fig_tick = 0; C.fig_page = -1;
	C.dirty = 1; C.prev_keys = 0xFF;   /* what is held now (the menu key, a face button) is not a press */
	memset(C.rep, 0, sizeof C.rep);
	s_rowh_ok = 0; s_settled = 0;
	{	/* the name without its extension (any core's) and its tags, in the theme's Latin letters */
		char base[128];
		const char *dot = strrchr(g, '.');
		unsigned n = (unsigned)strlen(g);
		if (dot && n - (unsigned)(dot - g) <= 5 && !strchr(dot, ' ')) n = (unsigned)(dot - g);
		if (n > sizeof base - 1) n = sizeof base - 1;
		memcpy(base, g, n);
		base[n] = 0;
		gba_clean_name(base, clean);
		pico_fold_ascii(clean, C.title, sizeof C.title);
	}
	pico_tr_igm_scope(1);
	tabs_lay_out();
	pico_tr_igm_scope(0);
	i = tab_of(C.page);
	C.box_x = C.tab_cx[i]; C.box_w = C.tab_w[i];
}

void pico_classic_igm_dismiss(void)
{
	if (C.up && !C.closing) { C.closing = 1; C.latch = 0; C.dirty = 1; }
}

static void go(int page, int dir)
{
	C.from = C.page; C.page = page; C.dir = dir; C.page_t = 0;
	if (dir > 0) { C.sel[page] = 0; C.top[page] = 0; }
	C.hy = (float)C.sel[page];
	C.nudge_t = 1; C.dirty = 1;
}
/* L or R: the tab beside the page's, each page keeping its row */
static void tab_step(int dir)
{
	int i = (tab_of(C.page) + dir + C.ntab) % C.ntab;
	if (C.ntab < 2) return;
	C.from = C.page; C.page = C.tab[i]; C.dir = dir; C.page_t = 0;
	C.hy = (float)C.sel[C.page];
	C.nudge_t = 1; C.dirty = 1;
}
static int fire(int i, int level, float dt)
{
	int f = 0;
	if (level && !(C.prev_keys & (1 << i))) { f = 1; C.rep[i] = .32f; }
	else if (level) { C.rep[i] -= dt; if (C.rep[i] <= 0) { f = 1; C.rep[i] = .07f; } }
	return f;
}

int pico_classic_igm_frame(unsigned int keys)
{
	cline ln[ROWS];
	unsigned int now = gpt4_get_current_tick();
	float dt = (float)(now - C.tick) / 13000000.0f;
	int n, sel, up, dn, lt, rt, a, b;
	C.tick = now;
	if (dt > .05f) dt = .05f;
	if (!C.up) return 0;
	if (pico_ra_paused) pico_ra_paused();	/* RetroAchievements while the game waits (about once a second) */
	if (g_dbg_key_hold > 0 && g_dbg_key >= 0 && g_dbg_key < 10 && g_dbg_key != 6 && g_dbg_key != 7) {   /* oem nav: an injected key */
		/* LRUDAB, then [ and ] as the shoulders (Select and Start are not the menu's) */
		static const unsigned int map[10] = { IGM_K_LEFT, IGM_K_RIGHT, IGM_K_UP, IGM_K_DOWN, IGM_K_A, IGM_K_B, 0, 0,
						      IGM_K_L, IGM_K_R };
		keys |= map[g_dbg_key];
		g_dbg_key_hold--;
	}
	C.clock += dt;
	if (C.closing) {
		C.show -= dt / CLOSE_S;
		if (C.show <= 0) { C.show = 0; C.up = 0; C.prev_keys = (int)keys; return C.latch; }
	} else if (C.show < 1) { C.show += dt / OPEN_S; if (C.show > 1) C.show = 1; }
	if (C.page_t < 1) { C.page_t += dt / .26f; if (C.page_t > 1) C.page_t = 1; }
	if (C.nudge_t < 1) { C.nudge_t += dt / .18f; if (C.nudge_t > 1) C.nudge_t = 1; }
	if (C.toast_t < 9) C.toast_t += dt;
	if (C.c->status && C.c->status[0]) {
		pico_tr_igm_scope(1);
		snprintf(C.toast, sizeof C.toast, "%s", cui_say(CF_M, C.c->status));
		pico_tr_igm_scope(0);
		C.c->status[0] = 0; C.toast_t = 0; C.dirty = 1;
	}
	n = lines(C.page, ln);
	if (C.sel[C.page] >= n) C.sel[C.page] = n ? n - 1 : 0;
	sel = C.sel[C.page];
	{	/* the list follows the focus, the bar glides to it, the tab box to its tab (the menu's pace) */
		float k = dt * 16, f = k / (1 + k + k * k * .5f) * (1 + k * .5f), want, hy;
		int ti = tab_of(C.page), w = (int)(C.top[C.page] + .5f);   /* a whole row: never stopped between two */
		if (sel < w) w = sel;
		if (sel > w + C.nv - 1) w = sel - C.nv + 1;
		want = (float)(w < 0 ? 0 : w);
		C.top[C.page] += (want - C.top[C.page]) * f;
		if (C.top[C.page] - want < .01f && want - C.top[C.page] < .01f) C.top[C.page] = want;
		hy = (float)sel;
		C.hy += (hy - C.hy) * f;
		if (C.hy - hy < .004f && hy - C.hy < .004f) C.hy = hy;
		C.box_x += (C.tab_cx[ti] - C.box_x) * f; C.box_w += (C.tab_w[ti] - C.box_w) * f;
		if (C.box_x - C.tab_cx[ti] < .3f && C.tab_cx[ti] - C.box_x < .3f) C.box_x = C.tab_cx[ti];
		if (C.box_w - C.tab_w[ti] < .3f && C.tab_w[ti] - C.box_w < .3f) C.box_w = C.tab_w[ti];
	}
	if (C.closing || C.show < .5f) { C.prev_keys = (int)keys; return -1; }
	up = fire(0, !!(keys & IGM_K_UP), dt);
	dn = fire(1, !!(keys & IGM_K_DOWN), dt);
	lt = fire(2, !!(keys & IGM_K_LEFT), dt);
	rt = fire(3, !!(keys & IGM_K_RIGHT), dt);
	a = (keys & IGM_K_A) && !(C.prev_keys & IGM_K_A);
	b = (keys & IGM_K_B) && !(C.prev_keys & IGM_K_B);
	{
		int tl = (keys & IGM_K_L) && !(C.prev_keys & IGM_K_L), tr_ = (keys & IGM_K_R) && !(C.prev_keys & IGM_K_R);
		C.prev_keys = (int)keys;
		if (tl || tr_) { tab_step(tl ? -1 : 1); return -1; }
	}
	if (!n) { if (b) go(IGM_TOP, -1); return -1; }
	if (up) { C.sel[C.page] = (sel + n - 1) % n; C.dirty = 1; }
	if (dn) { C.sel[C.page] = (sel + 1) % n; C.dirty = 1; }
	if (up || dn) return -1;
	{
		cline *l = &ln[sel];
		const struct igm_core *c = C.c;
		int dir = lt ? -1 : rt ? 1 : 0;
		if (C.page == IGM_ACH) { dir = 0; a = 0; }	/* the set is only looked at */
		if (dir) {
			if (C.page == IGM_CORE) c->opt_change(l->item, dir);
			else if (l->kind == IGM_VALUE) { if (c->change(l->item, dir, 0)) { C.closing = 1; C.latch = 0; } }
			else if ((l->item == c->save_item || l->item == c->load_item) && c->slot_item >= 0 && l->item >= 0)
				c->change(c->slot_item, dir, 0);
			else if (l->kind == IGM_LINK && dir > 0) { go(l->item, 1); return -1; }
			else return -1;
			C.nudge_row = sel; C.nudge_dir = dir; C.nudge_t = 0; C.dirty = 1;
		}
		if (a) {
			if (l->kind == IGM_LINK) go(l->item, 1);
			else if (l->item < 0 && C.page == IGM_TOP) { C.closing = 1; C.latch = 1; }   /* Resume */
			else if (C.page != IGM_CORE && (l->kind == IGM_ACTION || l->kind == IGM_VALUE)) {
				if (c->change(l->item, 0, 1)) {
					if (l->item == c->exit_item) { C.up = 0; C.show = 0; return 1; }   /* the game is ending */
					C.closing = 1; C.latch = 1;
				}
				C.nudge_row = sel; C.nudge_dir = 1; C.nudge_t = l->kind == IGM_VALUE ? 0 : 1;
			}
			C.dirty = 1;
		}
		if (b) {
			if (C.page != IGM_TOP) go(C.page == IGM_CORE ? IGM_PLAY : IGM_TOP, -1);
			else { C.closing = 1; C.latch = 2; }
			C.dirty = 1;
		}
	}
	return -1;
}

/* ---- the picture ---- */
static void rows(int page, float y0, float dx, float op, int bar)
{
	cline ln[ROWS];
	char vb[96], fb[128];
	int n = lines(page, ln), i, sel = C.sel[page], first;
	float top = C.top[page];
	if (op <= 0) return;
	if (bar) cui_spr(KS_BAR, 640, y0 + (C.hy - top) * ROW_P, FW - 112, ROW_P - 4, op, 1, 1, 1);
	first = (int)top - 1;
	if (first < 0) first = 0;
	for (i = first; i < n && i <= first + C.nv + 1; i++) {
		float cy = y0 + ((float)i - top) * ROW_P, vx = 1112 + dx, nud = 0, na = 1, vw = 0;
		const cline *l = &ln[i];
		const char *v;
		int sw, f = i == sel;
		float last = y0 + (C.nv - 1) * ROW_P, ro = op;
		/* a row gliding past either end of the list fades as it goes */
		if (cy < y0) ro *= cui_clamp01(1 - (y0 - cy) / 40);
		if (cy > last) ro *= cui_clamp01(1 - (cy - last) / 40);
		if (ro <= 0) continue;
		if (f && i == C.nudge_row && C.nudge_t < 1 && page == C.page) {	/* the value comes in the way it went */
			float e = cui_out_expo(C.nudge_t);
			nud = C.nudge_dir * 22.0f * (1 - e); na = e;
		}
		v = row_value(page, i, l, vb, &sw);
		if (sw >= 0)
			cui_spr_x(sw ? KS_SW_ON : KS_SW_OFF, 1082 + dx, cy, 3, ro);
		else if (v && v[0]) {
			const char *t = cui_fit(CF_M, 1, cui_say(CF_M, v), 420, fb, sizeof fb);
			uint32_t col = l->kind == IGM_INFO ? 0xFFB4B4B4u : 0xFFF4F4F4u;
			vw = cui_text_w(CF_M, 1, t);
			if (f && l->kind != IGM_INFO) {	/* the arrows either side, nudged too */
				float bump = C.nudge_t < 1 ? 6.0f * (1 - cui_out_expo(C.nudge_t)) : 0;
				cui_spr_x(KS_AR_L, vx - vw - 22 - (C.nudge_dir < 0 ? bump : 0), cy, 1.5f, ro);
				cui_spr_x(KS_AR_R, vx + 22 + (C.nudge_dir > 0 ? bump : 0), cy, 1.5f, ro);
			}
			cui_text(CF_M, vx + nud, cy, 1, col, 2, t, ro * na);
		} else if (l->kind == IGM_LINK)
			cui_spr_x(KS_AR_R, vx + 22, cy, 1.5f, ro * (f ? 1 : .5f));
		{
			float maxw = (sw >= 0 ? 1030 : vw > 0 ? 1112 - vw - 52 : 1090) - 160;
			cui_text(CF_M, 160 + dx, cy, 1, 0xFFF4F4F4u, 0, cui_fit(CF_M, 1, cui_say(CF_M, l->label), maxw, fb, sizeof fb), ro);
		}
	}
	if (page == C.page && op >= 1) {	/* more above, more below */
		if (top > .01f) cui_spr_x(KS_UP, 1182, y0 - 6, 1.5f, 1);
		if ((int)(top + .5f) + C.nv < n) cui_spr_x(KS_DOWN, 1182, y0 + (C.nv - 1) * ROW_P + 6, 1.5f, 1);
	}
}

/* the hints at the header's right; where they start back */
static float hints(float y)
{
	static const int ic[3] = { KS_DPAD, KS_BTN_B, KS_BTN_A };
	const char *lab[3];
	lab[0] = cui_say(CF_S, "Select"); lab[1] = cui_say(CF_S, C.page == IGM_TOP ? "Resume" : "Back"); lab[2] = cui_say(CF_S, "OK");
	return cui_hints(1180, y, 3, ic, lab);
}

/* the sheet's height and where its top is now (it slides down from above the screen) */
static float sheet_top(float *fh)
{
	float h = s_uh - 64 > 780 ? 780 : s_uh - 64, t0 = (s_uh - h) / 2;
	float e = C.closing ? 1 - cui_in_expo(1 - C.show) : cui_out_expo(C.show);
	*fh = h;
	return t0 - (t0 + h + 8) * (1 - e);
}
/* the tabs, the cyan box on the page's tab */
static void tabs(float T)
{
	float ty = T + 114;
	int i;
	cui_cursor_box(C.box_x, ty, C.box_w, 60, 1, C.clock);
	if (C.ntab > 1) {	/* L and R step through the tabs */
		cui_text(CF_S, C.tab_cx[0] - C.tab_w[0] / 2 - 34, ty, 1, 0xFF8C8C8Cu, 1, "L", 1);
		cui_text(CF_S, C.tab_cx[C.ntab - 1] + C.tab_w[C.ntab - 1] / 2 + 34, ty, 1, 0xFF8C8C8Cu, 1, "R", 1);
	}
	for (i = 0; i < C.ntab; i++) {
		int on = i == tab_of(C.page);
		if (C.tab[i] == IGM_TOP) cui_spr(KS_IC_RESUME, C.tab_cx[i], ty, 40, 40, on ? 1 : .55f, 1, 1, 1);
		else cui_text(CF_M, C.tab_cx[i], ty, C.tab_sc, on ? 0xFFF4F4F4u : 0xFF8C8C8Cu, 1, cui_say(CF_M, k_page[C.tab[i]]), 1);
	}
}

/* the sheet; rows_only: the list and the note again over what is there (nothing else moved) */
static void sheet(int rows_only)
{
	float fh, T = sheet_top(&fh), k = cui_smooth(C.page_t);
	char tb[128];
	if (T + fh <= 0) return;
	if (rows_only) {
		cui_fill(76, T + 163, 1128, fh - 175, 0xFF000000u);
		goto list;
	}
	cui_frame(64, T, 1216, T + fh);
	{	/* the header: the hints, the page's icon and name, changing with the page */
		int pages[2] = { C.from, C.page }, j;
		float room = hints(T + 38) - 32 - 176;
		for (j = 0; j < 2; j++) {
			int p = pages[j];
			float o = j ? (C.from != C.page ? k : 1) : (C.from != C.page ? 1 - k : 0);
			float dx = j ? C.dir * 40 * (1 - k) : -C.dir * 40 * k;
			const char *name;
			if (o <= 0) continue;
			if (C.from == C.page) dx = 0;
			cui_spr(p == IGM_TOP ? KS_IC_RESUME : p == IGM_DISPLAY ? KS_IC_DISPLAY : KS_IC_OPTION,
			    128 + dx, T + 39, 72, 72, o, 1, 1, 1);
			name = p == IGM_TOP ? C.title : cui_say(CF_L, k_page[p]);
			cui_text(CF_L, 176 + dx, T + 38, 1, 0xFFF4F4F4u, 0, cui_fit(CF_L, 1, name, room, tb, sizeof tb), o);
		}
	}
	cui_spr(KS_LINE, 640, T + 72, 1134, 12, 1, 1, 1, 1);
	tabs(T);
	cui_spr(KS_LINE, 640, T + 156, 1134, 12, 1, 1, 1, 1);
list:
	{	/* the rows: the page, and while it changes the one it came from */
		float y0 = T + 212;
		if (k < 1 && C.from != C.page) {
			rows(C.from, y0, -C.dir * 40 * k, 1 - k, 0);   /* inside the frame: the bar fades in place */
			rows(C.page, y0, C.dir * 40 * (1 - k), k, 1);
		} else rows(C.page, y0, 0, 1, 1);
	}
	if (C.toast[0] && C.toast_t < 2.2f) {	/* a note: up and in, then out */
		float t = C.toast_t, in = cui_out_expo(t / .2f), out = t > 1.9f ? 1 - (t - 1.9f) / .3f : 1, o = in * cui_clamp01(out);
		float tw = cui_text_w(CF_M, .9f, C.toast) + 72, ty = T + fh - 58 + (1 - in) * 12;
		if (o > 0) {
			cui_quad(640 - tw / 2, ty - 26, tw, 52, 0xFF1A1A1Eu, o);
			cui_quad(640 - tw / 2, ty - 26, tw, 3, s_set->line_core, o);
			cui_quad(640 - tw / 2, ty + 23, tw, 3, s_set->line_core, o);
			cui_text(CF_M, 640, ty, .9f, 0xFFF4F4F4u, 1, C.toast, o);
		}
	}
}

/* a row of the picture hashed in four lanes (one chain of multiplies per pixel was most of a paint) */
static uint32_t row_hash(const uint32_t *r, unsigned int w)
{
	uint32_t a = 2166136261u, b = 0x9E3779B9u, c = 0x85EBCA6Bu, d = 0xC2B2AE35u;
	unsigned int x = 0;
	for (; x + 4 <= w; x += 4) {
		a = (a ^ r[x]) * 16777619u; b = (b ^ r[x + 1]) * 16777619u;
		c = (c ^ r[x + 2]) * 16777619u; d = (d ^ r[x + 3]) * 16777619u;
	}
	for (; x < w; x++) a = (a ^ r[x]) * 16777619u;
	return a ^ (b * 3u) ^ (c * 5u) ^ (d * 7u);
}

static int paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H, int *box)
{
	unsigned int now = gpt4_get_current_tick(), y, x, t1, t2;
	int still, y0 = -1, y1 = -1, list_only, sliding;
	uint32_t dim;
	still = C.show >= 1 && !C.closing && C.page_t >= 1 && C.nudge_t >= 1 && C.toast_t > 2.2f &&
		C.hy == (float)C.sel[C.page] && C.top[C.page] == (float)(int)C.top[C.page] &&
		C.box_x == C.tab_cx[tab_of(C.page)] && C.box_w == C.tab_w[tab_of(C.page)];
	if (now - C.fig_tick > 13000000u || C.fig_page != C.page) {	/* the figures: once a second */
		for (x = 0; x < FIGS; x++) C.fig[x][0] = 0;
		C.fig_tick = now; C.fig_page = C.page; C.dirty = 1;
	}
	if (still && !C.dirty && now - C.last_paint < 13000000u / 12u) return 0;
	snes_target_fit(&s_t, buf, (int)pitch, (int)W, (int)H);
	s_t.offx = s_t.offy = 0;
	if (still && !C.dirty && s_settled) {	/* only the box's glow: the tab strip again, nothing else */
		float fh, T = sheet_top(&fh), sc = (float)W / 1280.0f;
		C.last_paint = now;
		cui_fill(76, T + 79, 1128, 70, 0xFF000000u);
		pico_tr_igm_scope(1);
		tabs(T);
		pico_tr_igm_scope(0);
		y0 = (int)((T + 79) * sc); y1 = (int)((T + 149) * sc) + 2;
		if (y0 < 0) y0 = 0;
		if (y1 > (int)H) y1 = (int)H;
		for (y = (unsigned)y0; y < (unsigned)y1 && y < 1024; y++) s_rowh[y] = row_hash(buf + y * pitch, W);
		box[0] = 0; box[1] = y0; box[2] = (int)W; box[3] = y1;
		g_dbg_cigm_us[5] = (gpt4_get_current_tick() - now) / 13u;
		return 1;
	}
	sliding = C.show < 1 || C.closing;
	if (sliding && s_rowh_ok == 0 && (s_slide_skip ^= 1))
		return 0;   /* sliding: every other frame (each one is the whole screen, the game's time) */
	C.dirty = 0; C.last_paint = now;
	/* the header and the tabs as they were: only the list (focus, scroll, values, a note) */
	list_only = s_settled && C.show >= 1 && !C.closing && C.page_t >= 1 &&
		    C.box_x == C.tab_cx[tab_of(C.page)] && C.box_w == C.tab_w[tab_of(C.page)];
	if (!s_settled || C.show < 1 || C.closing) {	/* the game behind, darkened as the sheet comes */
		float d = C.closing ? cui_clamp01(C.show / .7f) : cui_clamp01(C.show / .5f);
		dim = (uint32_t)(DIM * d) << 24;
		for (y = 0; y < H; y++) { uint32_t *r = buf + y * pitch; for (x = 0; x < W; x++) r[x] = dim; }
	}
	t1 = gpt4_get_current_tick();
	pico_tr_igm_scope(1);   /* the cores' rows come out in the language in use */
	sheet(list_only);
	pico_tr_igm_scope(0);
	t2 = gpt4_get_current_tick();
	s_settled = C.show >= 1 && !C.closing;
	if (sliding) {	/* all of it moved: no rows to compare, the whole picture goes */
		s_rowh_ok = 0;
		g_dbg_cigm_us[0] = (gpt4_get_current_tick() - now) / 13u;
		if (g_dbg_cigm_us[0] > g_dbg_cigm_us[1]) g_dbg_cigm_us[1] = g_dbg_cigm_us[0];
		g_dbg_cigm_us[2] = (t1 - now) / 13u; g_dbg_cigm_us[3] = (t2 - t1) / 13u; g_dbg_cigm_us[4] = 0;
		box[0] = 0; box[1] = 0; box[2] = (int)W; box[3] = (int)H;
		return 1;
	}
	{	/* hand on only the rows that came out different (the list's, when only it was drawn) */
		unsigned int ya = 0, yb = H;
		if (list_only) {
			float fh, T = sheet_top(&fh), sc = (float)W / 1280.0f;
			int a = (int)((T + 163) * sc) - 1, b = (int)((T + fh - 12) * sc) + 2;
			ya = a < 0 ? 0 : (unsigned)a; yb = b > (int)H ? H : (unsigned)b;
		}
		for (y = ya; y < yb && y < 1024; y++) {
			uint32_t h = row_hash(buf + y * pitch, W);
			if (!s_rowh_ok || h != s_rowh[y]) { s_rowh[y] = h; if (y0 < 0) y0 = (int)y; y1 = (int)y + 1; }
		}
	}
	s_rowh_ok = 1;
	{
		unsigned int t3 = gpt4_get_current_tick();
		g_dbg_cigm_us[0] = (t3 - now) / 13u;
		if (g_dbg_cigm_us[0] > g_dbg_cigm_us[1]) g_dbg_cigm_us[1] = g_dbg_cigm_us[0];
		g_dbg_cigm_us[2] = (t1 - now) / 13u; g_dbg_cigm_us[3] = (t2 - t1) / 13u; g_dbg_cigm_us[4] = (t3 - t2) / 13u;
	}
	if (y0 < 0) return 0;
	box[0] = 0; box[1] = y0; box[2] = (int)W; box[3] = y1;
	return 1;
}

void pico_classic_igm_overlay(int open)
{
	ayaneo_overlay_live(paint, open && s_set);
}

#else	/* no room kept for a kit on this console: the older panel */
int  pico_classic_igm_on(void) { return 0; }
void pico_classic_igm_open(const struct igm_core *c) { (void)c; }
void pico_classic_igm_dismiss(void) { }
int  pico_classic_igm_frame(unsigned int keys) { (void)keys; return 0; }
void pico_classic_igm_overlay(int open) { (void)open; }
#endif
