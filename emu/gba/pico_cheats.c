/*
 * pico_cheats.c - cheat codes for every core, and the in-game menu's Cheats page (pico_cheats.h).
 *
 * Memory: a fixed window of its own (ayaneo_cheats_arena, 2 MB), filled from its start for each game
 * and forgotten at the next: the pack's tables while the game is looked up, the inflater, the game's
 * cheats as text and a row for each. The LK heap is left alone (the Advance has little of it).
 *
 * A pack (tools/cheats/build_cheats.py): "GCD1", a version, then the count and place of its games
 * (where each one's text is, packed and plain), of its dumps (CRC-32, game) and of its names (FNV-1a
 * of a name key, game), the last two sorted. A game's text, raw deflate, is "K<key>" lines, then for
 * each cheat file a "#<format>" line and its cheats, "C<description>\t<code>" a line.
 */
#include "pico_cheats.h"
#include "gba_sd_save.h"
#include "sd_fat.h"
#include <lib/zlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void *ayaneo_cheats_arena(unsigned int *size);

#define CH_DIR      "/gammaos/cheats"
#define CH_SHOWN    95u        /* a list of up to this many rows is shown whole */
#define CH_PART     90u        /* a longer one in parts of this many, a row to step through them */
#define CH_USER_CAP (256u << 10)
#define CH_ON_CAP   (64u << 10)
#define CH_TEXT_CAP (1u << 20) /* a game's text in a pack, at most (the largest is 70 KB) */
#define CH_TABLE_CAP (512u << 10)

struct ch_row {
	const char *title;   /* a cheat's description, or a group's name */
	const char *code;    /* 0 for a group's heading */
	unsigned char on, bad, user;
};

static struct {
	fat_vol *v;
	int system, live;
	struct pico_cheat_core core;
	char game[256];
	char dir[32];            /* CH_DIR "/<pack>" */
	uint8_t *mem;
	unsigned cap, used;
	struct ch_row *row;
	int n, cheats, on, part;
	unsigned crc;            /* the dump's CRC-32 (for oem cheats) */
	int found;               /* 1 by CRC, 2 by name, 0 not in the pack */
	char shortv[32], partv[48];
} K;

/* ---- the window ---- */
static void *ch_alloc(unsigned n)
{
	void *p;
	n = (n + 7u) & ~7u;
	if (!K.mem || n > K.cap - K.used) return 0;
	p = K.mem + K.used;
	K.used += n;
	return p;
}
static voidpf ch_zalloc(voidpf opaque, uInt items, uInt size) { (void)opaque; return ch_alloc(items * size); }
static void ch_zfree(voidpf opaque, voidpf p) { (void)opaque; (void)p; }   /* all of it goes with the game */

/* ---- names ---- */
static const char *pack_of(int system)
{
	switch (system) {
	case GBA_CONSOLE_GB: return "gb";
	case GBA_CONSOLE_GBC: return "gbc";
	case GBA_CONSOLE_GBA: return "gba";
	case GBA_CONSOLE_SNES: return "snes";
	case GBA_CONSOLE_GENESIS: return "md";
	case GBA_CONSOLE_SMS: return "sms";
	case GBA_CONSOLE_GG: return "gg";
	case GBA_CONSOLE_NES: return "nes";
	default: return 0;
	}
}
/* the name key both sides compare (build_cheats.py norm): ASCII letters lower case and digits, the
 * bytes past ASCII as they are, the rest dropped; the file's extension first cut off */
static int name_key(const char *name, char *out, int cap)
{
	const char *dot = strrchr(name, '.');
	int n = 0;
	const char *p;
	for (p = name; *p && (!dot || p < dot) && n + 1 < cap; p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 'A' && c <= 'Z') out[n++] = (char)(c + 32);
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) out[n++] = (char)c;
	}
	out[n] = 0;
	return n;
}
static uint32_t fnv1a(const char *s)
{
	uint32_t h = 0x811C9DC5u;
	while (*s) h = (h ^ (uint8_t)*s++) * 0x01000193u;
	return h;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- the pack ---- */
/* the record whose key is k in a sorted table of n (key, record) pairs, or -1 */
static int table_find(fat_file *f, uint32_t off, uint32_t n, uint32_t k, int *count_same)
{
	uint8_t *t;
	uint32_t lo = 0, hi = n;
	unsigned mark = K.used;
	int r = -1;
	*count_same = 0;
	if (!n || n * 8u > CH_TABLE_CAP || !(t = ch_alloc(n * 8u))) return -1;
	if (fat_read(f, off, t, n * 8u) != n * 8u) { K.used = mark; return -1; }
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (rd32(t + mid * 8u) < k) lo = mid + 1; else hi = mid;
	}
	if (lo < n && rd32(t + lo * 8u) == k) {
		r = (int)rd32(t + lo * 8u + 4);
		while (lo + (uint32_t)*count_same < n && rd32(t + (lo + (uint32_t)*count_same) * 8u) == k) (*count_same)++;
	}
	K.used = mark;
	return r;
}
/* game g's text, plain and ended by a 0, or 0 */
static char *record_text(fat_file *f, uint32_t games_off, uint32_t ngames, int g, unsigned *len)
{
	uint8_t e[12], *packed;
	char *text;
	uint32_t at, csize, usize;
	z_stream s;
	int rc;
	if (g < 0 || (uint32_t)g >= ngames) return 0;
	if (fat_read(f, games_off + (uint32_t)g * 12u, e, 12) != 12) return 0;
	at = rd32(e); csize = rd32(e + 4); usize = rd32(e + 8);
	if (!csize || usize > CH_TEXT_CAP || csize > CH_TEXT_CAP) return 0;
	if (!(packed = ch_alloc(csize)) || !(text = ch_alloc(usize + 1))) return 0;
	if (fat_read(f, at, packed, csize) != csize) return 0;
	memset(&s, 0, sizeof s);
	s.zalloc = ch_zalloc; s.zfree = ch_zfree;
	if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return 0;
	s.next_in = packed; s.avail_in = csize;
	s.next_out = (Bytef *)text; s.avail_out = usize;
	rc = inflate(&s, Z_FINISH);
	inflateEnd(&s);
	if (rc != Z_STREAM_END || s.total_out != usize) return 0;
	text[usize] = 0;
	*len = usize;
	return text;
}
/* the text names this key among its K lines */
static int record_has_key(const char *text, const char *key)
{
	const char *p = text;
	size_t kn = strlen(key);
	while (*p == 'K') {
		const char *e = strchr(p, '\n');
		if (!e) return 0;
		if ((size_t)(e - p - 1) == kn && !memcmp(p + 1, key, kn)) return 1;
		p = e + 1;
	}
	return 0;
}

/* ---- the rows ---- */
static int s_rowcap;
static struct ch_row *row_new(void)
{
	struct ch_row *r;
	if (!K.row || K.n >= s_rowcap) return 0;
	r = &K.row[K.n++];
	memset(r, 0, sizeof *r);
	return r;
}
static int count_lines(const char *t)
{
	int n = 1;
	for (; *t; t++) n += *t == '\n';
	return n;
}
/* a pack's text into rows: a heading for each cheat file unless it is the only one and has no
 * format, then its cheats */
static void rows_from_pack(char *text)
{
	char *p = text, *e;
	int groups = 0;
	if (*p == '#') groups++;
	for (e = text; (e = strstr(e, "\n#")) != 0; e++) groups++;
	while (*p) {
		e = strchr(p, '\n');
		if (e) *e = 0;
		if (*p == '#') {
			if (groups > 1 || p[1]) {
				struct ch_row *r = row_new();
				if (!r) return;
				r->title = p[1] ? p + 1 : "Cheats";
			}
		} else if (*p == 'C') {
			char *tab = strchr(p, '\t');
			if (tab && tab[1]) {
				struct ch_row *r = row_new();
				if (!r) return;
				*tab = 0;
				r->title = p + 1; r->code = tab + 1;
				K.cheats++;
			}
		}
		if (!e) break;
		p = e + 1;
	}
}
/* a .cht line "cheat<N>_<field> = <value>": N (or -1), the field and the value, the value's quotes
 * off (written over in place when cut) */
static int cht_line(char *line, char **field, char **value, int cut)
{
	char *q = line, *eq, *v, *end;
	int idx = 0;
	while (*q == ' ' || *q == '\t') q++;
	if (strncmp(q, "cheat", 5) || q[5] < '0' || q[5] > '9') return -1;
	for (q += 5; *q >= '0' && *q <= '9'; q++) if (idx < 100000) idx = idx * 10 + (*q - '0');
	if (*q != '_' || !(eq = strchr(q, '='))) return -1;
	*field = q + 1;
	if (!cut) return idx;
	end = eq;
	while (end > *field && (end[-1] == ' ' || end[-1] == '\t')) end--;
	*end = 0;
	v = eq + 1;
	while (*v == ' ' || *v == '\t') v++;
	if (*v == '"') v++;
	end = v + strlen(v);
	while (end > v && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t' || end[-1] == '"')) end--;
	*end = 0;
	*value = v;
	return idx;
}
/* the highest cheat number in a .cht text (-1 none), the text left as it is */
static int cht_top(const char *text)
{
	enum { MAXN = 1000 };
	const char *p = text;
	int top = -1;
	while (*p) {
		const char *e = strchr(p, '\n');
		char line[16], *field, *value;
		size_t n = e ? (size_t)(e - p) : strlen(p);
		int idx;
		if (n > sizeof line - 2) n = sizeof line - 2;   /* room for the "=" and the 0 */
		memcpy(line, p, n); line[n] = 0;
		strcat(line, "=");   /* only the number is wanted here */
		idx = cht_line(line, &field, &value, 0);
		if (idx >= 0 && idx < MAXN && idx > top) top = idx;
		if (!e) break;
		p = e + 1;
	}
	return top;
}
/* the player's own .cht: RetroArch's form, cheatN_desc / cheatN_code / cheatN_enable */
static void rows_from_cht(char *text, int top)
{
	struct ch_row *first, *h;
	char *p, *e;
	int i;
	if (top < 0 || !(h = row_new())) return;
	h->title = "Your cheats";
	first = &K.row[K.n];
	for (i = 0; i <= top; i++) if (!row_new()) return;
	for (i = 0; i <= top; i++) first[i].user = 1;
	for (p = text; *p; p = e + 1) {
		char *field, *value;
		int idx;
		e = strchr(p, '\n');
		if (e) *e = 0;
		idx = cht_line(p, &field, &value, 1);
		if (idx >= 0 && idx <= top) {
			struct ch_row *r = &first[idx];
			if (!strcmp(field, "desc")) r->title = value;
			else if (!strcmp(field, "code")) r->code = value;
			else if (!strcmp(field, "enable")) r->on = !strcmp(value, "true");
		}
		if (!e) break;
	}
	/* a number with no code is no cheat: its row stays, with nothing in it, and is passed over */
	for (i = 0; i <= top; i++) {
		struct ch_row *r = &first[i];
		if (r->code && r->code[0]) { if (!r->title || !r->title[0]) r->title = r->code; K.cheats++; }
		else { r->code = 0; r->title = 0; r->on = 0; }
	}
}
/* drop the rows with nothing in them (numbers a .cht skipped) */
static void rows_pack(void)
{
	int i, n = 0;
	for (i = 0; i < K.n; i++) if (K.row[i].title) K.row[n++] = K.row[i];
	K.n = n;
}

/* ---- in force ---- */
static void apply(void)
{
	int i;
	K.on = 0;
	if (!K.live) return;
	K.core.reset();
	for (i = 0; i < K.n; i++) {
		struct ch_row *r = &K.row[i];
		if (!r->code || !r->on) continue;
		if (K.core.add(r->code) < 0) { r->bad = 1; r->on = 0; }
		else K.on++;
	}
	/* a code refused after others may have left part of its text in force: once more without it */
	for (i = 0; i < K.n; i++) if (K.row[i].bad && K.row[i].code) break;
	if (i < K.n) {
		K.core.reset();
		K.on = 0;
		for (i = 0; i < K.n; i++)
			if (K.row[i].code && K.row[i].on && K.core.add(K.row[i].code) == 0) K.on++;
	}
}
/* the codes on, a line each, to the game's .on file (off the game's thread) */
static void save_on(void)
{
	char *buf;
	unsigned mark = K.used, len = 0;
	int i;
	if (!K.v || !(buf = ch_alloc(CH_ON_CAP))) return;
	for (i = 0; i < K.n; i++) {
		size_t n;
		if (!K.row[i].code || !K.row[i].on) continue;
		n = strlen(K.row[i].code);
		if (len + n + 2 > CH_ON_CAP) break;
		memcpy(buf + len, K.row[i].code, n);
		len += (unsigned)n;
		buf[len++] = '\n';
	}
	gba_sd_write_named_bg(K.v, K.dir, K.game, "on", buf, len);
	K.used = mark;
}
/* the .on file read back: those codes on (the player's .cht's own enable flags give way to it) */
static void load_on(void)
{
	char *buf, *p, *e;
	unsigned n, mark = K.used;
	int i;
	if (!(buf = ch_alloc(CH_ON_CAP + 1))) return;
	n = gba_sd_read_named(K.v, K.dir, K.game, "on", (unsigned char *)buf, CH_ON_CAP);
	if (!n) { K.used = mark; return; }   /* never saved: the .cht's flags stand */
	buf[n] = 0;
	for (i = 0; i < K.n; i++) K.row[i].on = 0;
	for (p = buf; *p; p = e + 1) {
		e = strchr(p, '\n');
		if (!e) e = p + strlen(p);
		if (e > p && e[-1] == '\r') e[-1] = 0;
		*e = 0;
		for (i = 0; i < K.n; i++)
			if (K.row[i].code && !strcmp(K.row[i].code, p)) { K.row[i].on = 1; break; }
		if (e == buf + n) break;
	}
	K.used = mark;
}

/* ---- the game's list ----
 * The cheats come from one list at a time: the one found for the game (its dump's CRC-32 in the pack,
 * else its name, plus a .cht named as the game), a pack title the player picked, a .cht file the player
 * picked, or none. A pick is kept in <game>.use ("pack<TAB>title", "file<TAB>name" or "none"). */
enum { LIST_AUTO, LIST_PACK, LIST_FILE, LIST_NONE };
static struct {
	int kind, rec;               /* the list in force (rec: the pack's record for LIST_PACK) */
	char name[160];              /* its title, or the file's name */
	unsigned base;               /* the window's use before any list (what stays across lists) */
	int has_pack;                /* the system's pack is on the card */
	uint32_t hd[10];             /* its header */
	char pack[48];               /* its path */
	uint32_t crcs[3];
	int ncrc;
	char key[256];
	/* the chooser */
	int choosing, cpart, cn;
	/* the chooser's search: the letters typed, the letter Left / Right offer next, and the entries
	 * that contain the letters (all of them while nothing is typed) */
	char query[32];
	int cand;
	int *vis, vn;
	struct ch_pick { const char *title; int kind, rec; } *pick;
	unsigned cmark;
} L;

static int pack_open(fat_file *f)
{
	return L.has_pack && fat_open(K.v, L.pack, f) == 0 ? 0 : -1;
}
/* the pack's title table and the names after it, read into the window in one go: nt entries of
 * (place of a name, record) at *tab, the names at *names from file offset *lo (got bytes) */
static int titles_read(fat_file *f, uint8_t **tab, char **names, uint32_t *lo, uint32_t *got)
{
	uint32_t nt = L.hd[8], to = L.hd[9], k, hi = 0;
	*tab = 0; *names = 0; *got = 0;
	if (L.hd[1] < 2 || !nt || nt > 65536u) return -1;
	if (!(*tab = ch_alloc(nt * 8u)) || fat_read(f, to, *tab, nt * 8u) != nt * 8u) return -1;
	*lo = rd32(*tab);
	for (k = 0; k < nt; k++) {
		uint32_t a = rd32(*tab + k * 8u);
		if (a < *lo) *lo = a;
		if (a > hi) hi = a;
	}
	hi += 256;   /* the last name, at most this long */
	if (hi - *lo > CH_TABLE_CAP || !(*names = ch_alloc(hi - *lo + 1))) return -1;
	*got = fat_read(f, *lo, *names, hi - *lo);
	(*names)[*got] = 0;
	return 0;
}
/* the record the pack's title table gives this title (-1 none); what it read is given back */
static int title_record(fat_file *f, const char *title)
{
	unsigned mark = K.used;
	uint8_t *tab;
	char *names;
	uint32_t lo, got, k;
	int r = -1;
	if (titles_read(f, &tab, &names, &lo, &got) == 0)
		for (k = 0; k < L.hd[8] && r < 0; k++) {
			uint32_t a = rd32(tab + k * 8u) - lo;
			if (a < got && !strcmp(names + a, title)) r = (int)rd32(tab + k * 8u + 4);
		}
	K.used = mark;
	return r;
}
/* the rows of one list (the window from L.base on): 0 when it has no cheats */
static int load_list(int kind, const char *name)
{
	fat_file f;
	char *pack_text = 0, *user_text = 0;
	unsigned len;
	int g = -1, same = 0, i;
	K.used = L.base;
	K.row = 0; K.n = K.cheats = K.on = K.part = 0; K.found = 0;
	L.kind = kind; L.rec = -1; L.name[0] = 0;
	if (kind == LIST_NONE) { K.core.reset(); return 0; }
	if ((kind == LIST_AUTO || kind == LIST_PACK) && pack_open(&f) == 0) {
		uint32_t ng = L.hd[2], go = L.hd[3], nc = L.hd[4], co = L.hd[5], nn = L.hd[6], no = L.hd[7];
		if (kind == LIST_AUTO) {
			for (i = 0; i < L.ncrc && g < 0; i++) g = table_find(&f, co, nc, L.crcs[i], &same);
			if (g >= 0) { K.found = 1; pack_text = record_text(&f, go, ng, g, &len); }
			if (!pack_text && L.key[0]) {	/* a dump no list has: by its name, checked against the keys */
				g = table_find(&f, no, nn, fnv1a(L.key), &same);
				if (g >= 0) {
					unsigned mark = K.used;
					pack_text = record_text(&f, go, ng, g, &len);
					if (pack_text && !record_has_key(pack_text, L.key)) { pack_text = 0; K.used = mark; g = -1; }
					if (pack_text) K.found = 2;
				}
			}
		} else {
			g = title_record(&f, name);
			if (g >= 0) { pack_text = record_text(&f, go, ng, g, &len); K.found = 3; }
		}
		if (pack_text) L.rec = g;
	}
	if (kind == LIST_AUTO || kind == LIST_FILE) {	/* the player's own: read last, cut to what it holds */
		unsigned mark = K.used;
		char *buf = ch_alloc(CH_USER_CAP + 2);
		char fname[200];
		unsigned n;
		/* a file's name given with its extension, so a dot in the name is not taken for one */
		snprintf(fname, sizeof fname, "%s.cht", name);
		n = buf ? gba_sd_read_named(K.v, K.dir, kind == LIST_FILE ? fname : K.game, "cht",
					    (unsigned char *)buf, CH_USER_CAP) : 0;
		if (n) { buf[n] = 0; buf[n + 1] = 0; user_text = buf; K.used = mark + ((n + 2 + 7) & ~7u); }
		else K.used = mark;
	}
	if (kind != LIST_AUTO) snprintf(L.name, sizeof L.name, "%s", name);
	{	/* a row for each line of the pack's text, for each number of the .cht and their headings */
		int user_top = user_text ? cht_top(user_text) : -1;
		s_rowcap = (pack_text ? count_lines(pack_text) : 0) + user_top + 2;
		K.row = s_rowcap > 0 ? ch_alloc((unsigned)s_rowcap * sizeof(struct ch_row)) : 0;
		if (!K.row) { K.n = 0; K.core.reset(); return 0; }
		if (pack_text) rows_from_pack(pack_text);
		if (user_text) rows_from_cht(user_text, user_top);
		rows_pack();
	}
	load_on();
	apply();
	return K.cheats;
}
/* the list kept for the game (<game>.use), else the one found for it */
static void load_chosen(void)
{
	char b[200];
	unsigned n = gba_sd_read_named(K.v, K.dir, K.game, "use", (unsigned char *)b, sizeof b - 1);
	char *tab;
	b[n] = 0;
	while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
	tab = strchr(b, '\t');
	if (n && !strcmp(b, "none")) { load_list(LIST_NONE, ""); return; }
	if (n && tab) {
		*tab = 0;
		if (!strcmp(b, "pack")) { load_list(LIST_PACK, tab + 1); return; }
		if (!strcmp(b, "file")) { load_list(LIST_FILE, tab + 1); return; }
	}
	load_list(LIST_AUTO, "");
}
static void save_chosen(void)
{
	char b[200];
	int n;
	if (L.kind == LIST_AUTO) n = 0;   /* an empty file: the list found for the game */
	else if (L.kind == LIST_NONE) n = snprintf(b, sizeof b, "none\n");
	else n = snprintf(b, sizeof b, "%s\t%s\n", L.kind == LIST_PACK ? "pack" : "file", L.name);
	gba_sd_write_named_bg(K.v, K.dir, K.game, "use", b, (uint32_t)(n < 0 ? 0 : n));
}

/* the game ends: its core goes with it (never called again from here), and so do its codes */
void pico_cheats_game_end(void)
{
	memset(&K, 0, sizeof K);
	memset(&L, 0, sizeof L);
}

volatile unsigned g_cheats_dbg_us;   /* oem cheats: how long the last game's look up took */

void pico_cheats_game(fat_vol *v, int system, const char *game, const void *rom, unsigned size,
		      const struct pico_cheat_core *core)
{
	extern unsigned int gpt4_get_current_tick(void);
	unsigned t0 = gpt4_get_current_tick();
	const char *pk = pack_of(system);
	fat_file f;
	uint8_t hd[40];
	pico_cheats_game_end();
	if (!core || !core->reset || !core->add) return;
	core->reset();   /* nothing of an earlier game in force, whatever this one has */
	if (!v || !pk || !game) return;
	K.mem = ayaneo_cheats_arena(&K.cap);
	if (!K.mem) return;
	K.v = v; K.system = system; K.core = *core;
	snprintf(K.game, sizeof K.game, "%s", game);
	snprintf(K.dir, sizeof K.dir, CH_DIR "/%s", pk);
	snprintf(L.pack, sizeof L.pack, CH_DIR "/%s.gcd", pk);
	name_key(game, L.key, sizeof L.key);
	if (fat_open(v, L.pack, &f) == 0 && fat_read(&f, 0, hd, 40) == 40 && !memcmp(hd, "GCD1", 4) &&
	    (rd32(hd + 4) == 1 || rd32(hd + 4) == 2)) {
		const uint8_t *b = (const uint8_t *)rom;
		int i;
		for (i = 0; i < 10; i++) L.hd[i] = rd32(hd + 4 * i);
		if (L.hd[1] < 2) L.hd[8] = L.hd[9] = 0;   /* version 1: no titles */
		L.has_pack = 1;
		/* the dump as the core has it; one with a copier's or iNES header also without it */
		if (b && size) {
			L.crcs[L.ncrc++] = (uint32_t)crc32(0L, b, size);
			if (size > 16 && b[0] == 'N' && b[1] == 'E' && b[2] == 'S' && b[3] == 0x1A)
				L.crcs[L.ncrc++] = (uint32_t)crc32(0L, b + 16, size - 16);
			if (size > 512 && (size & 1023u) == 512u)
				L.crcs[L.ncrc++] = (uint32_t)crc32(0L, b + 512, size - 512);
			K.crc = L.crcs[0];
		}
	}
	L.base = K.used;
	K.live = 1;
	load_chosen();
	g_cheats_dbg_us = (gpt4_get_current_tick() - t0) / 13u;
}

int pico_cheats_on(void) { return K.live && K.on > 0; }

/* ---- the chooser: the list to use, picked by hand ---- */
/* the player's .cht files in the system's folder, then the pack's titles; a heading over each kind */
static int s_pick_cap;
static void pick_add(const char *title, int kind, int rec)
{
	if (L.cn < s_pick_cap) { L.pick[L.cn].title = title; L.pick[L.cn].kind = kind; L.pick[L.cn].rec = rec; L.cn++; }
}
enum { PICK_HEAD = -1 };
static char *str_keep(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = ch_alloc((unsigned)n);
	if (d) memcpy(d, s, n);
	return d;
}
static void chooser_open(void)
{
	fat_dir d;
	fat_dirent e;
	fat_file f;
	uint32_t nt = L.hd[8], k;
	int files = 0, first_title, at = 0;
	L.cmark = K.used;
	s_pick_cap = (int)nt + 520;
	L.pick = ch_alloc((unsigned)s_pick_cap * sizeof *L.pick);
	L.cn = 0;
	if (!L.pick) return;
	pick_add("Found for this game", LIST_AUTO, -1);
	pick_add("None", LIST_NONE, -1);
	if (fat_opendir(K.v, K.dir, &d) == 0) {
		int head = L.cn;
		pick_add("Your cheat files", PICK_HEAD, -1);
		while (fat_readdir(&d, &e) == 0 && files < 512) {
			size_t n = strlen(e.name);
			char *t;
			if (e.is_dir || n < 5 || strcmp(e.name + n - 4, ".cht")) continue;
			if (!(t = str_keep(e.name))) break;
			t[n - 4] = 0;
			pick_add(t, LIST_FILE, -1);
			files++;
		}
		if (!files) L.cn = head;
	}
	first_title = L.cn;
	if (nt && pack_open(&f) == 0) {
		uint8_t *tab;
		char *names;
		uint32_t lo, got;
		pick_add("Cheat database", PICK_HEAD, -1);
		first_title = L.cn;
		if (titles_read(&f, &tab, &names, &lo, &got) == 0)
			for (k = 0; k < nt; k++) {
				uint32_t a = rd32(tab + k * 8u) - lo;
				if (a < got) pick_add(names + a, LIST_PACK, (int)rd32(tab + k * 8u + 4));
			}
	}
	/* open at the part where the game's own name would be among the titles */
	{
		char gname[256], *dot;
		snprintf(gname, sizeof gname, "%s", K.game);
		if ((dot = strrchr(gname, '.')) != 0) *dot = 0;
		for (at = first_title; at < L.cn; at++) {
			const char *a = L.pick[at].title, *b = gname;
			while (*a && *b && (*a | 32) == (*b | 32)) a++, b++;
			if ((*a | 32) >= (*b | 32)) break;
		}
		if (at >= L.cn) at = L.cn - 1;
	}
	L.query[0] = 0;
	L.cand = 0;
	L.vis = ch_alloc((unsigned)(L.cn > 0 ? L.cn : 1) * sizeof(int));
	L.vn = 0;
	if (L.vis) for (k = 0; k < (uint32_t)L.cn; k++) L.vis[L.vn++] = (int)k;
	L.cpart = L.vn <= (int)CH_SHOWN ? 0 : at / (int)CH_PART;
	L.choosing = 1;
}
/* the letters a search takes, in the order Left / Right go through them */
static const char k_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
/* the entries holding the typed letters, case aside (the full list while nothing is typed) */
static void chooser_filter(void)
{
	int k;
	L.vn = 0;
	if (!L.vis) return;
	for (k = 0; k < L.cn; k++) {
		const char *t = L.pick[k].title, *q = L.query;
		if (q[0]) {
			const char *p;
			int hit = 0;
			if (L.pick[k].kind == PICK_HEAD || L.pick[k].kind == LIST_AUTO || L.pick[k].kind == LIST_NONE) continue;
			for (p = t; *p && !hit; p++) {
				int i = 0;
				while (q[i] && p[i] && ((p[i] >= 'a' && p[i] <= 'z') ? p[i] - 32 : p[i]) == q[i]) i++;
				hit = !q[i];
			}
			if (!hit) continue;
		}
		L.vis[L.vn++] = k;
	}
	L.cpart = 0;
}
static void chooser_close(void)
{
	if (!L.choosing) return;
	L.choosing = 0;
	K.used = L.cmark;
	L.pick = 0; L.cn = 0; L.vis = 0; L.vn = 0;
}
/* the Cheats page opens: on the game's list, not the chooser */
void pico_cheats_page_open(void) { chooser_close(); }

/* ---- the page ---- */
int pico_cheats_page(void) { return K.live; }
static int parts_of(int n) { return n <= (int)CH_SHOWN ? 1 : (n + (int)CH_PART - 1) / (int)CH_PART; }
static int list_n(void) { return L.choosing ? L.vn : K.n; }
static int *part_p(void) { return L.choosing ? &L.cpart : &K.part; }
static int parts(void) { return parts_of(list_n()); }
/* the rows before the list: the Cheat list row, or in the chooser Search and (with letters typed)
 * Clear search */
static int fixed_rows(void) { return L.choosing ? 1 + (L.query[0] != 0) : 1; }
enum { ROW_NONE = -2, ROW_PARTS = -1, ROW_LIST = -3, ROW_SEARCH = -4, ROW_CLEAR = -5 };
/* row i of the page: one of the ROW_* above, or the list's row (in the chooser: the entry's index) */
static int page_row(int i)
{
	int n = list_n(), part = *part_p(), base, f = fixed_rows(), r;
	if (!K.live || i < 0) return ROW_NONE;
	if (i < f) return !L.choosing ? ROW_LIST : i == 0 ? ROW_SEARCH : ROW_CLEAR;
	i -= f;
	if (parts() == 1) r = i < n ? i : ROW_NONE;
	else if (i == 0) return ROW_PARTS;
	else {
		base = part * (int)CH_PART + i - 1;
		r = i - 1 < (int)CH_PART && base < n ? base : ROW_NONE;
	}
	return r >= 0 && L.choosing ? L.vis[r] : r;
}
int pico_cheats_rows(void)
{
	int n = list_n(), left, rows;
	if (!K.live) return 0;
	if (parts() == 1) rows = n;
	else { left = n - *part_p() * (int)CH_PART; rows = 1 + (left < (int)CH_PART ? left : (int)CH_PART); }
	return rows + fixed_rows();
}
/* L / R: the part before or after (1 when it moved; 0 at the list's first or last part) */
int pico_cheats_step_part(int dir)
{
	int n = parts(), *p = part_p();
	if (!K.live || n < 2 || *p + dir < 0 || *p + dir >= n) return 0;
	*p += dir;
	return 1;
}
const char *pico_cheats_row_title(int i)
{
	int r = page_row(i);
	if (r == ROW_LIST) return "Cheat list";
	if (r == ROW_SEARCH) return "Search";
	if (r == ROW_CLEAR) return "Clear search";
	if (r == ROW_PARTS) return "Page";
	if (r < 0) return "";
	if (L.choosing) return L.pick[r].title;
	return K.row[r].title ? K.row[r].title : "";
}
const char *pico_cheats_row_value(int i)
{
	int r = page_row(i);
	if (r == ROW_LIST) {
		if (L.kind == LIST_NONE) return "None";
		if (L.kind == LIST_AUTO) return K.cheats ? "Found for this game" : "None";
		return L.name;
	}
	if (r == ROW_SEARCH) {	/* the letters typed, then the one Left / Right offer: "LEA[F]" */
		snprintf(K.partv, sizeof K.partv, "%s[%c]", L.query, k_letters[L.cand]);
		return K.partv;
	}
	if (r == ROW_PARTS) {
		snprintf(K.partv, sizeof K.partv, "%d / %d", *part_p() + 1, parts());
		return K.partv;
	}
	if (r < 0) return 0;
	if (L.choosing) {
		const struct ch_pick *p = &L.pick[r];
		int now = p->kind == L.kind && (p->kind == LIST_AUTO || p->kind == LIST_NONE ||
						 !strcmp(p->title, L.name));
		return p->kind != PICK_HEAD && now ? "In use" : 0;
	}
	if (!K.row[r].code) return 0;
	if (K.row[r].bad) return "Not read";
	return K.row[r].on ? "On" : "Off";
}
const char *pico_cheats_row_text(int i)
{
	int r = page_row(i);
	return r >= 0 && !L.choosing && K.row[r].code ? K.row[r].code : "";
}
int pico_cheats_row_head(int i)
{
	int r = page_row(i);
	if (r < 0) return 0;
	if (L.choosing) return L.pick[r].kind == PICK_HEAD;
	return !K.row[r].code;
}
int pico_cheats_row_switch(int i)
{
	int r = page_row(i);
	if (r < 0 || L.choosing || !K.row[r].code || K.row[r].bad) return -1;
	return K.row[r].on;
}
int pico_cheats_row_change(int i, int dir)
{
	int r = page_row(i);
	if (r == ROW_LIST) {	/* the list row: the chooser */
		if (dir < 0) return 0;
		chooser_open();
		return 1;
	}
	if (r == ROW_SEARCH) {	/* Left / Right the next letter, A takes it */
		int nl = (int)sizeof k_letters - 1;
		if (dir) { L.cand = (L.cand + dir + nl) % nl; return 1; }
		if (strlen(L.query) + 1 < sizeof L.query) {
			size_t q = strlen(L.query);
			L.query[q] = k_letters[L.cand]; L.query[q + 1] = 0;
			chooser_filter();
		}
		return 1;
	}
	if (r == ROW_CLEAR) {
		if (dir < 0) return 0;
		L.query[0] = 0;
		chooser_filter();
		return 1;
	}
	if (r == ROW_PARTS) {
		int n = parts(), *part = part_p();
		*part = (*part + (dir < 0 ? n - 1 : 1)) % n;
		return 1;
	}
	if (r < 0) return 0;
	if (L.choosing) {	/* a list picked: in force at once, kept for the game */
		struct ch_pick p = L.pick[r];
		char name[160];
		if (p.kind == PICK_HEAD || dir < 0) return 0;
		snprintf(name, sizeof name, "%s", p.title ? p.title : "");
		chooser_close();
		load_list(p.kind, name);
		save_chosen();
		return 1;
	}
	if (!K.row[r].code) return 0;
	if (K.row[r].bad) {	/* the core would not read it: once more, in case it was the order */
		K.row[r].bad = 0;
		K.row[r].on = 1;
	} else {
		K.row[r].on = !K.row[r].on;
	}
	apply();
	save_on();
	return 1;
}
/* the page's rows are a chooser now (the menus draw them as plain rows, the in use one marked) */
int pico_cheats_choosing(void) { return L.choosing; }
const char *pico_cheats_short(void)
{
	if (!K.live || !K.on) return 0;
	snprintf(K.shortv, sizeof K.shortv, "%d", K.on);
	return K.shortv;
}

void pico_cheats_diag(char *out, int cap)
{
	/* short: a fastboot line holds 64 characters */
	snprintf(out, (size_t)cap, "ch L%d S%d k%d F%d %08x r%d c%d on%d p%d %uK %ums",
		 K.live, K.system, L.kind, K.found, K.crc, K.n, K.cheats, K.on, K.part, K.used >> 10,
		 g_cheats_dbg_us / 1000u);
}

/* ---- oem cheats ---- */
/* oem cheats[:N | :list:N]: the state, then rows from the list's start; :N flips row N of the page
 * as A on it would, at the game's next frame (pico_cheats_service, on the game's thread: the core is
 * never called from the USB thread) */
static volatile int s_dbg_flip = -1;
static int s_dbg_from;
void pico_cheats_debug(const char *arg)
{
	int n = 0;
	if (!strncmp(arg, "list:", 5)) {
		for (arg += 5; *arg >= '0' && *arg <= '9'; arg++) n = n * 10 + (*arg - '0');
		s_dbg_from = n;
		return;
	}
	if (*arg < '0' || *arg > '9') return;
	for (; *arg >= '0' && *arg <= '9'; arg++) n = n * 10 + (*arg - '0');
	s_dbg_flip = n;
}
void pico_cheats_service(void)
{
	int r = s_dbg_flip;
	if (r < 0) return;
	s_dbg_flip = -1;
	pico_cheats_row_change(r, 0);
}
int pico_cheats_debug_line(int i, char *buf, unsigned cap)
{
	int r;
	const char *v;
	if (i == 0) { pico_cheats_diag(buf, (int)cap); return 1; }
	r = s_dbg_from + i - 1;
	if (r >= pico_cheats_rows()) return 0;
	v = pico_cheats_row_value(r);
	snprintf(buf, cap, "%d %s %.44s", r, pico_cheats_row_head(r) ? "--" : v ? v : "", pico_cheats_row_title(r));
	return 1;
}
