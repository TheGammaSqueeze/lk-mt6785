/*
 * pico_names.c - game names in any script for the Current theme's menus (pico_names.h).
 *
 * The menu fonts carry ASCII, Latin-1 and what the language in use needs. Game names can hold any
 * character, so the card has /gammaos/names.pack (tools/ayaneo/pico_current/build_names.py): some
 * seventeen thousand more glyphs, drawn once at a master size, 4 bits a pixel, in deflated blocks
 * of 64. The menu reads only the glyphs the names on the card use, and the first time a font of a
 * size asks for them scales them to that size (an area average, both ways) into an atlas of its
 * own; the fonts look there after their own glyphs and the language's. Arabic names are joined with
 * the pack's table of the letters' forms before they are drawn, and names with right to left
 * letters are put in drawing order (pico_bidi).
 *
 * All the memory comes from the library's arena (pico_libmem.c) and is taken once: the pack's
 * index and tables, the glyphs read, and each size's atlas (taken again only when a later prepare
 * read more glyphs, which happens when the library itself changes).
 */
#include <string.h>
#include "pico_names.h"
#include "pico_libmem.h"
#include "pico_i18n.h"
#include "fat_ro.h"

extern fat_vol *gba_sd_menu_vol(void);
extern int zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);

#define NAMES_PATH  "/gammaos/names.pack"
#define NAMES_MAGIC 0x4D414E50u   /* "PNAM" */
#define KEY_MEDIUM  (1u << 24)
#define NG_MAX      8192          /* glyphs read at most, both weights */
#define PIX_CAP     (3u << 20)    /* their pixels, 4 bits each */
#define NSETS       12            /* sizes and weights scaled */
#define ATLAS_W     512

typedef struct { uint32_t key; int8_t xo, yo; uint8_t w, h; uint16_t adv, blk; uint32_t off; } nidx;
typedef struct { uint32_t key; int8_t xo, yo; uint8_t w, h; uint16_t adv; uint16_t pad; uint32_t pix; } mglyph;
typedef struct { uint32_t off, zlen, len; } nblock;
typedef struct { uint16_t letter, iso, fin, ini, med; } njoin;
typedef struct { uint16_t lam, alef, iso, fin, pad[2]; } nlig;
typedef struct { uint32_t key, comp; } ncomp;
typedef struct { int px, medium, n, aw; unsigned serial; pico_nglyph *g; uint8_t *atlas; } nset;

static int s_state;                  /* 0 not opened, 1 open, -1 no usable pack */
static int s_px, s_count, s_nblk, s_njoin, s_nlig, s_ncomp;
static nidx *s_idx;
static nblock *s_blk;
static njoin *s_join;
static nlig *s_lig;
static ncomp *s_comp;
static uint8_t *s_zbuf, *s_raw;      /* a block, deflated and not */
static uint32_t s_zmax, s_rmax;
static mglyph *s_m;                  /* the glyphs read, sorted by key */
static mglyph *s_mnew;               /* a prepare's new ones, before they are merged in */
static int s_mn;
static uint8_t *s_pix;
static uint32_t s_pixn;
static uint32_t *s_want;             /* the keys a prepare collects (a hash set, then a list) */
static unsigned int s_serial = 1;
static nset s_set[NSETS];

unsigned int pico_names_serial(void) { return s_serial; }

/* ---- the pack ---- */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int pack_open(void)
{
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	uint8_t h[56];
	uint32_t io, to, jo, lo, co, i;
	if (s_state) return s_state;
	s_state = -1;
	if (!v || fat_open(v, NAMES_PATH, &f) != 0 || fat_read(&f, 0, h, 56) != 56) return s_state;
	if (rd32(h) != NAMES_MAGIC || rd16(h + 4) != 2) return s_state;
	s_px = rd16(h + 6);
	s_count = (int)rd32(h + 12); s_nblk = (int)rd32(h + 16);
	io = rd32(h + 20); to = rd32(h + 24); jo = rd32(h + 28); s_njoin = (int)rd32(h + 32);
	lo = rd32(h + 36); s_nlig = (int)rd32(h + 40);
	co = rd32(h + 44); s_ncomp = (int)rd32(h + 48);
	if (s_px < 8 || s_count <= 0 || s_nblk <= 0 || s_count > 65535 * 64) return s_state;
	s_idx = (nidx *)pico_lib_alloc((unsigned)s_count * sizeof(nidx));
	s_blk = (nblock *)pico_lib_alloc((unsigned)s_nblk * sizeof(nblock));
	s_join = (njoin *)pico_lib_alloc((unsigned)(s_njoin + 1) * sizeof(njoin));
	s_lig = (nlig *)pico_lib_alloc((unsigned)(s_nlig + 1) * sizeof(nlig));
	s_comp = (ncomp *)pico_lib_alloc((unsigned)(s_ncomp + 1) * sizeof(ncomp));
	if (!s_idx || !s_blk || !s_join || !s_lig || !s_comp) return s_state;
	if (fat_read(&f, io, s_idx, (uint32_t)s_count * 16u) != (uint32_t)s_count * 16u ||
	    fat_read(&f, to, s_blk, (uint32_t)s_nblk * 12u) != (uint32_t)s_nblk * 12u ||
	    (s_njoin && fat_read(&f, jo, s_join, (uint32_t)s_njoin * 10u) != (uint32_t)s_njoin * 10u) ||
	    (s_nlig && fat_read(&f, lo, s_lig, (uint32_t)s_nlig * 12u) != (uint32_t)s_nlig * 12u) ||
	    (s_ncomp && fat_read(&f, co, s_comp, (uint32_t)s_ncomp * 8u) != (uint32_t)s_ncomp * 8u))
		return s_state;
	for (i = 0; i < (uint32_t)s_nblk; i++) {
		if (s_blk[i].zlen > s_zmax) s_zmax = s_blk[i].zlen;
		if (s_blk[i].len > s_rmax) s_rmax = s_blk[i].len;
	}
	s_zbuf = (uint8_t *)pico_lib_alloc(s_zmax + 16u);
	s_raw = (uint8_t *)pico_lib_alloc(s_rmax + 16u);
	s_m = (mglyph *)pico_lib_alloc(NG_MAX * sizeof(mglyph));
	s_mnew = (mglyph *)pico_lib_alloc(NG_MAX * sizeof(mglyph));
	s_want = (uint32_t *)pico_lib_alloc(2u * NG_MAX * sizeof(uint32_t));
	if (!s_zbuf || !s_raw || !s_m || !s_mnew || !s_want) return s_state;
	s_state = 1;
	return s_state;
}

static const nidx *idx_find(uint32_t key)
{
	int lo = 0, hi = s_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (s_idx[mid].key == key) return &s_idx[mid];
		if (s_idx[mid].key < key) lo = mid + 1; else hi = mid - 1;
	}
	return 0;
}
static int m_find(uint32_t key)
{
	int lo = 0, hi = s_mn - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (s_m[mid].key == key) return mid;
		if (s_m[mid].key < key) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

/* ---- UTF-8 ---- */
static uint32_t u8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = *s++;
	int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (c >= 0x80 && more) {
		uint32_t v = c & (0x3Fu >> more);
		int k;
		for (k = 0; k < more && (s[k] & 0xC0) == 0x80; k++) v = v << 6 | (s[k] & 0x3F);
		if (k == more) { c = v; s += more; }
	}
	*ps = (const char *)s;
	return c;
}
static int u8_put(char *o, uint32_t c)
{
	if (c < 0x80) { o[0] = (char)c; return 1; }
	if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
	if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
	o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 0x3F));
	o[2] = (char)(0x80 | (c >> 6 & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
	return 4;
}

/* ---- reading the glyphs a set of names uses ---- */
static void sort_u32(uint32_t *a, int n)
{
	int gap, i, j;
	for (gap = n / 2; gap > 0; gap /= 2)
		for (i = gap; i < n; i++) {
			uint32_t t = a[i];
			for (j = i; j >= gap && a[j - gap] > t; j -= gap) a[j] = a[j - gap];
			a[j] = t;
		}
}

/* the keys of a name's characters past Latin-1 that the pack has and are not read yet, into the
 * hash set; returns 0 when the set is full */
static int want_name(const char *s, int *nw)
{
	while (*s) {
		uint32_t c = u8_next(&s), k;
		int w;
		if (c < 0x100) continue;
		for (w = 0; w < 2; w++) {
			uint32_t key = c | (w ? KEY_MEDIUM : 0u), h = (key * 2654435761u) & (2u * NG_MAX - 1u);
			if (m_find(key) >= 0 || !idx_find(key)) continue;
			for (k = 0; k < 2u * NG_MAX; k++, h = (h + 1u) & (2u * NG_MAX - 1u)) {
				if (s_want[h] == key) break;
				if (!s_want[h]) {
					if (s_mn + *nw >= NG_MAX) return 0;
					s_want[h] = key; (*nw)++;
					break;
				}
			}
		}
	}
	return 1;
}

/* read the glyphs whose keys are listed (n of them) and merge them into s_m */
static void read_glyphs(int n)
{
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	int i, have = -1, add = 0, a, b, k;
	if (!v || fat_open(v, NAMES_PATH, &f) != 0) return;
	if (!s_pix) s_pix = (uint8_t *)pico_lib_alloc(PIX_CAP);
	if (!s_pix) return;
	/* in the pack's order, so each block is read once */
	for (i = 0; i < n; i++) {
		const nidx *e = idx_find(s_want[i]);
		uint32_t bytes;
		if (!e) continue;
		if (e->blk != have) {
			const nblock *bl = &s_blk[e->blk];
			unsigned long zl = bl->zlen;
			have = -1;
			if (bl->zlen > s_zmax || bl->len > s_rmax) continue;
			if (fat_read(&f, bl->off, s_zbuf, bl->zlen) != bl->zlen) continue;
			if (zunzip(s_zbuf, &zl, s_raw, (int)bl->len, 0) != 0) continue;
			have = e->blk;
		}
		bytes = (uint32_t)((e->w + 1) / 2) * e->h;
		if (s_pixn + bytes > PIX_CAP || e->off + bytes > s_blk[e->blk].len) continue;
		memcpy(s_pix + s_pixn, s_raw + e->off, bytes);
		s_mnew[add].key = e->key; s_mnew[add].xo = e->xo; s_mnew[add].yo = e->yo;
		s_mnew[add].w = e->w; s_mnew[add].h = e->h; s_mnew[add].adv = e->adv;
		s_mnew[add].pix = s_pixn;
		s_pixn += bytes;
		add++;
	}
	if (!add) return;
	/* the new ones are in key order already (the list was), so the two runs merge from the top:
	 * every slot written there is past the old ones still to place */
	for (a = s_mn - 1, b = add - 1, k = s_mn + add - 1; b >= 0; k--)
		s_m[k] = (a >= 0 && s_m[a].key > s_mnew[b].key) ? s_m[a--] : s_mnew[b--];
	s_mn += add;
	s_serial++;
}

void pico_names_prepare(const char *names, int stride, int n)
{
	int i, nw = 0, k;
	if (pack_open() != 1) return;
	memset(s_want, 0, 2u * NG_MAX * sizeof(uint32_t));
	for (i = 0; i < n; i++)
		if (!want_name(names + (size_t)i * stride, &nw)) break;
	if (!nw) return;
	/* the set's keys into a list at its start, by key: the pack is in key order */
	for (i = 0, k = 0; i < 2 * NG_MAX; i++) if (s_want[i]) s_want[k++] = s_want[i];
	sort_u32(s_want, k);
	read_glyphs(k);
}

void pico_names_prepare_one(const char *name)
{
	pico_names_prepare(name, 0, 1);
}

/* ---- scaling to a font's size ---- */
/* one row (or column) of n source values to m: each output the average of the source over its
 * span, the partly covered values counted by how much of them is in it (16.16 fixed point) */
static void span_scale(const uint8_t *src, int sstep, int n, uint8_t *dst, int dstep, int m)
{
	int i;
	for (i = 0; i < m; i++) {
		uint32_t a = (uint32_t)(((uint64_t)i * n << 16) / (uint32_t)m);
		uint32_t b = (uint32_t)(((uint64_t)(i + 1) * n << 16) / (uint32_t)m);
		uint64_t acc = 0;
		uint32_t j;
		for (j = a >> 16; j < (b + 0xFFFFu) >> 16 && j < (uint32_t)n; j++) {
			uint32_t lo = a > (j << 16) ? a : (j << 16), hi = b < ((j + 1) << 16) ? b : ((j + 1) << 16);
			acc += (uint64_t)(hi - lo) * src[j * sstep];
		}
		dst[i * dstep] = (uint8_t)((acc + (b - a) / 2) / (b - a));
	}
}

static int div_round(int v, int d) { return v >= 0 ? (v * 2 + d) / (2 * d) : -((-v * 2 + d) / (2 * d)); }

static int scaled_dim(int v, int px) { int d = div_round(v * px, s_px); return v && d < 1 ? 1 : d; }

/* a master glyph, its 4 bit rows widened, scaled to dw x dh into the atlas at (x, y) */
static void glyph_scale(const mglyph *g, int dw, int dh, uint8_t *atlas, int aw, int x, int y, uint8_t *tmp)
{
	uint8_t *wide = tmp, *mid = tmp + 128 * 128;
	const uint8_t *p = s_pix + g->pix;
	int rb = (g->w + 1) / 2, r, c;
	for (r = 0; r < g->h; r++)
		for (c = 0; c < g->w; c++) {
			uint8_t v = p[r * rb + c / 2];
			v = (c & 1) ? (v & 15) : (v >> 4);
			wide[r * g->w + c] = (uint8_t)(v * 17);
		}
	for (r = 0; r < g->h; r++) span_scale(wide + r * g->w, 1, g->w, mid + r * dw, 1, dw);
	for (c = 0; c < dw; c++) span_scale(mid + c, dw, g->h, atlas + (uint32_t)y * aw + x + c, aw, dh);
}

int pico_names_set(int px, int medium, const pico_nglyph **g, const uint8_t **atlas, int *aw)
{
	nset *s = 0;
	int i, n = 0, x = 0, y = 0, shelf = 0, ah;
	uint8_t *tmp;
	if (s_state != 1 || !s_mn || px < 4) return 0;
	for (i = 0; i < NSETS; i++)
		if (s_set[i].px == px && s_set[i].medium == medium) { s = &s_set[i]; break; }
	if (s && s->serial == s_serial) goto done;
	if (!s) {
		for (i = 0; i < NSETS && s_set[i].px; i++) ;
		if (i == NSETS) return 0;
		s = &s_set[i];
		s->px = px; s->medium = medium;
	}
	/* the Regular glyphs are the set's characters; a Medium set takes the Medium one where read */
	for (i = 0; i < s_mn && !(s_m[i].key & KEY_MEDIUM); i++) n++;
	s->g = (pico_nglyph *)pico_lib_alloc((unsigned)n * sizeof(pico_nglyph));
	tmp = (uint8_t *)pico_lib_alloc(2u * 128u * 128u);
	if (!s->g || !tmp) { s->n = 0; s->serial = s_serial; return 0; }
	for (i = 0; i < n; i++) {   /* places in the atlas */
		const mglyph *m = &s_m[i];
		int mi = medium ? m_find(m->key | KEY_MEDIUM) : -1, dw, dh;
		if (mi >= 0) m = &s_m[mi];
		dw = scaled_dim(m->w, px); dh = scaled_dim(m->h, px);
		if (dw > 120) dw = 120;
		if (dh > 120) dh = 120;
		if (x + dw + 1 > ATLAS_W) { x = 0; y += shelf + 1; shelf = 0; }
		s->g[i].code = s_m[i].key;
		s->g[i].xo = (int16_t)div_round(m->xo * px, s_px);
		s->g[i].yo = (int16_t)div_round(m->yo * px, s_px);
		s->g[i].w = (uint16_t)dw; s->g[i].h = (uint16_t)dh;
		s->g[i].x = (uint16_t)x; s->g[i].y = (uint16_t)y;
		s->g[i].adv = (uint32_t)(((uint32_t)m->adv * (uint32_t)px + (uint32_t)s_px / 2) / (uint32_t)s_px);
		x += dw + 1;
		if (dh > shelf) shelf = dh;
	}
	ah = y + shelf + 1;
	s->atlas = (uint8_t *)pico_lib_alloc((unsigned)(ATLAS_W * ah));
	if (!s->atlas) { s->n = 0; s->serial = s_serial; return 0; }
	for (i = 0; i < n; i++) {
		const mglyph *m = &s_m[i];
		int mi = medium ? m_find(m->key | KEY_MEDIUM) : -1;
		if (mi >= 0) m = &s_m[mi];
		if (s->g[i].w && s->g[i].h && m->w && m->h && m->w <= 128 && m->h <= 128)
			glyph_scale(m, s->g[i].w, s->g[i].h, s->atlas, ATLAS_W, s->g[i].x, s->g[i].y, tmp);
	}
	s->n = n; s->aw = ATLAS_W; s->serial = s_serial;
done:
	if (!s->n) return 0;
	*g = s->g; *atlas = s->atlas; *aw = s->aw;
	return s->n;
}

/* ---- a name as it is drawn ---- */
static const njoin *join_of(uint32_t c)
{
	int lo = 0, hi = s_njoin - 1;
	if (c < 0x621 || c > 0x6FF) return 0;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (s_join[mid].letter == c) return &s_join[mid];
		if (s_join[mid].letter < c) lo = mid + 1; else hi = mid - 1;
	}
	return 0;
}
static int is_arabic_mark(uint32_t c) { return (c >= 0x64B && c <= 0x65F) || c == 0x670 || (c >= 0x6D6 && c <= 0x6ED); }

/* a letter and the combining mark after it as one precomposed character, or 0 */
static uint32_t compose(uint32_t a, uint32_t b)
{
	uint32_t key;
	int lo = 0, hi = s_ncomp - 1;
	/* Hangul written as jamo: a leading consonant and a vowel, then a trailing consonant */
	if (a >= 0x1100 && a <= 0x1112 && b >= 0x1161 && b <= 0x1175)
		return 0xAC00 + ((a - 0x1100) * 21 + (b - 0x1161)) * 28;
	if (a >= 0xAC00 && a <= 0xD7A3 && (a - 0xAC00) % 28 == 0 && b >= 0x11A8 && b <= 0x11C2)
		return a + (b - 0x11A7);
	if (a > 0xFFFF || b > 0xFFFF) return 0;
	key = a << 16 | b;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (s_comp[mid].key == key) return s_comp[mid].comp;
		if (s_comp[mid].key < key) lo = mid + 1; else hi = mid - 1;
	}
	return 0;
}

const char *pico_names_label(const char *s, char *out, int cap)
{
	static char built[384];
	uint32_t cp[128];
	const char *p = s;
	int n = 0, rtl = 0, arabic = 0, changed = 0, i, o = 0, have = pack_open() == 1;
	while (*p && n < 128) {
		uint32_t c = u8_next(&p), k;
		/* a mark, or a jamo, after the character it belongs to: one character where Unicode has it */
		if (c >= 0x300 && n && have && (k = compose(cp[n - 1], c)) != 0) { cp[n - 1] = k; changed = 1; continue; }
		if (c >= 0x300 && c <= 0x36F) { changed = 1; continue; }   /* an accent with nothing to sit on */
		if (is_arabic_mark(c)) { changed = 1; continue; }
		if (c >= 0x590 && c <= 0x8FF) { rtl = 1; if (c >= 0x600) arabic = 1; }
		if ((c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) rtl = 1;
		cp[n++] = c;
	}
	if (!rtl && !changed) return s;
	/* each Arabic letter in the form its neighbours ask for: it joins the one before when that one
	 * joins on both sides, the one after when it does itself and the next is a letter */
	for (i = 0; i < n && o + 4 < (int)sizeof built; i++) {
		const njoin *j = arabic && have ? join_of(cp[i]) : 0;
		uint32_t f = cp[i];
		if (j) {
			const njoin *pj = i ? join_of(cp[i - 1]) : 0, *nj = i + 1 < n ? join_of(cp[i + 1]) : 0;
			int prev = pj && pj->ini, next = j->ini && nj;
			if (cp[i] == 0x644 && i + 1 < n) {   /* lam then alef: one ligature */
				int k;
				for (k = 0; k < s_nlig; k++)
					if (s_lig[k].alef == cp[i + 1]) break;
				if (k < s_nlig) {
					o += u8_put(built + o, prev && s_lig[k].fin ? s_lig[k].fin : s_lig[k].iso);
					i++;
					continue;
				}
			}
			if (prev && next && j->med) f = j->med;
			else if (prev && j->fin) f = j->fin;
			else if (next && j->ini) f = j->ini;
			else f = j->iso;
		}
		o += u8_put(built + o, f);
	}
	built[o] = 0;
	if (rtl) return pico_bidi(built, out, cap);
	for (i = 0; i < cap - 1 && built[i]; i++) out[i] = built[i];
	out[i] = 0;
	return out;
}
