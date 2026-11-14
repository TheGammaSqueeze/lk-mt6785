/*
 * pico_stats.c - play time and last played (pico_stats.h).
 *
 * /gammaos/playtime.txt holds a line a game: "<console> <seconds played> <last started, UTC> <name>".
 * A session is timed on the LK's millisecond counter, so setting the clock during a game does not
 * change its length; its start is the clock's. The file is written through the card writer, off
 * the thread that asks.
 */
#include <stdio.h>
#include <string.h>
#include <platform.h>              /* current_time */
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include "pico_stats.h"
#include "pico_clock.h"
#include "card_writer.h"
#include "fat_ro.h"
#include "pico_libmem.h"
#include "gba_sd_save.h"           /* gba_sd_legacy_key */

extern fat_vol *gba_sd_menu_vol(void);

#define ST_DIR   "/gammaos"
#define ST_FILE  "playtime.txt"
#define ST_MAX   4096                  /* games remembered; the one played longest ago makes room */
#define ST_TEXT  (ST_MAX * 160u)
#define ST_EVERY (5u * 60u * 1000u)    /* a session in play is saved this often, ms */

struct st_game { unsigned char type; unsigned int secs; long long last; char name[128]; };
/* in the library's window (pico_libmem.c): a big library plays many games */
static struct st_game *s_g;
static int s_n, s_loaded;
static int s_cur = -1;                 /* the game in play */
static long long s_cur_was;            /* its last played before this start, to take back */
static int s_cur_new;                  /* it was not in the table before this start */
static unsigned int s_saved_ms;               /* how far into it the time is added */
static char *s_text;
static mutex_t s_mtx;
static volatile int s_mtx_up;

static void lock(void)
{
	enter_critical_section();
	if (!s_mtx_up) { mutex_init(&s_mtx); s_mtx_up = 1; }
	exit_critical_section();
	mutex_acquire(&s_mtx);
	if (!s_g) s_g = (struct st_game *)pico_lib_alloc(ST_MAX * (unsigned)sizeof(struct st_game));
	if (!s_text) s_text = (char *)pico_lib_alloc(ST_TEXT);
}
static void unlock(void) { mutex_release(&s_mtx); }

static void load(void)
{
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	uint32_t n;
	char *p, *end;
	s_loaded = 1;
	s_n = 0;
	if (!s_g || !s_text || !v || fat_open(v, ST_DIR "/" ST_FILE, &f) != 0) return;
	n = f.size < ST_TEXT - 1 ? f.size : ST_TEXT - 1;
	if (fat_read(&f, 0, s_text, n) != n) return;
	s_text[n] = 0;
	for (p = s_text, end = s_text + n; p < end && s_n < ST_MAX; ) {
		char *e = p;
		unsigned int type = 0, secs = 0;
		long long last = 0;
		while (e < end && *e != '\n') e++;
		*e = 0;
		while (*p >= '0' && *p <= '9') type = type * 10 + (unsigned)(*p++ - '0');
		if (*p == ' ') p++;
		while (*p >= '0' && *p <= '9') secs = secs * 10 + (unsigned)(*p++ - '0');
		if (*p == ' ') p++;
		while (*p >= '0' && *p <= '9') last = last * 10 + (*p++ - '0');
		if (*p == ' ' && p[1] && type < 256) {
			struct st_game *g = &s_g[s_n++];
			int k;
			p++;
			g->type = (unsigned char)type; g->secs = secs; g->last = last;
			for (k = 0; p[k] && p[k] != '\r' && k < (int)sizeof g->name - 1; k++) g->name[k] = p[k];
			g->name[k] = 0;
		}
		p = e + 1;
	}
}

/* a game's line: under its name, else (a name with characters outside ASCII) under the form
 * versions before 1.1 kept it under, when no other game has that form; add renames that one to the
 * name, so it is not there twice */
static int find(int type, const char *name, int add)
{
	int i;
	char legacy[128];
	if (!s_g) return -1;                  /* no memory for the table: nothing kept */
	for (i = 0; i < s_n; i++)
		if (s_g[i].type == type && !strcmp(s_g[i].name, name)) return i;
	if (gba_sd_legacy_own(name, legacy, sizeof legacy))
		for (i = 0; i < s_n; i++)
			if (s_g[i].type == type && !strcmp(s_g[i].name, legacy)) {
				if (add) snprintf(s_g[i].name, sizeof s_g[i].name, "%s", name);
				return i;
			}
	if (!add) return -1;
	if (s_n == ST_MAX) {	/* full: the game played longest ago makes room */
		int old = 0;
		for (i = 1; i < s_n; i++) if (s_g[i].last < s_g[old].last) old = i;
		if (old == s_cur) return -1;
		i = old;
	} else i = s_n++;
	s_g[i].type = (unsigned char)type; s_g[i].secs = 0; s_g[i].last = 0;
	snprintf(s_g[i].name, sizeof s_g[i].name, "%s", name);
	return i;
}

/* the time played since the last save added to the game in play, and the file written */
static void save(void)
{
	unsigned int now = (unsigned int)current_time();
	int i, n = 0;
	fat_vol *v = gba_sd_menu_vol();
	if (s_cur >= 0) {
		s_g[s_cur].secs += (now - s_saved_ms) / 1000u;
		s_saved_ms = now - (now - s_saved_ms) % 1000u;   /* the part of a second left over stays */
	}
	if (!s_g || !s_text) return;
	for (i = 0; i < s_n && n < (int)ST_TEXT - 160; i++)
		n += snprintf(s_text + n, ST_TEXT - n, "%u %u %lld %s\n", s_g[i].type, s_g[i].secs, s_g[i].last, s_g[i].name);
	if (v) card_writer_put(v, ST_DIR, ST_FILE, s_text, (uint32_t)n, 0, 0, 0, 0);
}

void pico_stats_begin(int type, const char *name)
{
	if (!name || !name[0]) return;
	lock();
	if (!s_loaded) load();
	if (s_cur >= 0) save();
	{
		int had = s_n, i = find(type, name, 0);
		/* found under its older form: from now on it is kept under the name */
		if (i >= 0 && strcmp(s_g[i].name, name)) snprintf(s_g[i].name, sizeof s_g[i].name, "%s", name);
		s_cur = i >= 0 ? i : find(type, name, 1);
		s_cur_new = i < 0 && s_n > had;
	}
	if (s_cur >= 0) {
		s_cur_was = s_g[s_cur].last;
		s_g[s_cur].last = pico_clock_utc();
		s_saved_ms = (unsigned int)current_time();
		save();
	}
	unlock();
}

void pico_stats_end(void)
{
	lock();
	if (s_cur >= 0) { save(); s_cur = -1; }
	unlock();
}

void pico_stats_abort(void)
{
	lock();
	if (s_cur >= 0) {	/* the game did not start: as if it had not been picked */
		if (s_cur_new) { s_g[s_cur] = s_g[s_n - 1]; s_n--; }
		else s_g[s_cur].last = s_cur_was;
		s_cur = -1;
		save();
	}
	unlock();
}

void pico_stats_tick(void)
{
	if (s_cur < 0 || (unsigned int)current_time() - s_saved_ms < ST_EVERY) return;
	lock();
	if (s_cur >= 0 && (unsigned int)current_time() - s_saved_ms >= ST_EVERY) save();
	unlock();
}

/* oem persist: the games known, the one in play, and game i's line */
int pico_stats_debug(int i, char *buf, int cap)
{
	int n;
	lock();
	n = s_n;
	if (i >= 0 && i < s_n)
		snprintf(buf, cap, "%u %u %lld %s%s", s_g[i].type, s_g[i].secs, s_g[i].last, s_g[i].name, i == s_cur ? " (playing)" : "");
	else snprintf(buf, cap, "%d games, playing %d, loaded %d", s_n, s_cur, s_loaded);
	unlock();
	return n;
}
int pico_stats_get(int type, const char *name, unsigned int *secs, long long *last)
{
	int i, ok = 0;
	lock();
	if (!s_loaded) load();
	i = find(type, name, 0);
	if (i >= 0) {
		*secs = s_g[i].secs;
		if (i == s_cur) *secs += ((unsigned int)current_time() - s_saved_ms) / 1000u;
		*last = s_g[i].last;
		ok = 1;
	}
	unlock();
	return ok;
}
