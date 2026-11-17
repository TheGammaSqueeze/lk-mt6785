/*
 * pico_coreopt.c - a core's own options for every menu (pico_coreopt.h).
 *
 * Saved per system as /gammaos/coreopt-<sys>.cfg, one "key=value" line per option the user set,
 * with the core's own value names: a core that reorders its values keeps the choice. An option
 * never set stays at the core's default and is not handed to it at all.
 *
 * The file's text is held on the heap only while it is read or written: the KPA LK has about half
 * a megabyte between its static data and the end of its RAM, and its boot needs most of it (16 KB
 * more of static buffers kept it from coming up).
 */
#include <string.h>
#include <printf.h>
#include <malloc.h>
#include "pico_coreopt.h"
#include "gba_sd_save.h"

extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w, int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w, int x, int y, int scale, unsigned int argb, const char *s);
extern int  ayaneo_pico_layout(int count, int H, int sel, int *top, int *rowH, int *panelH);
extern void ayaneo_pico_bar(unsigned int *buf, unsigned int pitch, int x, int w, int y, int rowH);
extern void ayaneo_pico_more(unsigned int *buf, unsigned int pitch, int px, int py, int panelW, int panelH,
			     int up, int down);

#define COPT_MAX	96
#define COPT_FILE_MAX	8192

static const struct pico_coreopt_core *s_core;
static fat_vol *s_vol;
static char s_sys[24];
static int s_cur[COPT_MAX];		/* the value in use, as an index */
static unsigned char s_set[COPT_MAX];	/* chosen by the user (saved); else the core's default */
static int s_page, s_sel, s_top;

static int opt_n(void)
{
	int n = s_core && s_core->count ? s_core->count() : 0;
	return n > COPT_MAX ? COPT_MAX : n;
}
static const struct pico_core_option *opt(int i) { return s_core && s_core->info ? s_core->info(i) : 0; }

static void file_name(char *d, unsigned cap) { snprintf(d, cap, "coreopt-%s", s_sys); }

/* the saved line for key, its value copied to out; 0 when there is none */
static int saved_value(const char *text, const char *key, char *out, unsigned cap)
{
	unsigned kl = (unsigned)strlen(key);
	const char *p = text;
	while (*p) {
		const char *e = p;
		while (*e && *e != '\n') e++;
		if ((unsigned)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			unsigned n = (unsigned)(e - (p + kl + 1));
			if (n >= cap) n = cap - 1;
			memcpy(out, p + kl + 1, n);
			out[n] = 0;
			if (n && out[n - 1] == '\r') out[n - 1] = 0;
			return 1;
		}
		p = *e ? e + 1 : e;
	}
	return 0;
}

static void save(void)
{
	char name[40], *buf;
	int i, n = 0, k = opt_n();
	if (!s_vol || !(buf = malloc(COPT_FILE_MAX))) return;
	for (i = 0; i < k; i++) {
		const struct pico_core_option *o = opt(i);
		if (!o || !s_set[i] || s_cur[i] < 0 || s_cur[i] >= o->nvalues) continue;
		if (n + 160 >= COPT_FILE_MAX) break;
		n += snprintf(buf + n, COPT_FILE_MAX - n, "%s=%s\n", o->key, o->values[s_cur[i]]);
	}
	file_name(name, sizeof name);
	gba_sd_write_named(s_vol, "/gammaos", name, "cfg", buf, (uint32_t)n);
	free(buf);
}

void pico_coreopt_begin(fat_vol *v, const char *sys, const struct pico_coreopt_core *c)
{
	char name[40], val[64], *text;
	uint32_t len = 0;
	int i, k;
	s_core = c;
	s_vol = v;
	snprintf(s_sys, sizeof s_sys, "%s", sys);
	s_page = 0;
	s_sel = s_top = 0;
	file_name(name, sizeof name);
	text = malloc(COPT_FILE_MAX + 1);
	if (!text) return;			/* nothing saved can be read: the core's defaults */
	if (v) len = gba_sd_read_named(v, "/gammaos", name, "cfg", (unsigned char *)text, COPT_FILE_MAX);
	text[len] = 0;
	k = opt_n();
	for (i = 0; i < k; i++) {
		const struct pico_core_option *o = opt(i);
		int j;
		s_set[i] = 0;
		s_cur[i] = o ? o->def : 0;
		if (!o || !saved_value(text, o->key, val, sizeof val)) continue;
		for (j = 0; j < o->nvalues; j++)
			if (!strcmp(o->values[j], val)) {
				s_cur[i] = j;
				s_set[i] = 1;
				if (c->set) c->set(o->key, o->values[j]);
				break;
			}
	}
	free(text);
}

void pico_coreopt_end(void) { s_core = 0; s_page = 0; }

int pico_coreopt_n(void) { return opt_n(); }

const char *pico_coreopt_label(int i)
{
	const struct pico_core_option *o = opt(i);
	return o ? o->label : "";
}

const char *pico_coreopt_value(int i, char *buf)
{
	const struct pico_core_option *o = opt(i);
	if (!o || i >= COPT_MAX || s_cur[i] < 0 || s_cur[i] >= o->nvalues) { buf[0] = 0; return buf; }
	{	/* a core that gives its values no names shows them as they are set: "enabled" / "disabled"
		 * read as On / Off, anything else starts with a capital */
		const char *t = o->labels[s_cur[i]];
		if (!strcmp(t, "enabled")) t = "On";
		else if (!strcmp(t, "disabled")) t = "Off";
		snprintf(buf, 96, "%s", t);
		if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 32);
	}
	return buf;
}

void pico_coreopt_change(int i, int dir)
{
	const struct pico_core_option *o = opt(i);
	if (!o || i >= COPT_MAX || o->nvalues < 2) return;
	s_cur[i] = (s_cur[i] + (dir < 0 ? -1 : 1) + o->nvalues) % o->nvalues;
	s_set[i] = 1;
	if (s_core->set) s_core->set(o->key, o->values[s_cur[i]]);
	save();
}

/* ---- the Classic panel's page: the in-game panel's own look (title band, 2x rows, the bar) ---- */
void pico_coreopt_page_open(void) { s_page = opt_n() > 0; s_sel = s_top = 0; }
int  pico_coreopt_page_is_open(void) { return s_page; }

int pico_coreopt_page_input(unsigned keys)
{
	int n = opt_n();
	if (keys & PICO_COREOPT_K_B) { s_page = 0; return 1; }
	if (!n) { s_page = 0; return 1; }
	if (keys & PICO_COREOPT_K_UP) s_sel = (s_sel + n - 1) % n;
	if (keys & PICO_COREOPT_K_DOWN) s_sel = (s_sel + 1) % n;
	if (keys & PICO_COREOPT_K_LEFT) pico_coreopt_change(s_sel, -1);
	if (keys & PICO_COREOPT_K_RIGHT) pico_coreopt_change(s_sel, 1);
	return 0;
}

/* a label or value in the panel's 8 px wide font, cut to fit max characters */
static void text_fit(unsigned int *buf, unsigned int pitch, int x, int y, unsigned fg, const char *s, int max)
{
	char t[64];
	int n = (int)strlen(s);
	if (max > (int)sizeof t - 1) max = (int)sizeof t - 1;
	if (max < 4) max = 4;
	if (n > max) {
		memcpy(t, s, (unsigned)(max - 2));
		t[max - 2] = '.'; t[max - 1] = '.'; t[max] = 0;
	} else
		snprintf(t, sizeof t, "%s", s);
	ayaneo_text(buf, pitch, x, y, 2, fg, t);
}

void pico_coreopt_page_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int n = opt_n(), rowH, panelH, vis, panelW = (int)W - 80 < 720 ? (int)W - 80 : 720, px, py, x, y, i;
	char val[96];
	if (!n) return;
	vis = ayaneo_pico_layout(n, (int)H, s_sel, &s_top, &rowH, &panelH);
	px = ((int)W - panelW) / 2; py = ((int)H - panelH) / 2;
	x = px + 28; y = py + 60;
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xD010141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 11, 3, 0xFFFFFFFFu, "Core Settings");
	for (i = s_top; i < n && i < s_top + vis; i++, y += rowH) {
		unsigned fg = i == s_sel ? 0xFF101018u : 0xFFC8D0E0u;
		int vw, maxv = 16;
		if (i == s_sel) ayaneo_pico_bar(buf, pitch, px + 10, panelW - 20, y, rowH);
		pico_coreopt_value(i, val);
		vw = (int)strlen(val); if (vw > maxv) vw = maxv;
		text_fit(buf, pitch, x, y, fg, pico_coreopt_label(i), (panelW - 56) / 16 - vw - 2);
		text_fit(buf, pitch, px + panelW - 28 - vw * 16, y, fg, val, maxv);
	}
	ayaneo_pico_more(buf, pitch, px, py, panelW, panelH, s_top > 0, s_top + vis < n);
	ayaneo_text(buf, pitch, x, py + panelH - 14, 1, 0xFF8890A0u, "Up/Down move  Left/Right change  B back");
}
