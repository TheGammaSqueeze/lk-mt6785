/*
 * pico_scrape.c - see pico_scrape.h. Only on the consoles with Wi-Fi.
 */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
#include <string.h>
#include <stdlib.h>          /* strtoul */
#include <lib/zlib.h>        /* crc32: a dump without its header */
#include <printf.h>
#include <stdint.h>
#include "pico_scrape.h"
#include "fat_wr.h"		/* fat_wr_mkpath: the lists' folder */
#include "pico_clock.h"	/* the day a miss is kept with */
#include "gba_sd_save.h"
#include "gba_boxart_sd.h"
#include "pico_artimport.h"
#include "rom_archive.h"
#include "../../platform/common/pico/pico_netsvc.h"

#ifdef PICO_SS_CREDS
#include "pico_ss_creds.gen.h"		/* never committed: tools/ayaneo/pico_current/gen_ss_creds.py */
#endif

#define SS_HOST		"api.screenscraper.fr"
#define LR_HOST		"thumbnails.libretro.com"
#define DB_HOST		"raw.githubusercontent.com"	/* libretro-database: the No-Intro lists (CRC to name) */
#define DB_PATH		"/libretro/libretro-database/master/metadat/no-intro/"

enum { ST_IDLE, ST_PICK, ST_INFO, ST_INFO_WAIT, ST_IMAGE_WAIT, ST_LR_NEXT, ST_LR_WAIT, ST_IMPORT, ST_DONE,
       ST_DB, ST_DB_WAIT, ST_LR_CRC, ST_LR_CRC_WAIT };

static struct {
	int state, source;
	char region[4];
	fat_vol *v;
	const gba_rom_entry *roms;
	int nrom, at;			/* the game in hand (roms index) */
	struct pico_scrape_info info;
	char dir[112];
	int lr_try;			/* libretro: which name form / region is being tried */
	unsigned int asked;		/* when the request in hand was made (GPT ticks) */
	uint32_t crc, size;		/* the game in hand: its CRC (0 none) and size, worked out once */
	uint32_t crc_bare;		/* NES / SNES: the CRC without its file header (0 none), for the lists */
	int have_id;
	char path[768];
} S;

/* ---- the games a source did not have: not asked again for 30 days ----
 * /gammaos/net/artmiss.dat: "GPAM", version 1, the count, then per game a hash of the source, the
 * system and the name, and the day (since 1970) it was missed. A test ROM or a homebrew game cost
 * a server round trip on every pass; now one a month. */
#define MISS_MAX	4096u
#define MISS_DAYS	30
/* how games are matched, in the misses' hash: raised whenever matching finds more (2: libretro by
 * No-Intro CRC, 0137380), so games missed the old way are asked again at once, not a month later
 * (the old entries no longer match anything and go after their 30 days) */
#define MISS_MATCH_GEN	2u
struct miss { uint32_t h; uint32_t day; };
static struct miss *s_miss;
static unsigned int s_nmiss;
static int s_miss_dirty, s_skipped;
static uint32_t miss_hash(const gba_rom_entry *r)
{
	uint32_t h = 2166136261u ^ (uint32_t)S.source ^ (MISS_MATCH_GEN << 8);
	const char *c;
	h = (h ^ (uint32_t)r->type) * 16777619u;
	for (c = r->name; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
	return h;
}
static uint32_t today(void)
{
	long long t = pico_clock_is_set() ? pico_clock_utc() : 0;
	return t > 0 ? (uint32_t)(t / 86400) : 0;
}
static void miss_load(void)
{
	unsigned char *b;
	int n;
	s_nmiss = 0; s_miss_dirty = 0; s_skipped = 0;
	if (!s_miss) s_miss = (struct miss *)malloc(MISS_MAX * sizeof *s_miss + 8);
	if (!s_miss) return;
	b = (unsigned char *)s_miss;
	n = gba_sd_read_named(S.v, "/gammaos/net", "artmiss", "dat", b, MISS_MAX * sizeof *s_miss + 8);
	if (n >= 8 && !memcmp(b, "GPAM", 4) && b[4] == 1) {
		unsigned int cnt = b[6] | (unsigned)b[7] << 8, i;
		if (cnt > MISS_MAX || 8 + cnt * sizeof *s_miss > (unsigned)n) cnt = 0;
		memmove(s_miss, b + 8, cnt * sizeof *s_miss);
		/* the ones past their 30 days go (and with no clock, all are kept as they are) */
		for (i = 0; i < cnt; i++)
			if (!today() || today() - s_miss[i].day < MISS_DAYS) s_miss[s_nmiss++] = s_miss[i];
		if (s_nmiss != cnt) s_miss_dirty = 1;
	}
}
static int miss_known(const gba_rom_entry *r)
{
	uint32_t h = miss_hash(r);
	unsigned int i;
	for (i = 0; i < s_nmiss; i++) if (s_miss[i].h == h) return 1;
	return 0;
}
static void miss_add(const gba_rom_entry *r)
{
	if (!s_miss || s_nmiss >= MISS_MAX || miss_known(r) || !today()) return;	/* (no clock: no date to keep) */
	s_miss[s_nmiss].h = miss_hash(r);
	s_miss[s_nmiss].day = today();
	s_nmiss++;
	s_miss_dirty = 1;
}
static void miss_save(void)
{
	static unsigned char hdr[8] = { 'G', 'P', 'A', 'M', 1, 0, 0, 0 };
	unsigned char *b;
	if (!s_miss || !s_miss_dirty) return;
	/* the header in front of the entries: they move up by its 8 bytes for the write */
	b = (unsigned char *)s_miss;
	memmove(b + 8, b, s_nmiss * sizeof *s_miss);
	hdr[6] = (unsigned char)s_nmiss; hdr[7] = (unsigned char)(s_nmiss >> 8);
	memcpy(b, hdr, 8);
	gba_sd_write_named(S.v, "/gammaos/net", "artmiss", "dat", b, 8 + s_nmiss * sizeof *s_miss);
	memmove(b, b + 8, s_nmiss * sizeof *s_miss);
	s_miss_dirty = 0;
}

/* ---- libretro by CRC ----
 * libretro's thumbnails are named after No-Intro's names ("Super Mario Bros. (World)"), which a
 * game's file often is not. libretro-database's No-Intro list of each system (0.5 to 3.2 MB, read
 * once a pass for a system with games to find) gives the name for the game's CRC: kept here as CRC
 * and name pairs, in the import memory below the answers (6 to 12 MB in: the art's index and the
 * PNG import use the first MBs, and only after the downloads). */
#define DB_SYSTEMS	16
struct db_ent { uint32_t crc, name; };
static struct { int tried; unsigned int first, n, nbase, nlen; } s_db[DB_SYSTEMS];
static struct db_ent *s_dbe;			/* the pairs, every system's, one after the other */
static char *s_dbn;				/* their names */
static unsigned int s_dbe_n, s_dbe_max, s_dbn_at, s_dbn_max;
static int db_mem(void)
{
	extern void *ayaneo_import_arena(unsigned int *size);
	unsigned int sz = 0;
	unsigned char *a = (unsigned char *)ayaneo_import_arena(&sz);
	if (!a || sz < (16u << 20)) return 0;
	s_dbe = (struct db_ent *)(a + (6u << 20));
	s_dbe_max = (2u << 20) / sizeof *s_dbe;	/* 2 MB of pairs (NES: 13,296) */
	s_dbn = (char *)(a + (8u << 20));
	s_dbn_max = 4u << 20;			/* 4 MB of names */
	return 1;
}
/* the list's games into pairs: each "name" at a line's start names the game, each rom's "crc" after
 * it adds a pair (a game's two dumps, with and without a header, share one name) */
static void db_take(int sys, const char *t, unsigned int len)
{
	const char *e = t + len, *p = t;
	unsigned int name = 0;
	int have = 0;
	s_db[sys].first = s_dbe_n;
	s_db[sys].nbase = s_dbn_at;
	while (p < e) {
		const char *nl = memchr(p, '\n', (size_t)(e - p)), *le = nl ? nl : e, *q = p;
		while (q < le && (*q == ' ' || *q == '\t')) q++;
		if (le - q > 7 && !memcmp(q, "name \"", 6)) {	/* a game's name (the rom's own is mid-line) */
			const char *nb = q + 6, *ne = memchr(nb, '"', (size_t)(le - nb));
			unsigned int nlen = ne ? (unsigned int)(ne - nb) : 0;
			have = 0;
			if (nlen && s_dbn_at + nlen + 1 <= s_dbn_max) {
				name = s_dbn_at;
				memcpy(s_dbn + s_dbn_at, nb, nlen);
				s_dbn[s_dbn_at + nlen] = 0;
				s_dbn_at += nlen + 1;
				have = 1;
			}
		} else if (have) {
			const char *c;
			for (c = q; c + 13 <= le; c++)
				if (!memcmp(c, " crc ", 5)) {
					uint32_t v = 0;
					int k;
					for (k = 0; k < 8; k++) {
						char h = c[5 + k];
						v = v << 4 | (uint32_t)(h >= '0' && h <= '9' ? h - '0' : h >= 'A' && h <= 'F' ? h - 'A' + 10 :
									h >= 'a' && h <= 'f' ? h - 'a' + 10 : 0);
					}
					if (s_dbe_n < s_dbe_max) { s_dbe[s_dbe_n].crc = v; s_dbe[s_dbe_n].name = name; s_dbe_n++; }
					break;
				}
		}
		p = le + 1;
	}
	s_db[sys].n = s_dbe_n - s_db[sys].first;
	s_db[sys].nlen = s_dbn_at - s_db[sys].nbase;
}
/* A system's pairs kept on the card for 30 days (/gammaos/net/lrdb/<system number>.db): "GPDB",
 * version 1, the day, the pairs' count, the names' length, then the pairs (names counted from the
 * system's first) and the names. The NES list is 3.2 MB to fetch (some 18 s), its pairs 0.4 MB. */
static void db_file(int sys, char *name, unsigned int cap) { snprintf(name, cap, "%d", sys); }
static int db_load(int sys)
{
	char nm[8];
	unsigned char hdr[20];
	unsigned char *buf;
	unsigned int n, nl, i, day, need;
	int got;
	extern void *ayaneo_import_arena(unsigned int *size);
	unsigned int sz = 0;
	unsigned char *a = (unsigned char *)ayaneo_import_arena(&sz);
	if (!a || sz < (16u << 20)) return 0;
	buf = a + sz - (4u << 20);			/* (read whole into the answers' memory, free here) */
	db_file(sys, nm, sizeof nm);
	got = gba_sd_read_named(S.v, "/gammaos/net/lrdb", nm, "db", buf, 4u << 20);
	if (got < 20 || memcmp(buf, "GPDB", 4) || buf[4] != 1) return 0;
	memcpy(hdr, buf, 20);
	memcpy(&day, hdr + 8, 4); memcpy(&n, hdr + 12, 4); memcpy(&nl, hdr + 16, 4);
	if (today() && (today() < day || today() - day >= MISS_DAYS)) return 0;	/* old: fetched again */
	need = 20 + n * (unsigned)sizeof(struct db_ent) + nl;
	if ((unsigned)got < need || s_dbe_n + n > s_dbe_max || s_dbn_at + nl > s_dbn_max) return 0;
	s_db[sys].first = s_dbe_n;
	s_db[sys].nbase = s_dbn_at;
	memcpy(s_dbe + s_dbe_n, buf + 20, n * sizeof(struct db_ent));
	for (i = 0; i < n; i++) s_dbe[s_dbe_n + i].name += s_dbn_at;
	memcpy(s_dbn + s_dbn_at, buf + 20 + n * sizeof(struct db_ent), nl);
	s_dbe_n += n; s_dbn_at += nl;
	s_db[sys].n = n; s_db[sys].nlen = nl;
	return 1;
}
static void db_store(int sys)
{
	char nm[8];
	unsigned char *buf;
	unsigned int n = s_db[sys].n, nl = s_db[sys].nlen, i, day = today(), len;
	extern void *ayaneo_import_arena(unsigned int *size);
	unsigned int sz = 0;
	unsigned char *a = (unsigned char *)ayaneo_import_arena(&sz);
	if (!a || !n || !day) return;			/* (no clock: no day to keep it by) */
	len = 20 + n * (unsigned)sizeof(struct db_ent) + nl;
	if (len > (4u << 20)) return;
	buf = a + sz - (4u << 20);
	memcpy(buf, "GPDB", 4); buf[4] = 1; buf[5] = buf[6] = buf[7] = 0;
	memcpy(buf + 8, &day, 4); memcpy(buf + 12, &n, 4); memcpy(buf + 16, &nl, 4);
	memcpy(buf + 20, s_dbe + s_db[sys].first, n * sizeof(struct db_ent));
	for (i = 0; i < n; i++) ((struct db_ent *)(void *)(buf + 20))[i].name -= s_db[sys].nbase;
	memcpy(buf + 20 + n * sizeof(struct db_ent), s_dbn + s_db[sys].nbase, nl);
	db_file(sys, nm, sizeof nm);
	fat_wr_mkpath(S.v, "/gammaos/net/lrdb");
	gba_sd_write_named(S.v, "/gammaos/net/lrdb", nm, "db", buf, len);
}
static const char *db_name(int sys, uint32_t crc)
{
	unsigned int i;
	if (sys < 0 || sys >= DB_SYSTEMS || !crc) return 0;
	for (i = s_db[sys].first; i < s_db[sys].first + s_db[sys].n; i++)
		if (s_dbe[i].crc == crc) return s_dbn + s_dbe[i].name;
	return 0;
}
/* the game in hand's CRC and size, once (an archive's from its directory, a file of up to 4 MB read
 * whole, a bigger one by its size alone); 0 when it cannot be read */
static int game_id(void)
{
	const gba_rom_entry *g = &S.roms[S.at];
	if (S.have_id) return 1;
	S.size = g->size;
	S.crc = 0;
	if ((g->arc != RA_NONE || g->size <= (4u << 20)) && gba_rom_identity(S.v, g, &S.size, &S.crc) != RA_OK) {
		if (g->arc == RA_NONE) return 0;
		S.crc = 0;			/* an archive that keeps no CRC (a 7z may not): by name and size */
		S.size = g->size;
	}
	S.crc_bare = 0;
	if (g->type == GBA_CONSOLE_NES || g->type == GBA_CONSOLE_SNES) {
		/* No-Intro lists a NES game's dump with its standard iNES header and without any; a card's
		 * header is often another, and a SNES dump may carry a 512 byte copier header the lists
		 * never do: the CRC without the header finds them. The game is read (unpacked when in an
		 * archive) into the answers' memory, free before the game's first request. */
		extern void *ayaneo_import_arena(unsigned int *size);
		unsigned int sz = 0;
		unsigned char *a = (unsigned char *)ayaneo_import_arena(&sz), *buf;
		uint32_t n;
		if (a && sz >= (16u << 20)) {
			buf = a + sz - (4u << 20);
			n = gba_sd_load_rom(S.v, g, buf, 4u << 20);
			if (g->type == GBA_CONSOLE_NES && n > 16 && !memcmp(buf, "NES\x1a", 4))
				S.crc_bare = (uint32_t)crc32(0L, buf + 16, n - 16);
			else if (g->type == GBA_CONSOLE_SNES && n > 512 && n % 1024u == 512u)
				S.crc_bare = (uint32_t)crc32(0L, buf + 512, n - 512);
		}
	}
	S.have_id = 1;
	return 1;
}

/* where the box art stands, for oem netsvc:crumb: the step (ST_*), the game (<< 8), what the menu
 * thread is doing in it (<< 24: 1 reading the game, 2 writing a picture, 3 making .ART files) */
volatile unsigned int g_scrape_crumb;
/* oem netsvc:scrape?: the time the requests took, ms: the game's information (most, sum), the cover
 * (most, sum), and since the start */
volatile unsigned int g_scrape_ms[5];
static unsigned int s_scrape_t0;

static char s_why[64];

int pico_scrape_has_screenscraper(void)
{
#ifdef PICO_SS_CREDS
	return 1;
#else
	return 0;
#endif
}

/* ---- a few text helpers ---- */

/* text into a URL query or path: letters, digits and -._~ as they are, the rest as %XX of its
 * UTF-8 bytes; libretro's own rule first turns &*:`<>?\|/" in a game's name into '_' (game) */
static unsigned int url_add(char *out, unsigned int at, unsigned int cap, const char *t, int game)
{
	static const char hex[] = "0123456789ABCDEF";
	for (; *t && at + 4 < cap; t++) {
		unsigned char c = (unsigned char)*t;
		if (game && strchr("&*/:`<>?\\|\"", c)) c = '_';
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c))
			out[at++] = (char)c;
		else { out[at++] = '%'; out[at++] = hex[c >> 4]; out[at++] = hex[c & 15]; }
	}
	out[at] = 0;
	return at;
}

static unsigned int put(char *out, unsigned int at, unsigned int cap, const char *t)
{
	while (*t && at + 1 < cap) out[at++] = *t++;
	out[at] = 0;
	return at;
}

static int is_png(const unsigned char *b, unsigned int n)
{
	return n > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
}

/* ---- the game list ---- */

/* the game has art already: its .ART beside it or in the system's own folder, or a PNG waiting */
static int has_art(const gba_rom_entry *r)
{
	unsigned char probe[16];
	const char *home = gba_boxart_home_dir((int)r->type);
	gba_boxart_dir(r, S.dir, sizeof S.dir);
	return gba_sd_read_named(S.v, S.dir, r->name, "ART", probe, sizeof probe) >= 12 ||
	       (home && gba_sd_read_named(S.v, home, r->name, "ART", probe, sizeof probe) >= 12) ||
	       gba_sd_read_named(S.v, S.dir, r->name, "png", probe, sizeof probe) >= 8;
}

static int ss_system(int console)
{
	static const int ids[] = { 9, 10, 12, 4, 1, 2, 21, 109, 3 };	/* GBA_CONSOLE_* order */
	return console >= 0 && console < (int)(sizeof ids / sizeof ids[0]) ? ids[console] : 0;
}

static const char *lr_system(int console)
{
	switch (console) {
	case GBA_CONSOLE_GB:      return "Nintendo - Game Boy";
	case GBA_CONSOLE_GBC:     return "Nintendo - Game Boy Color";
	case GBA_CONSOLE_GBA:     return "Nintendo - Game Boy Advance";
	case GBA_CONSOLE_SNES:    return "Nintendo - Super Nintendo Entertainment System";
	case GBA_CONSOLE_NES:     return "Nintendo - Nintendo Entertainment System";
	case GBA_CONSOLE_GENESIS: return "Sega - Mega Drive - Genesis";
	case GBA_CONSOLE_SMS:     return "Sega - Master System - Mark III";
	case GBA_CONSOLE_GG:      return "Sega - Game Gear";
	case GBA_CONSOLE_SG:      return "Sega - SG-1000";
	default:                  return 0;
	}
}

static const char *default_ext(int console)
{
	static const char *const ext[] = { ".gb", ".gbc", ".gba", ".sfc", ".md", ".sms", ".gg", ".sg", ".nes" };
	return console >= 0 && console < (int)(sizeof ext / sizeof ext[0]) ? ext[console] : "";
}

/* ---- ScreenScraper ---- */

#ifdef PICO_SS_CREDS
/* one masked value into out (the key's bytes undone), for as long as a request is being made */
static unsigned int unmask(char *out, unsigned int cap, const unsigned char *a, unsigned int na, const unsigned char *b,
			   unsigned int len)
{
	unsigned int i;
	if (len + 1 > cap) return 0;
	for (i = 0; i < len; i++)
		out[i] = (char)((i < na ? a[i] : b[i - na]) ^ ss_key[i % sizeof ss_key]);
	out[len] = 0;
	return len;
}

/* the query's credential part: devid, devpassword, softname */
static unsigned int ss_creds(char *out, unsigned int at, unsigned int cap)
{
	char v[96];
	unmask(v, sizeof v, ss_id_a, sizeof ss_id_a, ss_id_b, SS_ID_LEN);
	at = put(out, at, cap, "devid=");
	at = url_add(out, at, cap, v, 0);
	unmask(v, sizeof v, ss_pw_a, sizeof ss_pw_a, ss_pw_b, SS_PW_LEN);
	at = put(out, at, cap, "&devpassword=");
	at = url_add(out, at, cap, v, 0);
	unmask(v, sizeof v, ss_soft_a, sizeof ss_soft_a, ss_soft_b, SS_SOFT_LEN);
	at = put(out, at, cap, "&softname=");
	at = url_add(out, at, cap, v, 0);
	memset(v, 0, sizeof v);
	return at;
}
#endif

/* a JSON string's value after "key": in obj (up to end), unescaped into out: 1 found */
static int json_str(const char *obj, const char *end, const char *key, char *out, unsigned int cap)
{
	unsigned int kl = (unsigned)strlen(key), n = 0;
	const char *p = obj;
	for (; p + kl + 3 < end; p++) {
		if (*p != '"' || strncmp(p + 1, key, kl) || p[1 + kl] != '"') continue;
		p += 2 + kl;
		while (p < end && (*p == ' ' || *p == ':')) p++;
		if (p >= end || *p != '"') return 0;
		for (p++; p < end && *p != '"' && n + 1 < cap; p++) {
			if (*p == '\\' && p + 1 < end) {
				p++;
				if (*p == 'u') { p += 4; continue; }	/* never in the fields we read */
			}
			out[n++] = *p;
		}
		out[n] = 0;
		return 1;
	}
	return 0;
}

/* the best cover's address in a jeuInfos answer: box-2D then box-3D, the region asked for first,
 * then the world, USA, Europe, Japan, any: 1 found */
static int ss_cover(const char *json, unsigned int len, char *url, unsigned int cap)
{
	static const char *const types[] = { "box-2D", "box-3D" };
	const char *regions[7];
	const char *end = json + len, *m;
	int t, r, best = 99;
	regions[0] = S.region; regions[1] = "wor"; regions[2] = "us"; regions[3] = "eu"; regions[4] = "ss"; regions[5] = "jp";
	regions[6] = 0;
	m = strstr(json, "\"medias\"");
	if (!m || m >= end) return 0;
	for (t = 0; t < 2; t++) {
		const char *p = m;
		while ((p = strchr(p, '{')) != 0 && p < end) {
			const char *q = strchr(p, '}');
			char ty[16], rg[8], u[400];
			if (!q || q > end) break;
			if (json_str(p, q, "type", ty, sizeof ty) && !strcmp(ty, types[t]) && json_str(p, q, "url", u, sizeof u)) {
				int score = 6;			/* a region we did not list */
				if (json_str(p, q, "region", rg, sizeof rg))
					for (r = 0; r < 6; r++) if (!strcmp(rg, regions[r])) { score = r; break; }
				if (score < best) {
					best = score;
					strncpy(url, u, cap - 1);
					url[cap - 1] = 0;
				}
			}
			p = q + 1;
		}
		if (best < 99) return 1;
	}
	return 0;
}

/* ---- the steps ---- */

/* oem netsvc:scrape?: the last games that failed, and why */
static char s_fail_log[3][64];
static unsigned s_ss_threads, s_ss_today, s_ss_daymax;	/* ScreenScraper's limits for our account (0 not known) */
static unsigned s_lr_crc_hits;			/* covers found by CRC through the No-Intro lists */
static unsigned s_fail_n;
const char *pico_scrape_fail_line(int i) { return i >= 0 && i < 3 ? s_fail_log[i] : ""; }
static void fail_game(const char *why)
{
	if (S.at < S.nrom)
	{	/* (the name cut to 24 by hand: this printf reads %.24s as a width) */
		char nm[25];
		strncpy(nm, S.roms[S.at].name, sizeof nm - 1); nm[sizeof nm - 1] = 0;
		snprintf(s_fail_log[s_fail_n++ % 3], sizeof s_fail_log[0], "%s: %s", nm, why ? why : "");
	}
	S.info.failed++;
	strncpy(s_why, why ? why : "", sizeof s_why - 1);
	S.info.why = s_why;
}

static void next_game(void)
{
	S.info.done++;
	S.at++;
	S.state = ST_PICK;
}

void pico_scrape_start(fat_vol *v, const gba_rom_entry *roms, int nrom, int source, const char *region)
{
	int i;
	memset(&S, 0, sizeof S);
	S.v = v;
	S.roms = roms;
	S.nrom = nrom;
	S.source = (source == SCRAPE_SCREENSCRAPER || source == SCRAPE_BOTH) && pico_scrape_has_screenscraper() ? source :
		   SCRAPE_LIBRETRO;
	strncpy(S.region, region && *region ? region : "wor", sizeof S.region - 1);
	S.info.running = 1;
	S.info.why = "";
	{ extern unsigned int gpt4_get_current_tick(void); s_scrape_t0 = gpt4_get_current_tick(); }
	memset((void *)g_scrape_ms, 0, sizeof g_scrape_ms);
	{ extern volatile unsigned int g_wf_step_ms[4]; memset((void *)g_wf_step_ms, 0, sizeof g_wf_step_ms); }
	{ extern volatile unsigned int g_tls_ms[3]; memset((void *)g_tls_ms, 0, sizeof g_tls_ms); }
	s_why[0] = 0;
	miss_load();
	memset(s_db, 0, sizeof s_db);
	s_dbe_n = s_dbn_at = 0;
	s_lr_crc_hits = 0;
	for (i = 0; i < nrom; i++)
		if (lr_system((int)roms[i].type) && !has_art(&roms[i]) && !miss_known(&roms[i])) S.info.total++;
	S.state = ST_PICK;
}

void pico_scrape_cancel(void)
{
	if (S.state == ST_INFO_WAIT || S.state == ST_IMAGE_WAIT || S.state == ST_LR_WAIT)
		netsvc_http_abort();		/* one still running stops now, its slot freed */
	S.state = S.info.found ? ST_IMPORT : ST_DONE;
	strncpy(s_why, "stopped", sizeof s_why - 1);
	S.info.why = s_why;
}

void pico_scrape_get(struct pico_scrape_info *info)
{
	*info = S.info;
}

/* a request to the service: path (host apart) */
static int ask(const char *host, int secure)
{
	extern unsigned int gpt4_get_current_tick(void);
	S.asked = gpt4_get_current_tick();
	{	/* the answers into the top 4 MB of the import memory (free while the art is fetched: the
		 * art's index and the PNG import use its first few MB, and only once the downloads are
		 * done): a famous game's information is past the service's 768 KB */
		extern void *ayaneo_import_arena(unsigned int *size);
		unsigned int sz = 0;
		unsigned char *a = (unsigned char *)ayaneo_import_arena(&sz);
		if (a && sz >= (8u << 20))
			return netsvc_http_begin_into(host, S.path, secure, 0, 0, a + sz - (4u << 20), 4u << 20);
	}
	return netsvc_http_begin(host, S.path, secure, 0, 0);
}

/* longer than the service's own limits allow (name, connection, handshake, a minute of answer):
 * given up, so one game never holds the rest up */
#define ANSWER_LIMIT_MS	120000u

/* a transfer's answer: 1 done (*body, *len, *status), 0 not yet, -1 failed (it is released) */
static int answer(unsigned int *status, const unsigned char **body, unsigned int *len)
{
	const char *why = "";
	int r = netsvc_http_poll(status, body, len, &why);
	if (r == -1) { netsvc_http_release(); fail_game(why); return -1; }
	if (r == -2) { fail_game("the request was lost"); return -1; }
	if (r == 0) {
		extern unsigned int gpt4_get_current_tick(void);
		if ((gpt4_get_current_tick() - S.asked) / 13000u > ANSWER_LIMIT_MS) {
			netsvc_http_abort();
			fail_game("the server took too long");
			return -1;
		}
	}
	if (r == 1) {
		extern unsigned int gpt4_get_current_tick(void);
		unsigned int ms = (gpt4_get_current_tick() - S.asked) / 13000u, k = S.state == ST_IMAGE_WAIT ? 2u : 0u;
		if (ms > g_scrape_ms[k]) g_scrape_ms[k] = ms;
		g_scrape_ms[k + 1] += ms;
	}
	return r == 1 ? 1 : 0;
}
/* oem netsvc:scrape?: how it stands, in two short lines */
void pico_scrape_report(char *a, unsigned cap_a, char *b, unsigned cap_b)
{
	extern unsigned int gpt4_get_current_tick(void);
	if (S.info.running) g_scrape_ms[4] = (gpt4_get_current_tick() - s_scrape_t0) / 13000u;
	snprintf(a, cap_a, "scrape %s %d/%d f%d m%d x%d skip %d crc %u db %u", S.info.running ? "on" : "off",
		 S.info.done, S.info.total, S.info.found, S.info.missing, S.info.failed, s_skipped, s_lr_crc_hits, s_dbe_n);
	{
		extern volatile unsigned int g_wf_step_ms[4];
		(void)g_wf_step_ms;
		snprintf(b, cap_b, "ms info %u/%u img %u/%u all %u", g_scrape_ms[0], g_scrape_ms[1],
			 g_scrape_ms[2], g_scrape_ms[3], g_scrape_ms[4]);
	}
}

static void save_png(const unsigned char *b, unsigned int len)
{
	const gba_rom_entry *r = &S.roms[S.at];
	gba_boxart_dir(r, S.dir, sizeof S.dir);
	g_scrape_crumb |= 2u << 24;
	if (gba_sd_write_named(S.v, S.dir, r->name, "png", b, len) == 0) S.info.found++;
	else fail_game("the card write failed");
}

/* the libretro name to try now (forms: the name, and with '_' read as an apostrophe; regions:
 * none when the name has one, else the usual ones): 0 when there are no more */
static int lr_path(void)
{
	static const char *const regions[] = { "", " (USA)", " (USA, Europe)", " (World)", " (Europe)", " (Japan)",
					       " (USA, Australia)", " (Japan, USA)" };
	const gba_rom_entry *r = &S.roms[S.at];
	int nreg = strchr(r->name, '(') ? 1 : (int)(sizeof regions / sizeof regions[0]);
	int forms = strchr(r->name, '_') ? 2 : 1, form = S.lr_try / nreg, reg = S.lr_try % nreg;
	char name[sizeof r->name];
	unsigned int at;
	if (form >= forms) return 0;
	strcpy(name, r->name);
	if (form) { char *c; for (c = name; *c; c++) if (*c == '_') *c = '\''; }
	S.path[0] = '/';
	at = url_add(S.path, 1, sizeof S.path, lr_system((int)r->type), 0);
	at = put(S.path, at, sizeof S.path, "/Named_Boxarts/");
	at = url_add(S.path, at, sizeof S.path, name, 1);
	at = url_add(S.path, at, sizeof S.path, regions[reg], 0);
	url_add(S.path, at, sizeof S.path, ".png", 0);
	return 1;
}

int pico_scrape_step(void)
{
	unsigned int status = 0, len = 0;
	const unsigned char *body = 0;
	int r;
	g_scrape_crumb = (unsigned)S.state | ((unsigned)S.at & 0xFFFFu) << 8;
	switch (S.state) {
	case ST_IDLE:
		return 0;
	case ST_PICK: {
		const gba_rom_entry *g;
		struct netsvc_status ns;
		netsvc_get(&ns);
		if (ns.state != NETSVC_ONLINE) {
			strncpy(s_why, "not connected to a network", sizeof s_why - 1);
			S.info.why = s_why;
			S.state = S.info.found ? ST_IMPORT : ST_DONE;
			return 1;
		}
		while (S.at < S.nrom && (!lr_system((int)S.roms[S.at].type) || has_art(&S.roms[S.at]) ||
					 (miss_known(&S.roms[S.at]) && ++s_skipped))) S.at++;
		if (S.at >= S.nrom) { S.state = S.info.found ? ST_IMPORT : ST_DONE; return 1; }
		g = &S.roms[S.at];
		S.info.game = g->uname;
		S.lr_try = 0;
		S.have_id = 0;
		S.state = S.source == SCRAPE_SCREENSCRAPER ? ST_INFO : ST_DB;
		return 1;
	}
#ifdef PICO_SS_CREDS
	case ST_INFO: {
		const gba_rom_entry *g = &S.roms[S.at];
		uint32_t size = 0, crc = 0;
		char num[16];
		unsigned int at;
		/* the CRC: from an archive's directory (cheap), or by reading a file of up to 4 MB in this
		 * frame; a bigger one goes by its name and size, so the menu never stalls for seconds */
		g_scrape_crumb |= 1u << 24;
		if (!game_id()) {
			fail_game("the game could not be read");
			next_game();
			return 1;
		}
		size = S.size; crc = S.crc;
		at = put(S.path, 0, sizeof S.path, "/api2/jeuInfos.php?");
		at = ss_creds(S.path, at, sizeof S.path);
		at = put(S.path, at, sizeof S.path, "&output=json&romtype=rom&systemeid=");
		snprintf(num, sizeof num, "%d", ss_system((int)g->type));
		at = put(S.path, at, sizeof S.path, num);
		if (crc) {
			at = put(S.path, at, sizeof S.path, "&crc=");
			snprintf(num, sizeof num, "%08X", (unsigned)crc);
			at = put(S.path, at, sizeof S.path, num);
		}
		at = put(S.path, at, sizeof S.path, "&romtaille=");
		snprintf(num, sizeof num, "%u", (unsigned)size);
		at = put(S.path, at, sizeof S.path, num);
		at = put(S.path, at, sizeof S.path, "&romnom=");
		at = url_add(S.path, at, sizeof S.path, g->name, 0);
		url_add(S.path, at, sizeof S.path, default_ext((int)g->type), 0);
		r = ask(SS_HOST, 1);
		memset(S.path, 0, sizeof S.path);		/* the credentials go with it */
		if (r != 0) return 1;				/* the service is busy: again next frame */
		S.state = ST_INFO_WAIT;
		return 1;
	}
	case ST_INFO_WAIT: {
		static char url[400];
		r = answer(&status, &body, &len);
		if (r == 0) return 1;
		if (r < 0) { next_game(); return 1; }
		if (status == 200) {	/* the account's limits, as the answer gives them (oem netsvc:scrape?) */
			char v[16];
			const char *e = (const char *)body + len;
			if (json_str((const char *)body, e, "maxthreads", v, sizeof v)) s_ss_threads = (unsigned)strtoul(v, 0, 10);
			if (json_str((const char *)body, e, "requeststoday", v, sizeof v)) s_ss_today = (unsigned)strtoul(v, 0, 10);
			if (json_str((const char *)body, e, "maxrequestsperday", v, sizeof v)) s_ss_daymax = (unsigned)strtoul(v, 0, 10);
		}
		if (status == 200 && ss_cover((const char *)body, len, url, sizeof url)) {
			/* https://host/path: the address carries the credentials, so it is never kept */
			char host[96];
			const char *h = strstr(url, "://"), *slash;
			netsvc_http_release();
			h = h ? h + 3 : url;
			slash = strchr(h, '/');
			if (!slash || (unsigned)(slash - h) >= sizeof host) { fail_game("an odd media address"); next_game(); return 1; }
			memcpy(host, h, (unsigned)(slash - h));
			host[slash - h] = 0;
			strncpy(S.path, slash, sizeof S.path - 1);
			/* ScreenScraper's covers come full size (900 KB and more: past what a download may
			 * hold); the menu shows them at 224, so at most twice that is asked for */
			if (strstr(S.path, "mediaJeu.php") && strlen(S.path) + 32 < sizeof S.path)
				strcat(S.path, "&maxwidth=448&maxheight=448");
			r = ask(host, 1);
			memset(S.path, 0, sizeof S.path);
			memset(url, 0, sizeof url);
			if (r != 0) { fail_game("the service was busy"); next_game(); return 1; }
			S.state = ST_IMAGE_WAIT;
			return 1;
		}
		netsvc_http_release();
		if (status == 404 || status == 200) { S.info.missing++; miss_add(&S.roms[S.at]); }	/* not known, or no cover */
		else if (status == 401 || status == 403 || status == 423 || status == 429 || status == 430 || status == 431) {
			snprintf(s_why, sizeof s_why, "ScreenScraper refused (%u): try later", status);
			S.info.why = s_why;
			S.state = S.info.found ? ST_IMPORT : ST_DONE;
			return 1;
		} else {
			char w[40];
			snprintf(w, sizeof w, "ScreenScraper answered %u", status);
			fail_game(w);
		}
		next_game();
		return 1;
	}
	case ST_IMAGE_WAIT:
		r = answer(&status, &body, &len);
		if (r == 0) return 1;
		if (r > 0) {
			if (status == 200 && is_png(body, len)) save_png(body, len);
			else if (status == 200) fail_game("the cover is not a PNG");
			else S.info.missing++;
			netsvc_http_release();
		}
		next_game();
		return 1;
#else
	case ST_INFO:
	case ST_INFO_WAIT:
	case ST_IMAGE_WAIT:
		S.state = ST_LR_NEXT;
		return 1;
#endif
	case ST_DB: {	/* the system's No-Intro list, once a pass */
		const gba_rom_entry *g = &S.roms[S.at];
		int sys = (int)g->type;
		unsigned int at;
		if (sys < 0 || sys >= DB_SYSTEMS || s_db[sys].tried || (!s_dbe && !db_mem())) { S.state = ST_LR_CRC; return 1; }
		if (db_load(sys)) { s_db[sys].tried = 1; S.state = ST_LR_CRC; return 1; }	/* kept from an earlier pass */
		at = put(S.path, 0, sizeof S.path, DB_PATH);
		at = url_add(S.path, at, sizeof S.path, lr_system(sys), 0);
		put(S.path, at, sizeof S.path, ".dat");
		if (ask(DB_HOST, 1) != 0) return 1;		/* busy: again next frame */
		S.state = ST_DB_WAIT;
		return 1;
	}
	case ST_DB_WAIT: {
		int sys = (int)S.roms[S.at].type;
		r = answer(&status, &body, &len);
		if (r == 0) return 1;
		if (r < 0) S.info.failed--;			/* (the list is a help: no list, no fault of the game) */
		s_db[sys].tried = 1;
		if (r > 0) {
			if (status == 200) db_take(sys, (const char *)body, len);
			netsvc_http_release();
			if (status == 200) db_store(sys);
		}
		S.state = ST_LR_CRC;
		return 1;
	}
	case ST_LR_CRC: {	/* the game's No-Intro name by its CRC: libretro's thumbnail of that name */
		const gba_rom_entry *g = &S.roms[S.at];
		const char *nm;
		unsigned int at;
		g_scrape_crumb |= 1u << 24;
		nm = game_id() ? db_name((int)g->type, S.crc) : 0;
		if (!nm && S.crc_bare) nm = db_name((int)g->type, S.crc_bare);	/* (the dump without its header) */
		if (!nm) { S.state = ST_LR_NEXT; return 1; }	/* not in the list: the name forms */
		S.path[0] = '/';
		at = url_add(S.path, 1, sizeof S.path, lr_system((int)g->type), 0);
		at = put(S.path, at, sizeof S.path, "/Named_Boxarts/");
		at = url_add(S.path, at, sizeof S.path, nm, 1);
		url_add(S.path, at, sizeof S.path, ".png", 0);
		if (ask(LR_HOST, 1) != 0) return 1;
		S.state = ST_LR_CRC_WAIT;
		return 1;
	}
	case ST_LR_CRC_WAIT:
		r = answer(&status, &body, &len);
		if (r == 0) return 1;
		if (r < 0) { S.info.failed--; S.state = ST_LR_NEXT; return 1; }
		if (status == 200 && is_png(body, len)) {
			save_png(body, len);
			netsvc_http_release();
			s_lr_crc_hits++;
			next_game();
			return 1;
		}
		netsvc_http_release();
		S.state = ST_LR_NEXT;			/* no thumbnail under that name: the file's name forms */
		return 1;
	case ST_LR_NEXT:
		if (!lr_path()) {
			if (S.source == SCRAPE_BOTH) { S.state = ST_INFO; return 1; }	/* not at libretro: ScreenScraper */
			S.info.missing++; miss_add(&S.roms[S.at]); next_game(); return 1;
		}
		if (ask(LR_HOST, 1) != 0) return 1;		/* busy: again next frame */
		S.state = ST_LR_WAIT;
		return 1;
	case ST_LR_WAIT:
		r = answer(&status, &body, &len);
		if (r == 0) return 1;
		if (r < 0) {
			if (S.source == SCRAPE_BOTH) { S.info.failed--; S.state = ST_INFO; return 1; }	/* (ScreenScraper may have it) */
			next_game();
			return 1;
		}
		if (status == 200 && is_png(body, len)) {
			save_png(body, len);
			netsvc_http_release();
			next_game();
			return 1;
		}
		netsvc_http_release();
		S.lr_try++;
		S.state = ST_LR_NEXT;
		return 1;
	case ST_IMPORT:
		miss_save();
		g_scrape_crumb |= 3u << 24;
		S.info.made = pico_artimport_run(S.v, S.roms, S.nrom);
		S.state = ST_DONE;
		return 1;
	case ST_DONE:
	default:
		miss_save();
		S.info.running = 0;
		S.info.game = 0;
		S.state = ST_IDLE;
		return 0;
	}
}
#endif
