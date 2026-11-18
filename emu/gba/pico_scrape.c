/*
 * pico_scrape.c - see pico_scrape.h. Only on the consoles with Wi-Fi.
 */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
#include <string.h>
#include <printf.h>
#include <stdint.h>
#include "pico_scrape.h"
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

enum { ST_IDLE, ST_PICK, ST_INFO, ST_INFO_WAIT, ST_IMAGE_WAIT, ST_LR_NEXT, ST_LR_WAIT, ST_IMPORT, ST_DONE };

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
	char path[768];
} S;

/* where the box art stands, for oem netsvc:crumb: the step (ST_*), the game (<< 8), what the menu
 * thread is doing in it (<< 24: 1 reading the game, 2 writing a picture, 3 making .ART files) */
volatile unsigned int g_scrape_crumb;

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

static void fail_game(const char *why)
{
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
	S.source = source == SCRAPE_SCREENSCRAPER && pico_scrape_has_screenscraper() ? SCRAPE_SCREENSCRAPER : SCRAPE_LIBRETRO;
	strncpy(S.region, region && *region ? region : "wor", sizeof S.region - 1);
	S.info.running = 1;
	S.info.why = "";
	s_why[0] = 0;
	for (i = 0; i < nrom; i++)
		if (lr_system((int)roms[i].type) && !has_art(&roms[i])) S.info.total++;
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
	return r == 1 ? 1 : 0;
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
		while (S.at < S.nrom && (!lr_system((int)S.roms[S.at].type) || has_art(&S.roms[S.at]))) S.at++;
		if (S.at >= S.nrom) { S.state = S.info.found ? ST_IMPORT : ST_DONE; return 1; }
		g = &S.roms[S.at];
		S.info.game = g->uname;
		S.lr_try = 0;
		S.state = S.source == SCRAPE_SCREENSCRAPER ? ST_INFO : ST_LR_NEXT;
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
		size = g->size;
		g_scrape_crumb |= 1u << 24;
		if ((g->arc != RA_NONE || g->size <= (4u << 20)) && gba_rom_identity(S.v, g, &size, &crc) != RA_OK) {
			fail_game("the game could not be read");
			next_game();
			return 1;
		}
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
		if (status == 404 || status == 200) S.info.missing++;	/* not known, or no cover */
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
	case ST_LR_NEXT:
		if (!lr_path()) { S.info.missing++; next_game(); return 1; }
		if (ask(LR_HOST, 1) != 0) return 1;		/* busy: again next frame */
		S.state = ST_LR_WAIT;
		return 1;
	case ST_LR_WAIT:
		r = answer(&status, &body, &len);
		if (r == 0) return 1;
		if (r < 0) { next_game(); return 1; }
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
		g_scrape_crumb |= 3u << 24;
		S.info.made = pico_artimport_run(S.v, S.roms, S.nrom);
		S.state = ST_DONE;
		return 1;
	case ST_DONE:
	default:
		S.info.running = 0;
		S.info.game = 0;
		S.state = ST_IDLE;
		return 0;
	}
}
#endif
