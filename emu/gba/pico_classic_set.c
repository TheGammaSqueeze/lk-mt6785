/*
 * pico_classic_set.c - the Classic themes' menubar pages (pico_classic_set.h).
 *
 * Each page is the menu's Options page in shape: the frame, the page's icon and name at its top
 * left with the button hints at its right, a line, then rows: the theme's blue bar under the
 * focused one (gliding as the focus moves and riding along a scroll), the on/off settings as the
 * theme's switches, a value between small arrows that nudges the way it went, a chevron where a
 * row opens a page of its own (which slides in from the side, B sliding it back), and along the
 * bottom the line that says more about the focused setting. Display has the themes' thumbnails
 * under its rows with the cyan box on the one picked; Language is a grid of every language, the
 * one in use with its radio button lit; System's About shows the version and build.
 */
#include <string.h>
#include <stdio.h>
#include "pico_classic_set.h"
#include "pico_classic_ui.h"
#include "pico_settings.h"
#include "pico_i18n.h"
#include "pico_version.h"

#define ROW_P   60.0f            /* the row pitch, as the Options page */
#define FH      648.0f           /* the frame's height: the Options page's */
#define NV      7                /* rows a page shows */
#define THEME_VIS 4              /* thumbnails across */

/* the pages: the four slots' and the three their rows open */
enum { PG_DISPLAY, PG_SETTINGS, PG_LANGUAGE, PG_SYSTEM, PG_BIOS, PG_RESUME, PG_CLOCK, PG_N };
static const signed char k_rows[PG_N][8] = {
	{ PS_BRIGHT, PS_BFI, PS_THEME, -1 },
	{ PS_VOLUME, PS_MUTE_MENU, PS_BIOS, PS_RESUME, PS_BOOT_OS, -1 },
	{ -1 },
	{ PS_CLOCK, PS_ABOUT, -1 },
	{ PS_SKIP_INTRO, PS_INTRO_BG, PS_MUTE_BIOS, PS_INTRO_RESUME, -1 },
	{ PS_RESUME_BOOT, PS_RESUME_OPEN, -1 },
	{ PS_YEAR, PS_MONTH, PS_DAY, PS_HOUR, PS_MINUTE, PS_ZONE, PS_CLOCK12, -1 },
};
static const char *const k_page_name[PG_N] = { "Display", "Settings", "Language", "System", "BIOS boot",
	"Game resume", "Date & Time" };
static const signed char k_page_parent[PG_N] = { -1, -1, -1, -1, PG_SETTINGS, PG_SETTINGS, PG_SYSTEM };
static const signed char k_page_icon[PG_N] = { KS_IC_DISPLAY, KS_IC_OPTION, KS_IC_LANGUAGE, KS_IC_SYSTEM,
	KS_IC_OPTION, KS_IC_OPTION, KS_IC_SYSTEM };
/* the languages' names in English, where a font lacks a name's own letters */
static const char *const k_lang_en[LANG_N] = { "English", "French", "German", "Spanish", "Italian", "Dutch",
	"Portuguese", "Russian", "Japanese", "Korean", "Chinese (Simplified)", "Chinese (Traditional)",
	"Polish", "Turkish", "Arabic", "Hebrew", "Persian" };

static snes_menu *s_m;
static const snes_pack *s_pk;
static struct cui_set s_set;
static int s_ok;
static struct pico_classic_themes s_th;

static struct {
	int page, from, dir;
	float page_t;
	int sel[PG_N];
	float top[PG_N];
	float hy;                   /* the bar, in rows from the list's first */
	int nudge_row, nudge_dir;
	float nudge_t;
	int theme_sel, theme_scroll;
	float clock;
	float rep[4], held[4];
	int req_theme, req_lang;
} P;

static int nrows(int pg) { int n = 0; if (pg == PG_LANGUAGE) return LANG_N; while (n < 8 && k_rows[pg][n] >= 0) n++; return n; }
static int page_of_slot(int slot)
{
	return slot == CSLOT_DISPLAY ? PG_DISPLAY : slot == CSLOT_SETTINGS ? PG_SETTINGS :
	       slot == CSLOT_LANGUAGE ? PG_LANGUAGE : PG_SYSTEM;
}
/* the page a settings row opens */
static int page_of_row(int id)
{
	switch (pico_set_page(id)) {
	case PSP_BIOS:   return PG_BIOS;
	case PSP_RESUME: return PG_RESUME;
	case PSP_CLOCK:  return PG_CLOCK;
	default:         return -1;
	}
}

int pico_classic_set_take_theme(void) { int r = P.req_theme; P.req_theme = -1; return r; }
int pico_classic_set_take_lang(void) { int r = P.req_lang; P.req_lang = -1; return r; }

/* ---- input ---- */
static void go(int pg, int dir)
{
	P.from = P.page; P.page = pg; P.dir = dir; P.page_t = 0;
	if (dir > 0) { P.sel[pg] = 0; P.top[pg] = 0; }
	P.hy = (float)P.sel[pg];
	P.nudge_t = 1;
}
/* a held direction: the press, then again after a pause, then quickly */
static int fire(int i, int held, int edge, float dt)
{
	if (edge) { P.held[i] = 0; P.rep[i] = .32f; return 1; }
	if (!held) return 0;
	P.held[i] += dt;
	P.rep[i] -= dt;
	if (P.rep[i] <= 0) { P.rep[i] = .07f; return 1; }
	return 0;
}
static void page_open(int slot)
{
	int pg = page_of_slot(slot);
	P.page = P.from = pg; P.page_t = 1; P.dir = 1;
	if (pg == PG_LANGUAGE) P.sel[pg] = pico_lang();
	P.hy = (float)P.sel[pg];
	P.nudge_t = 1;
	P.theme_sel = s_th.cur;
	P.theme_scroll = s_th.cur >= THEME_VIS ? s_th.cur - THEME_VIS + 1 : 0;
}
static int page_input(int slot, const snes_input *in, unsigned e, float dt)
{
	int up = fire(0, in->up, !!(e & SNES_E_UP), dt), dn = fire(1, in->down, !!(e & SNES_E_DOWN), dt);
	int lt = fire(2, in->left, !!(e & SNES_E_LEFT), dt), rt = fire(3, in->right, !!(e & SNES_E_RIGHT), dt);
	int a = !!(e & SNES_E_A), b = !!(e & SNES_E_B), n = nrows(P.page), sel = P.sel[P.page], dir = lt ? -1 : rt ? 1 : 0;
	(void)slot;
	if (P.page_t < 1) return 0;
	if (b) {
		if (k_page_parent[P.page] >= 0) {	/* back to the row that opened it */
			int parent = k_page_parent[P.page], i;
			for (i = 0; i < nrows(parent); i++) if (page_of_row(k_rows[parent][i]) == P.page) P.sel[parent] = i;
			go(parent, -1);
			snes_menu_sfx(s_m, 2);
			return 0;
		}
		return 1;   /* the page closes, sliding up */
	}
	if (P.page == PG_LANGUAGE) {	/* a grid three across */
		int s = sel;
		if (lt && s % 3) s--;
		if (rt && s % 3 < 2 && s + 1 < n) s++;
		if (up && s >= 3) s -= 3;
		if (dn && s + 3 < n) s += 3;
		if (s != sel) { P.sel[P.page] = s; snes_menu_sfx(s_m, 0); }
		if (a) {
			if (s != pico_lang()) P.req_lang = s;
			snes_menu_sfx(s_m, 1);
		}
		return 0;
	}
	if (up || dn) {
		P.sel[P.page] = (sel + (up ? n - 1 : 1)) % n;
		snes_menu_sfx(s_m, 0);
		return 0;
	}
	{
		int id = k_rows[P.page][sel], kind = pico_set_kind(id);
		if (kind == PSK_PAGE && (a || rt)) { go(page_of_row(id), 1); snes_menu_sfx(s_m, 1); return 0; }
		if (id == PS_THEME) {
			if (dir && s_th.n > 0) {
				int t = P.theme_sel + dir;
				if (t >= 0 && t < s_th.n) {
					P.theme_sel = t;
					if (t < P.theme_scroll) P.theme_scroll = t;
					if (t >= P.theme_scroll + THEME_VIS) P.theme_scroll = t - THEME_VIS + 1;
					P.nudge_row = sel; P.nudge_dir = dir; P.nudge_t = 0;
					snes_menu_sfx(s_m, 0);
				}
			}
			if (a) {
				if (P.theme_sel != s_th.cur) P.req_theme = P.theme_sel;
				snes_menu_sfx(s_m, 1);
			}
			return 0;
		}
		if ((kind == PSK_STEP || kind == PSK_SWITCH) && pico_set_change(id, dir, a)) {
			P.nudge_row = sel; P.nudge_dir = dir ? dir : 1; P.nudge_t = kind == PSK_STEP ? 0 : 1;
			snes_menu_sfx(s_m, dir ? 0 : 1);
		}
		if (kind == PSK_ACTION && a) { snes_menu_sfx(s_m, 1); pico_set_change(id, 0, 1); }   /* Boot to Android: does not come back */
	}
	return 0;
}

/* ---- the picture ---- */
static void step(float dt)
{
	int sel = P.sel[P.page], w = (int)(P.top[P.page] + .5f);
	float k = dt * 16, f = k / (1 + k + k * k * .5f) * (1 + k * .5f), want;
	P.clock += dt;
	if (P.page_t < 1) { P.page_t += dt / .26f; if (P.page_t > 1) P.page_t = 1; }
	if (P.nudge_t < 1) { P.nudge_t += dt / .18f; if (P.nudge_t > 1) P.nudge_t = 1; }
	if (P.page == PG_LANGUAGE) return;
	if (sel < w) w = sel;   /* the list's goal, a whole row: never stopped between two */
	if (sel > w + NV - 1) w = sel - NV + 1;
	want = (float)(w < 0 ? 0 : w);
	P.top[P.page] += (want - P.top[P.page]) * f;
	if (P.top[P.page] - want < .01f && want - P.top[P.page] < .01f) P.top[P.page] = want;
	P.hy += ((float)sel - P.hy) * f;
	if (P.hy - (float)sel < .004f && (float)sel - P.hy < .004f) P.hy = (float)sel;
}

static void rows(int pg, float y0, float dx, float op, int bar)
{
	char vb[96], fb[128];
	int n = nrows(pg), i, sel = P.sel[pg], first;
	float top = P.top[pg], last = y0 + (NV - 1) * ROW_P;
	if (op <= 0) return;
	if (bar) cui_spr(KS_BAR, 640, y0 + (P.hy - top) * ROW_P, 1040, ROW_P - 4, op, 1, 1, 1);
	first = (int)top - 1;
	if (first < 0) first = 0;
	for (i = first; i < n && i <= first + NV + 1; i++) {
		int id = k_rows[pg][i], kind = pico_set_kind(id), f = i == sel, num, sw;
		float cy = y0 + ((float)i - top) * ROW_P, ro = op, vx = 1112 + dx, vw = 0, nud = 0, na = 1;
		if (cy < y0) ro *= cui_clamp01(1 - (y0 - cy) / 40);
		if (cy > last) ro *= cui_clamp01(1 - (cy - last) / 40);
		if (ro <= 0) continue;
		if (f && i == P.nudge_row && P.nudge_t < 1 && pg == P.page) {
			float e = cui_out_expo(P.nudge_t);
			nud = P.nudge_dir * 22.0f * (1 - e); na = e;
		}
		sw = pico_set_switch(id);
		if (id == PS_INTRO_BG) sw = -1;
		if (sw >= 0) cui_spr_x(sw ? KS_SW_ON : KS_SW_OFF, 1082 + dx, cy, 3, ro);
		else if (kind == PSK_PAGE) cui_spr_x(KS_AR_R, vx + 22, cy, 1.5f, ro * (f ? 1 : .5f));
		else {
			const char *v = vb;
			if (id == PS_THEME) {
				int t = P.theme_sel;
				const char *lab = t >= 0 && t < s_th.n && s_th.label[t] ? s_th.label[t] : "";
				if (t >= 0 && t < s_th.n && ((s_th.beta >> t) & 1u)) snprintf(vb, sizeof vb, cui_say(CF_M, "%s (Beta)"), lab);
				else snprintf(vb, sizeof vb, "%s", lab);
			} else pico_set_value(id, vb, sizeof vb, &num);
			if (v[0]) {
				const char *t = cui_fit(CF_M, 1, v, 420, fb, sizeof fb);
				vw = cui_text_w(CF_M, 1, t);
				if (f && (kind == PSK_STEP || kind == PSK_PICK)) {
					float bump = P.nudge_t < 1 ? 6.0f * (1 - cui_out_expo(P.nudge_t)) : 0;
					cui_spr_x(KS_AR_L, vx - vw - 22 - (P.nudge_dir < 0 ? bump : 0), cy, 1.5f, ro);
					cui_spr_x(KS_AR_R, vx + 22 + (P.nudge_dir > 0 ? bump : 0), cy, 1.5f, ro);
				}
				cui_text(CF_M, vx + nud, cy, 1, kind == PSK_INFO ? 0xFFB4B4B4u : 0xFFF4F4F4u, 2, t, ro * na);
			}
		}
		{
			float maxw = (sw >= 0 ? 1030 : vw > 0 ? 1112 - vw - 52 : 1090) - 160;
			cui_text(CF_M, 160 + dx, cy, 1, 0xFFF4F4F4u, 0, cui_fit(CF_M, 1, cui_say(CF_M, pico_set_name(id)), maxw, fb, sizeof fb), ro);
		}
	}
	if (pg == P.page && op >= 1) {	/* more above, more below */
		if (top > .01f) cui_spr_x(KS_UP, 1182, y0 - 6, 1.5f, 1);
		if ((int)(top + .5f) + NV < n) cui_spr_x(KS_DOWN, 1182, last + 6, 1.5f, 1);
	}
}

/* the themes' thumbnails, the check on the one in use, a Beta tag, the cyan box on the one picked
 * while the Theme row has the focus */
static void themes_strip(float cy, float op, int focused)
{
	int j;
	for (j = 0; j < THEME_VIS; j++) {
		int i = P.theme_scroll + j;
		int vis = s_th.n < THEME_VIS ? s_th.n : THEME_VIS;   /* centred on as many as show */
		float x = 640 + ((float)j - (float)(vis - 1) / 2.0f) * 196;
		if (i >= s_th.n) break;
		cui_tex(s_th.thumb[i], x, cy, 172, 98, op);
		if ((s_th.beta >> i) & 1u) {
			const char *tag = cui_say(CF_S, "Beta");
			cui_quad(x + 5, cy - 45, 78, 28, 0xFFFFBD3Du, .95f * op);
			cui_text(CF_S, x + 44, cy - 31, .8f, 0xFF1B1408u, 1, tag, op);
		}
		if (i == s_th.cur) cui_spr_x(KS_CHECK, x, cy, 3, op);
		if (focused && i == P.theme_sel) cui_cursor_box(x, cy, 184, 110, op, P.clock);
	}
}
/* System > About: the name, its version, the build, and its warning */
static void about(float y, float op)
{
	char t[96];
	cui_text(CF_L, 640, y, 1, 0xFFF4F4F4u, 1, "GammaOS Pico", op);
	snprintf(t, sizeof t, cui_say(CF_M, "Version %s"), pico_version());
	cui_text(CF_M, 640, y + 46, .9f, 0xFFF4F4F4u, 1, t, op);
	snprintf(t, sizeof t, cui_say(CF_S, "Build %s, %s"), pico_commit(), pico_build_day());
	cui_text(CF_S, 640, y + 80, .9f, 0xFFB4B4B4u, 1, t, op);
	cui_text(CF_S, 640, y + 108, .9f, 0xFFB4B4B4u, 1, cui_say(CF_S, "Experimental software, provided as it is, with no warranty"), op);
}

static void language_grid(float y0, float op)
{
	int i, sel = P.sel[PG_LANGUAGE];
	for (i = 0; i < LANG_N; i++) {
		float cx = 640 + (float)(i % 3 - 1) * 360, cy = y0 + (float)(int)(i / 3) * 70;   /* three across */
		const char *nm = pico_lang_name(i);
		if (!snes_font_covers(s_pk, snes_res_font(s_pk, cui_font(CF_M)), nm)) nm = k_lang_en[i];
		if (i == sel) cui_spr(KS_BAR, cx, cy, 344, 56, op, 1, 1, 1);
		cui_spr_x(i == pico_lang() ? KS_RADIO_ON : KS_RADIO_OFF, cx - 140, cy, 2.4f, op);
		cui_text(CF_M, cx - 112, cy, .9f, 0xFFF4F4F4u, 0, nm, op);
	}
}

/* a page's contents (rows, grid, strip, about) at its place, moved dx and faded to op */
static void contents(int pg, float T, float dx, float op, int bar)
{
	float y0 = T + 132;
	if (pg == PG_LANGUAGE) { language_grid(y0, op); return; }
	rows(pg, y0, dx, op, bar);
	if (pg == PG_DISPLAY && s_th.n > 0)
		themes_strip(T + 420, op, P.page == PG_DISPLAY && k_rows[PG_DISPLAY][P.sel[PG_DISPLAY]] == PS_THEME);
	if (pg == PG_SYSTEM) about(T + 330, op);
}

static void page_draw(int slot, snes_target *t, float slide)
{
	static unsigned int last;
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int now = gpt4_get_current_tick();
	float uh = (float)t->H / snes_target_base(t), T = (uh - 720.0f) / 2.0f + 36.0f - slide, k;
	float dt = last ? (float)(now - last) / 13000000.0f : 0;
	char tb[128];
	(void)slot;
	last = now;
	if (dt > .05f) dt = .05f;
	step(dt);
	k = cui_smooth(P.page_t);
	cui_use(s_pk, &s_set, t);
	cui_frame(64, T, 1216, T + FH);
	{	/* the header: the page's icon and name (crossfading with a page change), the hints */
		static const int ic[3] = { KS_DPAD, KS_BTN_B, KS_BTN_A };
		const char *lab[3];
		int pages[2] = { P.from, P.page }, j;
		float room;
		lab[0] = cui_say(CF_S, "Select"); lab[1] = cui_say(CF_S, "Back"); lab[2] = cui_say(CF_S, "OK");
		room = cui_hints(1180, T + 38, 3, ic, lab) - 32 - 176;
		for (j = 0; j < 2; j++) {
			int pg = pages[j];
			float o = j ? (P.from != P.page ? k : 1) : (P.from != P.page ? 1 - k : 0);
			float dx = P.from == P.page ? 0 : j ? P.dir * 40 * (1 - k) : -P.dir * 40 * k;
			if (o <= 0) continue;
			cui_spr(k_page_icon[pg], 128 + dx, T + 39, 72, 72, o, 1, 1, 1);
			cui_text(CF_L, 176 + dx, T + 38, 1, 0xFFF4F4F4u, 0, cui_fit(CF_L, 1, cui_say(CF_L, k_page_name[pg]), room, tb, sizeof tb), o);
		}
	}
	cui_spr(KS_LINE, 640, T + 72, 1134, 12, 1, 1, 1, 1);
	if (k < 1 && P.from != P.page) {
		contents(P.from, T, -P.dir * 40 * k, 1 - k, 0);
		contents(P.page, T, P.dir * 40 * (1 - k), k, 1);
	} else contents(P.page, T, 0, 1, 1);
	if (P.page != PG_LANGUAGE) {	/* what the focused setting is for, along the bottom */
		const char *about_line = pico_set_about(k_rows[P.page][P.sel[P.page]]);
		if (about_line) cui_text(CF_S, 640, T + FH - 34, .9f, 0xFFB4B4B4u, 1, cui_say(CF_S, about_line), k);
	}
}

void pico_classic_set_begin(snes_menu *m, const snes_pack *pk, const struct pico_classic_themes *th)
{
	static const char *const icon[5] = { "btn_link.png", "btn_display.png", "btn_option.png", "btn_language.png",
					     "btn_copyright.png" };
	static const char *const caption[5] = { "Link", "Display", "Settings", "Language", "System" };
	static const snes_target none;
	int i;
	s_m = m; s_pk = pk;
	s_th = *th;
	s_ok = cui_set_from_pack(&s_set, pk);
	P.req_theme = P.req_lang = -1;
	if (!s_ok) return;   /* the pack lacks the frame's parts: the authored pages stay */
	cui_use(pk, &s_set, (snes_target *)&none);   /* for the captions' fonts (nothing is drawn) */
	m->page_open = page_open;
	m->page_input = page_input;
	m->page_draw = page_draw;
	for (i = 0; i < 5; i++) snes_menu_own_slot(m, i, icon[i], cui_say(CF_M, caption[i]), i != CSLOT_LINK);
}
