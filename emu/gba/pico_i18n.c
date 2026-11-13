/*
 * pico_i18n.c - the menus' languages (pico_i18n.h).
 *
 * A language pack is a menu pack (the deflated 'PCUR' container of pico_current.c) with:
 *   "strings"  type 5: u32 count, then count x (u32 hash of the English, u32 offset of the text from
 *              the entry's start), sorted by hash; the texts are UTF-8, each ending in a 0. A word
 *              that changes with a count has its other forms under "<English>#one", "#two", "#few",
 *              "#many" and "#zero".
 *   "f_<role>" type 3: for each menu font, the glyphs this language needs that the font lacks, in
 *              the font entry's own layout (pico_current.c), sorted by code point.
 * Arabic and Persian come with their letters already joined (the presentation forms), in reading
 * order like every text; pico_bidi() puts a text with right-to-left letters in drawing order once it
 * is complete, numbers and Latin words in it still reading left to right.
 */
#include <string.h>
#include <stdio.h>
#include "pico_i18n.h"
#include "fat_ro.h"

extern fat_vol *gba_sd_menu_vol(void);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern void *ayaneo_lang_arena(unsigned int *size);
extern int  ayaneo_get_language(void);
extern void ayaneo_set_language(int l);

#define LANG_MAGIC_Z  0x5A534E53u     /* "SNSZ" */
#define LANG_MAGIC    0x52554350u     /* "PCUR" */
#define LANG_COMP_MAX 0x00200000u     /* the compressed pack, staged at the arena's top */
#define T_FONT        3
#define T_STRINGS     5

static const char *const k_code[LANG_N] = { "en", "fr", "de", "es", "it", "nl", "pt", "ru", "ja", "ko",
					    "zh-cn", "zh-tw", "pl", "tr", "ar", "he", "fa" };
static const char *const k_name[LANG_N] = {
	"English", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1ol", "Italiano", "Nederlands",
	"Portugu\xC3\xAAs", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",
	"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4",
	"\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87",
	"Polski", "T\xC3\xBCrk\xC3\xA7" "e",
	/* the right-to-left ones in reading order, Arabic and Persian with their letters joined */
	"\xEF\xBA\x8D\xEF\xBB\x9F\xEF\xBB\x8C\xEF\xBA\xAE\xEF\xBA\x91\xEF\xBB\xB4\xEF\xBA\x94",
	"\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA",
	"\xEF\xBB\x93\xEF\xBA\x8E\xEF\xBA\xAD\xEF\xBA\xB3\xEF\xAF\xBD",
};

typedef struct { char name[32]; uint16_t type, flags, w, h; uint32_t off, len; } lang_entry;

static struct {
	int lang;                  /* the language whose pack is in memory, -1 none */
	unsigned W, H;
	unsigned serial;
	const uint8_t *raw;
	const lang_entry *ent;
	int nent;
	const uint32_t *str;       /* the strings entry: count, then (hash, offset) pairs */
	uint32_t nstr;
	int igm_scope;
} L = { -1 };

const char *pico_lang_code(int lang) { return k_code[lang >= 0 && lang < LANG_N ? lang : 0]; }
const char *pico_lang_name(int lang) { return k_name[lang >= 0 && lang < LANG_N ? lang : 0]; }
int  pico_lang(void) { int l = ayaneo_get_language(); return l >= 0 && l < LANG_N ? l : LANG_EN; }
void pico_lang_set(int lang) { ayaneo_set_language(lang >= 0 && lang < LANG_N ? lang : LANG_EN); }
unsigned int pico_lang_serial(void) { return L.serial; }
void pico_tr_igm_scope(int on) { L.igm_scope = on; }
const char *tr_igm(const char *en) { return L.igm_scope ? tr(en) : en; }

static uint32_t fnv(const char *s, uint32_t h)
{
	while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

int pico_lang_load(unsigned W, unsigned H)
{
	char path[64];
	fat_vol *v;
	fat_file f;
	unsigned char hdr[12];
	unsigned int size;
	uint32_t rawn, compn, i;
	unsigned long zlen;
	uint8_t *arena;
	const uint32_t *ph;
	int lang = pico_lang();
	if (lang == LANG_EN) {
		if (L.lang != LANG_EN) { L.lang = LANG_EN; L.raw = 0; L.nent = 0; L.str = 0; L.nstr = 0; L.serial++; }
		return 0;
	}
	if (L.lang == lang && L.W == W && L.H == H && L.raw) return 0;
	L.lang = -1; L.raw = 0; L.nent = 0; L.str = 0; L.nstr = 0; L.serial++;
	arena = (uint8_t *)ayaneo_lang_arena(&size);
	v = gba_sd_menu_vol();
	if (!arena || size <= LANG_COMP_MAX || !v) return -1;
	snprintf(path, sizeof path, "/gammaos/lang-%s-%ux%u.pack", k_code[lang], W, H);
	if (fat_open(v, path, &f) != 0) return -1;
	if (fat_read(&f, 0, hdr, 12) != 12) return -2;
	memcpy(&rawn, hdr + 4, 4); memcpy(&compn, hdr + 8, 4);
	if (*(const uint32_t *)hdr != LANG_MAGIC_Z || rawn < 16 || rawn > size - LANG_COMP_MAX || !compn ||
	    compn > LANG_COMP_MAX || 12u + compn > f.size) return -2;
	if (fat_read(&f, 12, arena + size - LANG_COMP_MAX, compn) != compn) return -3;
	zlen = compn;
	if (zunzip(arena + size - LANG_COMP_MAX, &zlen, arena, (int)rawn, 0) != 0) return -4;
	ph = (const uint32_t *)arena;
	if (ph[0] != LANG_MAGIC || ph[1] != 1u || (ph[3] & 0xFFFFu) != W || (ph[3] >> 16) != H ||
	    16u + ph[2] * sizeof(lang_entry) > rawn) return -5;
	L.raw = arena;
	L.ent = (const lang_entry *)(arena + 16);
	L.nent = (int)ph[2];
	for (i = 0; i < (uint32_t)L.nent; i++) {
		if (L.ent[i].off + L.ent[i].len > rawn) { L.raw = 0; L.nent = 0; return -5; }
		if (L.ent[i].type == T_STRINGS && L.ent[i].len >= 4) {
			const uint32_t *t = (const uint32_t *)(arena + L.ent[i].off);
			if (4u + t[0] * 8u <= L.ent[i].len) { L.str = t; L.nstr = t[0]; }
		}
	}
	L.lang = lang; L.W = W; L.H = H;
	return 0;
}

const uint8_t *pico_lang_entry(const char *name, int type, uint32_t *len)
{
	int i;
	if (!L.raw) return 0;
	for (i = 0; i < L.nent; i++)
		if (L.ent[i].type == type && !strncmp(L.ent[i].name, name, 32)) {
			if (len) *len = L.ent[i].len;
			return L.raw + L.ent[i].off;
		}
	return 0;
}

/* the text under a hash, or 0 */
static const char *lookup(uint32_t h)
{
	int lo = 0, hi = (int)L.nstr - 1;
	while (lo <= hi) {
		int m = (lo + hi) >> 1;
		uint32_t k = L.str[1 + 2 * m];
		if (k == h) return (const char *)L.str + L.str[2 + 2 * m];
		if (k < h) lo = m + 1; else hi = m - 1;
	}
	return 0;
}

const char *tr(const char *en)
{
	const char *t;
	if (!en || !en[0] || !L.nstr || L.lang != pico_lang()) return en;
	t = lookup(fnv(en, 2166136261u));
	return t ? t : en;
}

/* the form of a count in a language: the suffix its text sits under, 0 for the plain one */
static const char *plural_form(int lang, int n)
{
	int m10, m100;
	if (n < 0) n = -n;
	m10 = n % 10; m100 = n % 100;
	switch (lang) {
	case LANG_FR: case LANG_PT: return n <= 1 ? "#one" : 0;
	case LANG_RU:
		if (m10 == 1 && m100 != 11) return "#one";
		if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "#few";
		return "#many";
	case LANG_PL:
		if (n == 1) return "#one";
		if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "#few";
		return "#many";
	case LANG_AR:
		if (n == 0) return "#zero";
		if (n == 1) return "#one";
		if (n == 2) return "#two";
		if (m100 >= 3 && m100 <= 10) return "#few";
		if (m100 >= 11) return "#many";
		return 0;
	case LANG_HE: return n == 1 ? "#one" : n == 2 ? "#two" : 0;
	case LANG_FA: return n <= 1 ? "#one" : 0;
	case LANG_JA: case LANG_KO: case LANG_ZH_CN: case LANG_ZH_TW: case LANG_TR: return 0;
	default: return n == 1 ? "#one" : 0;
	}
}

const char *tr_n(int n, const char *one, const char *other)
{
	const char *form, *t;
	if (!L.nstr || L.lang != pico_lang()) return n == 1 ? one : other;
	form = plural_form(L.lang, n);
	if (form && (t = lookup(fnv(form, fnv(other, 2166136261u))))) return t;
	if ((t = lookup(fnv(other, 2166136261u)))) return t;
	return n == 1 ? one : other;
}

uint32_t pico_upper(uint32_t c)
{
	if (c < 0x80) {
		if (c == 'i' && L.lang == LANG_TR && L.nstr) return 0x130;   /* i -> dotted capital I */
		return c >= 'a' && c <= 'z' ? c - 32 : c;
	}
	if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
	if (c == 0xFF) return 0x178;
	if (c == 0x131) return 'I';                                      /* dotless i */
	if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & ~1u;
	if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1u) ? c : c - 1;
	if (c >= 0x430 && c <= 0x44F) return c - 0x20;
	if (c >= 0x450 && c <= 0x45F) return c - 0x50;
	return c;
}

/* ---- right to left ---------------------------------------------------------------------------
 * A short form of the Unicode bidirectional algorithm, enough for one line of menu text: letters
 * of Hebrew and Arabic run right to left; Latin letters, CJK and numbers left to right; a number
 * keeps the separators and percent signs inside it; spaces and punctuation between two runs of
 * the same way go with them, else with the line. The line runs right to left when its first
 * strong letter does. Brackets in a right-to-left run are mirrored. */
enum { BD_L, BD_R, BD_EN, BD_N };
static int bidi_class(uint32_t c)
{
	if ((c >= 0x590 && c <= 0x8FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return BD_R;
	if (c >= '0' && c <= '9') return BD_EN;
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7 &&
	    !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) && !(c >= 0xFF00 && c <= 0xFF0F)))
		return BD_L;
	return BD_N;
}
static uint32_t bidi_mirror(uint32_t c)
{
	switch (c) {
	case '(': return ')'; case ')': return '('; case '[': return ']'; case ']': return '[';
	case '<': return '>'; case '>': return '<'; case '{': return '}'; case '}': return '{';
	}
	return c;
}
static int utf8_put(char *o, uint32_t c)
{
	if (c < 0x80) { o[0] = (char)c; return 1; }
	if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
	o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63));
	return 3;
}
static uint32_t utf8_get(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = s[0];
	if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) { c = ((c & 31) << 6) | (s[1] & 63); *ps += 2; }
	else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		c = ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); *ps += 3;
	} else *ps += 1;
	return c;
}
#define BIDI_MAX 160
const char *pico_bidi(const char *s, char *out, int cap)
{
	uint32_t cp[BIDI_MAX];
	unsigned char cl[BIDI_MAX], lv[BIDI_MAX];
	const char *p = s;
	int n = 0, i, j, rtl = -1, any = 0, base, k, o = 0, top;
	if (!s) return s;
	while (*p && n < BIDI_MAX) {
		cp[n] = utf8_get(&p);
		cl[n] = (unsigned char)bidi_class(cp[n]);
		if (cl[n] == BD_R) any = 1;
		if (rtl < 0 && (cl[n] == BD_R || cl[n] == BD_L)) rtl = cl[n] == BD_R;
		n++;
	}
	if (!any || *p || cap < 4) return s;   /* nothing to turn, or too long to: as it is */
	base = rtl > 0;
	/* a number takes in the separators between its digits and a percent sign or unit sign after it */
	for (i = 1; i + 1 < n; i++)
		if (cl[i] == BD_N && cl[i - 1] == BD_EN && cl[i + 1] == BD_EN &&
		    (cp[i] == ':' || cp[i] == '.' || cp[i] == ',' || cp[i] == '/')) cl[i] = BD_EN;
	for (i = 1; i < n; i++)
		if (cl[i] == BD_N && cl[i - 1] == BD_EN && (cp[i] == '%' || cp[i] == '+')) cl[i] = BD_EN;
	/* levels: right-to-left letters odd, the rest even, numbers inside right-to-left text above them */
	for (i = 0; i < n; i++) {
		if (cl[i] == BD_R) lv[i] = 1;
		else if (cl[i] == BD_L) lv[i] = base ? 2 : 0;
		else if (cl[i] == BD_EN) lv[i] = 2;
		else lv[i] = 255;   /* a neutral, below */
	}
	for (i = 0; i < n; i++) {	/* a number in left-to-right text with no right-to-left letter before it stays put */
		if (cl[i] != BD_EN || base) continue;
		for (j = i - 1; j >= 0 && cl[j] != BD_L && cl[j] != BD_R; j--) ;
		if (j < 0 || cl[j] == BD_L) lv[i] = 0;
	}
	for (i = 0; i < n; i = j) {	/* neutrals: the way of the runs both sides, else the line's */
		int before, after, d;
		if (lv[i] != 255) { j = i + 1; continue; }
		for (j = i; j < n && lv[j] == 255; j++) ;
		before = i > 0 ? (cl[i - 1] == BD_L ? 0 : 1) : base;          /* a number counts as right to left */
		after = j < n ? (cl[j] == BD_L ? 0 : 1) : base;
		if (i > 0 && cl[i - 1] == BD_EN && !base && lv[i - 1] == 0) before = 0;
		if (j < n && cl[j] == BD_EN && !base && lv[j] == 0) after = 0;
		d = before == after ? before : base;
		for (k = i; k < j; k++) lv[k] = (unsigned char)(d ? 1 : base ? 2 : 0);
	}
	/* reverse each run at or above a level, from the highest down to 1 */
	for (top = 2; top >= 1; top--)
		for (i = 0; i < n; ) {
			if (lv[i] < top) { i++; continue; }
			for (j = i; j < n && lv[j] >= top; j++) ;
			for (k = 0; k < (j - i) / 2; k++) {
				uint32_t t = cp[i + k]; unsigned char u = lv[i + k];
				cp[i + k] = cp[j - 1 - k]; cp[j - 1 - k] = t;
				lv[i + k] = lv[j - 1 - k]; lv[j - 1 - k] = u;
			}
			i = j;
		}
	for (i = 0; i < n && o + 4 < cap; i++) o += utf8_put(out + o, (lv[i] & 1) ? bidi_mirror(cp[i]) : cp[i]);
	out[o] = 0;
	return out;
}
