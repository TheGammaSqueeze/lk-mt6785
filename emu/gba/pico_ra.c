/*
 * pico_ra.c - RetroAchievements in GammaOS Pico (see pico_ra.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * The blob (cores/ra-pico, rcheevos' rc_client) is loaded from /cores/ra-pico.blob into its memory
 * after the Wi-Fi memory (RA_MEM_PA, pico_bt.h): the image, then its heap, then a megabyte of
 * buffers of ours (the server calls waiting, the unlock queue, a file buffer), so none of it costs
 * the LK's own small heap.
 *
 * Server calls: the blob hands each one over (ra_request) and gets its answer later (answer).
 * From the menu, and before a game's first frame, they go to the network when it is up, and every
 * good answer to the calls that can be kept (signing in, a game's set, its session) is written to
 * /gammaos/ra/c; without a network that copy answers instead. During play nothing goes out: the
 * blob answers unlocks itself and hands each one over to be kept (/gammaos/ra/queue.dat, through
 * the card writer), and the menu sends them when it can.
 *
 * The account: the name and rcheevos' token, masked for this console, in /gammaos/ra/account.dat.
 * The password is used once to sign in and never kept.
 */
#include <string.h>
#include <printf.h>
#include <arch/arm/mmu.h>
#include <platform/boot_mode.h>
#include <platform/pico_bt.h>
#include "pico_ra.h"
#include "ra_core_abi.h"
#include "gba_core_abi.h"
#include "core_file.h"
#include "card_writer.h"
#include "gba_sd_save.h"
#include "../../platform/common/pico/pico_netsvc.h"
#include "pico_png.h"

extern BOOT_ARGUMENT *g_boot_arg;
extern unsigned gpt4_get_current_tick(void);
extern void mtk_wdt_restart(void);
extern void thread_sleep(unsigned int ms);
extern int pico_clock_is_set(void);
extern long long pico_clock_utc(void);

#define RA_BLOB_PATH	"/cores/ra-pico.blob"
#define RA_DIR		"/gammaos/ra"
#define RA_COPY_DIR	"/gammaos/ra/c"
#define RA_UA		"GammaOS-Pico/1.0.0"

/* our megabyte at the end of the RA memory */
#define RA_IO_PA	(RA_MEM_PA + RA_MEM_SIZE - 0x100000u)
#define RA_REQ_MAX	8
#define RA_QUEUE_MAX	256
#define RA_FBUF_OFF	0x8000u
#define RA_LIST_OFF	0xF0000u		/* the in-game menu's rows (64 KB) */
#define RA_FBUF_CAP	(RA_LIST_OFF - RA_FBUF_OFF)
#define RA_LIST_MAX	120
#define RA_BLOB_MAX	0x100000u		/* the image and its BSS: blob.ld's limit */

struct ra_req {
	unsigned used, id;
	int secure;
	char api[24], key[48], host[64], path[160];
	char post[640];
};
#define S_REQ	((struct ra_req *)RA_IO_PA)
#define S_QUEUE	((struct ra_queued *)(RA_IO_PA + 0x4000u))
#define S_FBUF	((unsigned char *)(RA_IO_PA + RA_FBUF_OFF))
struct ra_row { char title[96], value[32], text[160]; int unlocked, sw, pref; };	/* sw: -1 none, 0/1 a switch */
#define S_LIST	((struct ra_row *)(RA_IO_PA + RA_LIST_OFF))

static fat_vol *s_vol;
static int s_mapped, s_load_failed;
static const struct ra_core_exports *X;

static char s_user[64], s_token[64];		/* the account kept on the card ("" none) */
static int s_state = PICO_RA_SIGNED_OUT;
static unsigned s_points;
static char s_note[96];
static int s_nq;				/* unlocks in S_QUEUE */
static unsigned s_sign_in_at;			/* ms of the kept account's last try (0 never) */
static int s_login_refused;			/* the server said no to the last sign in */

static int s_fake, s_fake_login;		/* the test server (pico_ra_debug) */
static volatile int s_dbg_in;			/* oem cheevos:in: a sign in waiting for the menu */
static char s_dbg_user[64], s_dbg_pass[64];

static unsigned s_http_id;			/* the call on the network now (0 none) */
static int s_http_award;			/* that call is a kept unlock's (S_QUEUE[0]) */
static unsigned s_award_wait_until;		/* ms: no unlock sent before (after a failure) */

static int s_console;				/* rcheevos' console of the game picked, 0 none */
static int s_ra_system;				/* the game picked is of a system RetroAchievements knows */
static int s_in_game;				/* from the game's pick to the menu again */
static int s_game_on;				/* the game's set is loaded and counting */
static int s_game_seen;				/* RA_EV_GAME came for the game loading */
static long long s_utc0;			/* the time at the game's start, for the blob's clock */
static unsigned s_utc0_ms;

/* ---- small things ---- */

static unsigned now_ms(void)
{
	extern unsigned int ayaneo_bb_uptime_ms(void);
	return ayaneo_bb_uptime_ms();
}

static void note(const char *s)
{
	strncpy(s_note, s ? s : "", sizeof s_note - 1);
	s_note[sizeof s_note - 1] = 0;
}

static void copy_n(char *dst, unsigned cap, const char *src)
{
	unsigned i = 0;
	if (!cap) return;
	if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
	dst[i] = 0;
}

/* a kept answer's file name: the call and its key, only letters, digits, '-' and '_' */
static void copy_name(char *out, unsigned cap, const char *api, const char *key)
{
	unsigned n = 0;
	const char *s;
	for (s = api; *s && n + 2 < cap; s++) out[n++] = *s;
	out[n++] = '-';
	for (s = key; *s && n + 1 < cap && n < 72; s++) {
		char c = *s;
		out[n++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? c : '_';
	}
	out[n] = 0;
}

static int keepable(const char *api)
{
	return !strcmp(api, "login2") || !strcmp(api, "achievementsets") || !strcmp(api, "startsession") ||
	       !strcmp(api, "gameid") || !strcmp(api, "patch");
}

/* the RA memory: DRAM the preloader marked usable, mapped cached (the blob runs from it) */
static int ra_map(void)
{
	unsigned i;
	int ok = 0;
	if (s_mapped) return 0;
	if (!g_boot_arg) return -1;
	for (i = 0; i < g_boot_arg->mblock_info.mblock_num && i < 128u; i++) {
		unsigned long long s = g_boot_arg->mblock_info.mblock[i].start;
		unsigned long long e = s + g_boot_arg->mblock_info.mblock[i].size;
		if (s <= RA_MEM_PA && RA_MEM_PA + (unsigned long long)RA_MEM_SIZE <= e) ok = 1;
	}
	if (!ok || arch_mmu_map((uint64_t)RA_MEM_PA, (vaddr_t)RA_MEM_PA,
				MMU_MEMORY_TYPE_NORMAL_WRITE_BACK | MMU_MEMORY_AP_P_RW_U_NA, RA_MEM_SIZE) != 0)
		return -1;
	memset((void *)RA_IO_PA, 0, RA_FBUF_OFF);
	s_mapped = 1;
	return 0;
}

/* ---- badges ----
 * The pictures of a game's achievements, 48 x 48 RGB565, kept on the card as one pack per game
 * (/gammaos/ra/b/<game id>.bdg: "GPBG", version, count, then name and pixels each) and read whole at
 * the game's start into the 512 KB below our buffers, so a card in play draws its badge with nothing
 * read. Badges the pack lacks are fetched in the menu when there is a network (media.retroachievements
 * .org), shrunk by pico_png and added to the pack. */
#define RA_BADGE_PA	(RA_IO_PA - 0x80000u)
#define BDG_PX		(PICO_RA_BADGE * PICO_RA_BADGE)
#define BDG_MAX		100
struct bdg { char name[8]; unsigned short px[BDG_PX]; };
#define S_BDG	((struct bdg *)RA_BADGE_PA)
static unsigned s_bdg_game;			/* the game the badges are of (0 none) */
static int s_bdg_n;				/* badges in memory */
static char s_bdg_want[BDG_MAX][8];		/* badges of that game not fetched yet */
static int s_bdg_wantn, s_bdg_dirty;
static int s_http_badge;			/* the call on the network is a badge's */

const unsigned short *pico_ra_badge(const char *name)
{
	int i;
	if (!name || !name[0] || !s_mapped) return 0;
	for (i = 0; i < s_bdg_n; i++)
		if (!strncmp(S_BDG[i].name, name, sizeof S_BDG[i].name)) return S_BDG[i].px;
	return 0;
}

/* the pack back on the card (the menu's thread) */
static void badges_store(void)
{
	char name[16];
	unsigned len = 8 + (unsigned)s_bdg_n * sizeof(struct bdg);
	if (!s_bdg_dirty || !s_bdg_game || !s_vol || len > RA_FBUF_CAP) return;
	memcpy(S_FBUF, "GPBG", 4);
	S_FBUF[4] = 1; S_FBUF[5] = 0;
	S_FBUF[6] = (unsigned char)s_bdg_n; S_FBUF[7] = (unsigned char)(s_bdg_n >> 8);
	memcpy(S_FBUF + 8, S_BDG, (unsigned)s_bdg_n * sizeof(struct bdg));
	snprintf(name, sizeof name, "%u", s_bdg_game);
	gba_sd_write_named(s_vol, RA_DIR "/b", name, "bdg", S_FBUF, len);
	s_bdg_dirty = 0;
}

/* the game's pack from the card, and the badges of its set it lacks (the game is loaded) */
static void badges_load(void)
{
	struct ra_summary sm;
	char name[16];
	unsigned n, cnt, i, k, len;
	badges_store();			/* the last game's badges fetched so far, first */
	X->summary(&sm);
	s_bdg_n = s_bdg_wantn = s_bdg_dirty = 0;
	s_bdg_game = sm.game_id;
	if (!s_bdg_game || !s_vol) return;
	snprintf(name, sizeof name, "%u", s_bdg_game);
	len = gba_sd_read_named(s_vol, RA_DIR "/b", name, "bdg", S_FBUF, 8);
	if (len == 8 && !memcmp(S_FBUF, "GPBG", 4) && S_FBUF[4] == 1) {
		cnt = S_FBUF[6] | (unsigned)S_FBUF[7] << 8;
		if (cnt > BDG_MAX) cnt = BDG_MAX;
		n = gba_sd_read_named(s_vol, RA_DIR "/b", name, "bdg", S_FBUF, RA_FBUF_CAP);
		if (n >= 8 + cnt * sizeof(struct bdg)) {
			memcpy(S_BDG, S_FBUF + 8, cnt * sizeof(struct bdg));
			s_bdg_n = (int)cnt;
		}
	}
	/* the set's badges not in the pack: to fetch */
	n = (unsigned)X->achievement_count();
	for (i = 0; i < n && s_bdg_wantn < BDG_MAX; i++) {
		struct ra_achievement a;
		if (X->achievement((int)i, &a) != 0 || !a.badge[0] || pico_ra_badge(a.badge)) continue;
		for (k = 0; k < (unsigned)s_bdg_wantn && strncmp(s_bdg_want[k], a.badge, 8); k++) ;
		if (k == (unsigned)s_bdg_wantn && s_bdg_n + s_bdg_wantn < BDG_MAX)
			copy_n(s_bdg_want[s_bdg_wantn++], sizeof s_bdg_want[0], a.badge);
	}
}

/* a fetched badge's PNG (in the download buffer): shrunk into the pack */
static int badge_take(const char *want, const unsigned char *png, unsigned len)
{
	/* the file buffer's halves: the PNG, then the shrunk picture and pico_png's work */
	unsigned char *art = S_FBUF + RA_FBUF_CAP / 2, *work = art + 0x2000u;
	int r;
	if (s_bdg_n >= BDG_MAX || len < 8 || len > RA_FBUF_CAP / 2) return -100;
	memcpy(S_FBUF, png, len);			/* (pico_png works on the file in place) */
	r = pico_png_to_art(S_FBUF, len, PICO_RA_BADGE, work, RA_FBUF_CAP / 2 - 0x2000u, art, 0x2000u, 0);
	if (r != 12 + BDG_PX * 2) return r;		/* (a badge not square, or broken: left out) */
	copy_n(S_BDG[s_bdg_n].name, sizeof S_BDG[0].name, want);
	memcpy(S_BDG[s_bdg_n].px, art + 12, BDG_PX * 2);
	s_bdg_n++;
	s_bdg_dirty = 1;
	return 0;
}

/* ---- the card ---- */

#define ACCOUNT_LEN (8 + 64 + 64)

static void account_store(void)
{
	unsigned char f[ACCOUNT_LEN];
	if (!s_vol) return;
	memset(f, 0, sizeof f);
	memcpy(f, "GPRA", 4);
	f[4] = 1;
	copy_n((char *)f + 8, 64, s_user);
	copy_n((char *)f + 72, 64, s_token);
	netsvc_mask(f + 8, ACCOUNT_LEN - 8, "pico ra account");
	gba_sd_write_named(s_vol, RA_DIR, "account", "dat", f, s_user[0] ? ACCOUNT_LEN : 8);
	memset(f, 0, sizeof f);
}

static void account_load(void)
{
	unsigned char f[ACCOUNT_LEN];
	unsigned n;
	s_user[0] = s_token[0] = 0;
	n = gba_sd_read_named(s_vol, RA_DIR, "account", "dat", f, sizeof f);
	if (n == ACCOUNT_LEN && !memcmp(f, "GPRA", 4) && f[4] == 1) {
		netsvc_mask(f + 8, ACCOUNT_LEN - 8, "pico ra account");
		copy_n(s_user, sizeof s_user, (char *)f + 8);
		copy_n(s_token, sizeof s_token, (char *)f + 72);
	}
	memset(f, 0, sizeof f);
}

/* ---- the player's settings: /gammaos/ra/prefs.dat, "GPRP", version, then one byte each ---- */

enum { PREF_ON, PREF_UNLOCK_CARDS, PREF_PROGRESS_CARDS, PREF_START_CARD, PREF_HARDCORE, PREFS };
/* PREF_HARDCORE: off, on, or automatic: as RetroAchievements' verdict on this client says (hardcore
 * once it has validated GammaOS Pico, softcore until then) */
enum { HC_OFF, HC_ON, HC_AUTO };
static unsigned char s_pref[PREFS] = { 1, 1, 1, 1, HC_AUTO };
static int s_paused_by_pref;			/* achievements turned off in this game: counts start again */

static void prefs_load(void)
{
	unsigned char f[16];
	unsigned n = gba_sd_read_named(s_vol, RA_DIR, "prefs", "dat", f, sizeof f), i;
	if (n < 8 + PREFS || memcmp(f, "GPRP", 4) || f[4] != 1) return;
	for (i = 0; i < PREFS; i++) s_pref[i] = i == PREF_HARDCORE ? (f[8 + i] <= HC_AUTO ? f[8 + i] : HC_AUTO) : f[8 + i] ? 1 : 0;
}

static void prefs_store(void)
{
	unsigned char f[16];
	if (!s_vol) return;
	memset(f, 0, sizeof f);
	memcpy(f, "GPRP", 4);
	f[4] = 1;
	memcpy(f + 8, s_pref, PREFS);
	if (s_in_game) card_writer_put(s_vol, RA_DIR, "prefs.dat", f, sizeof f, 0, 0, 0, 0);
	else gba_sd_write_named(s_vol, RA_DIR, "prefs", "dat", f, sizeof f);
}

/* ---- hardcore ----
 * The verdict: RetroAchievements' word on this client from the last set read online (1 validated,
 * 0 not, -1 not known), kept in /gammaos/ra/verdict.dat ("GPRV", version 1, the verdict, the
 * time) so a start knows it before the network answers. A session is hardcore when the player
 * says so (on, or automatic with a validated client) and its set came from the network; it drops
 * to softcore when RetroAchievements says it does not validate us and the player left it to the
 * verdict. s_hc_why says why a session is softcore when hardcore was asked for. */
static int s_verdict = -1;
static long long s_verdict_utc;
static int s_session_online;			/* the game's set came from the network */
static char s_hc_why[80];
static volatile int s_reset_wanted;		/* RA_EV_RESET: the runner restarts its game */

static void verdict_load(void)
{
	unsigned char f[16];
	unsigned n = gba_sd_read_named(s_vol, RA_DIR, "verdict", "dat", f, sizeof f);
	if (n < 16 || memcmp(f, "GPRV", 4) || f[4] != 1) return;
	s_verdict = (signed char)f[5];
	memcpy(&s_verdict_utc, f + 8, 8);
}

static void verdict_store(void)
{
	unsigned char f[16];
	if (!s_vol) return;
	memset(f, 0, sizeof f);
	memcpy(f, "GPRV", 4);
	f[4] = 1;
	f[5] = (unsigned char)(signed char)s_verdict;
	memcpy(f + 8, &s_verdict_utc, 8);
	if (s_in_game) card_writer_put(s_vol, RA_DIR, "verdict.dat", f, sizeof f, 0, 0, 0, 0);
	else gba_sd_write_named(s_vol, RA_DIR, "verdict", "dat", f, sizeof f);
}

/* hardcore as the player wants it now (before the network's say for this session) */
static int hc_wanted(void)
{
	return s_pref[PREF_HARDCORE] == HC_ON || (s_pref[PREF_HARDCORE] == HC_AUTO && s_verdict == 1);
}

/* rcheevos counts in hardcore now */
static int hc_now(void)
{
	struct ra_summary sm;
	if (!X || !s_game_on) return 0;
	X->summary(&sm);
	return sm.hardcore;
}

/* the queue as a file: "GPRQ", version, count, then the records */
static unsigned queue_image(unsigned char *out, unsigned cap)
{
	unsigned len = 8 + (unsigned)s_nq * sizeof(struct ra_queued);
	if (len > cap) return 0;
	memcpy(out, "GPRQ", 4);
	out[4] = 2;					/* 2: each record says whether it was earned in hardcore */
	out[5] = (unsigned char)(s_nq & 0xFF);
	out[6] = (unsigned char)(s_nq >> 8);
	out[7] = 0;
	memcpy(out + 8, S_QUEUE, (unsigned)s_nq * sizeof(struct ra_queued));
	return len;
}

static void queue_load(void)
{
	unsigned n = gba_sd_read_named(s_vol, RA_DIR, "queue", "dat", S_FBUF, RA_FBUF_CAP), cnt;
	s_nq = 0;
	if (n < 8 || memcmp(S_FBUF, "GPRQ", 4) || (S_FBUF[4] != 1 && S_FBUF[4] != 2)) return;
	cnt = S_FBUF[5] | (unsigned)S_FBUF[6] << 8;
	if (cnt > RA_QUEUE_MAX || 8 + cnt * sizeof(struct ra_queued) > n) return;
	memcpy(S_QUEUE, S_FBUF + 8, cnt * sizeof(struct ra_queued));
	s_nq = (int)cnt;
	if (S_FBUF[4] == 1) {		/* version 1: all softcore, and that byte was padding */
		unsigned i;
		for (i = 0; i < cnt; i++) S_QUEUE[i].hardcore = 0;
	}
}

/* in the menu: written here; during play: through the card writer (which copies it at once). The
 * file buffer holds the image: nothing else is in it then (a kept answer is used up before) */
static void queue_store(int playing)
{
	unsigned len;
	if (!s_vol || !s_mapped) return;
	len = queue_image(S_FBUF, RA_FBUF_CAP);
	if (!len) return;
	if (playing) card_writer_put(s_vol, RA_DIR, "queue.dat", S_FBUF, len, 0, 0, 0, 0);
	else gba_sd_write_named(s_vol, RA_DIR, "queue", "dat", S_FBUF, len);
}

/* ---- the blob's services ---- */

static void im_request(unsigned id, const char *url, const char *post, const char *ctype, const char *api, const char *key)
{
	struct ra_req *r = 0;
	const char *h, *slash;
	int i;
	(void)ctype;
	for (i = 0; i < RA_REQ_MAX; i++)
		if (!S_REQ[i].used) { r = &S_REQ[i]; break; }
	if (!r) return;					/* unanswered: rc_client times out and tries again */
	memset(r, 0, sizeof *r);
	r->id = id;
	copy_n(r->api, sizeof r->api, api);
	copy_n(r->key, sizeof r->key, key);
	r->secure = !strncmp(url, "https://", 8);
	h = strstr(url, "://");
	h = h ? h + 3 : url;
	slash = strchr(h, '/');
	if (!slash || (unsigned)(slash - h) >= sizeof r->host || strlen(slash) >= sizeof r->path ||
	    (post && strlen(post) >= sizeof r->post))
		return;					/* (never: rcheevos' addresses are short) */
	memcpy(r->host, h, (unsigned)(slash - h));
	copy_n(r->path, sizeof r->path, slash);
	copy_n(r->post, sizeof r->post, post ? post : "");
	r->used = 1;
}

static unsigned long long im_now_ms(void) { return now_ms(); }

/* the time of day from the snapshot taken on this thread at the game's start (or now, from the
 * menu: the RTC is read on the menu's thread only) */
static long long im_utc(void)
{
	if (!s_utc0) return 0;
	return s_utc0 + (long long)((now_ms() - s_utc0_ms) / 1000u);
}

/* rcheevos' warnings and errors, the last few, for oem ra */
#define LOG_LINES 8
static char s_log[LOG_LINES][80];
static unsigned s_log_n;
static void im_log(const char *line)
{
	copy_n(s_log[s_log_n++ % LOG_LINES], sizeof s_log[0], line);
}

/* oem cheevos: line i of the state (0 when there are no more) */
int pico_ra_debug_line(int i, char *buf, unsigned cap)
{
	if (i == 0) {
		snprintf(buf, cap, "ra: state %d user %s points %u queue %d blob %s game %d console %d http %u", pico_ra_state(),
			 s_user[0] ? s_user : "-", s_points, s_nq, X ? "loaded" : s_load_failed ? "failed" : "no", s_game_on,
			 s_console, s_http_id);
		return 1;
	}
	if (i == 1) {
		snprintf(buf, cap, "ra: hardcore %d pref %d verdict %d online %d why %s", hc_now(), s_pref[PREF_HARDCORE],
			 s_verdict, s_session_online, s_hc_why);
		return 1;
	}
	i--;
	if (i == 1 && s_bdg_game) {	/* (shifts the lines below by one while a game's badges are known) */
		snprintf(buf, cap, "ra: badges game %u have %d want %d", s_bdg_game, s_bdg_n, s_bdg_wantn);
		return 1;
	}
	if (s_bdg_game) i--;
	if (i == 1) { snprintf(buf, cap, "ra: note %s", s_note); return 1; }
	i -= 2;
	if (i < 0 || (unsigned)i >= LOG_LINES || (unsigned)i >= s_log_n) return 0;
	copy_n(buf, cap, s_log[(s_log_n - 1u - (unsigned)i) % LOG_LINES]);
	return 1;
}

static const struct ra_core_imports s_imports = { im_request, im_now_ms, im_utc, im_log };

static void clock_snapshot(void)
{
	s_utc0 = pico_clock_is_set() ? pico_clock_utc() : 0;
	s_utc0_ms = now_ms();
}

/* ---- the blob ---- */

/* Our own user agent, always (RetroAchievements validates clients by it): "GammaOS-Pico/1.0.0
 * (LK; <core>, <the emulator it comes from>) rcheevos/x.y.z". The core is named so that
 * RetroAchievements can see what runs each game; it never claims to be another client. */
static void ua_set(const char *core)
{
	char ua[160], clause[48];
	clause[0] = 0;
	if (X) X->user_agent_clause(clause, sizeof clause);
	if (core && core[0]) snprintf(ua, sizeof ua, "%s (LK; %s) %s", RA_UA, core, clause);
	else snprintf(ua, sizeof ua, "%s (LK) %s", RA_UA, clause);
	netsvc_user_agent(ua);
}

/* the core that runs a game of rcheevos' console c, and its upstream */
static const char *core_credit(int c)
{
	switch (c) {
	case 5: {	/* GBA: the core picked for this game */
		extern const struct gba_core_exports *gba_core_current(void) __attribute__((weak));
		const struct gba_core_exports *g = gba_core_current ? gba_core_current() : 0;
		const char *n = g && g->version >= 3 && g->name ? g->name : "";
		if (!strncmp(n, "armGBA", 6)) return "armGBA, derived from mGBA";
		if (!strncmp(n, "gpSP", 4) || !strncmp(n, "gpsp", 4)) return "gpSP";
		return "armGBA, derived from mGBA";
	}
	case 4: case 6: return "gambatte";
	case 3: return "snes9x";
	case 7: return "armNES";
	case 1: case 11: case 15: case 33: return "Genesis Plus GX";
	default: return "";
	}
}

static int ra_load(void)
{
	ra_core_entry_fn entry;
	const unsigned *hdr = (const unsigned *)RA_MEM_PA;
	unsigned span, heap;
	int err;
	if (X) return 0;
	if (s_load_failed || !s_vol || ra_map() != 0) { s_load_failed = 1; return -1; }
	entry = (ra_core_entry_fn)core_file_load(s_vol, RA_BLOB_PATH, RA_MEM_PA, RA_BLOB_MAX, RA_CORE_ABI_MAGIC,
						 RA_CORE_ABI_VERSION, RA_CORE_ABI_VERSION, &err);
	if (!entry) { s_load_failed = 1; note("RetroAchievements is missing from the card (/cores)"); return -1; }
	X = entry(&s_imports);
	if (!X || X->magic != RA_CORE_ABI_MAGIC || X->version != RA_CORE_ABI_VERSION) {
		X = 0;
		s_load_failed = 1;
		return -1;
	}
	span = (hdr[4] + 63u) & ~63u;
	heap = RA_BADGE_PA - (RA_MEM_PA + span);
	X->heap_init((void *)(RA_MEM_PA + span), heap);
	if (X->init() != 0) { X = 0; s_load_failed = 1; return -1; }
	ua_set(0);
	return 0;
}

/* ---- the cards over the game ---- */

#define CARDS 4
#define CARD_MS 4000u
static struct pico_ra_card s_cards[CARDS];
static unsigned s_card_head, s_card_tail, s_card_t0;

static struct pico_ra_card *card_new(int kind)
{
	static struct pico_ra_card dropped;	/* a card the player turned off: filled in, never shown */
	struct pico_ra_card *c;
	int pref = kind == PICO_RA_CARD_UNLOCK || kind == PICO_RA_CARD_MASTERED ? PREF_UNLOCK_CARDS :
		   kind == PICO_RA_CARD_PROGRESS ? PREF_PROGRESS_CARDS : kind == PICO_RA_CARD_GAME ? PREF_START_CARD : -1;
	if (pref >= 0 && !s_pref[pref]) {
		memset(&dropped, 0, sizeof dropped);
		return &dropped;
	}
	if (s_card_tail - s_card_head >= CARDS) s_card_head++;
	c = &s_cards[s_card_tail++ % CARDS];
	memset(c, 0, sizeof *c);
	c->kind = kind;
	c->points = -1;
	if (s_card_tail - s_card_head == 1) s_card_t0 = now_ms();
	return c;
}

int pico_ra_card_now(struct pico_ra_card *out)
{
	unsigned t, ms = now_ms();
	while (s_card_head != s_card_tail) {
		t = ms - s_card_t0;
		if (t < CARD_MS) {
			*out = s_cards[s_card_head % CARDS];
			out->alpha = t < 250u ? t * 255u / 250u : t > CARD_MS - 400u ? (CARD_MS - t) * 255u / 400u : 255u;
			return 1;
		}
		s_card_head++;
		s_card_t0 = ms;
	}
	return 0;
}

/* ---- events ---- */

static void events(int playing)
{
	struct ra_event e;
	struct pico_ra_card *c;
	if (!X) return;
	while (X->next_event(&e)) {
		switch (e.kind) {
		case RA_EV_LOGIN:
			if (e.ok) {
				s_state = PICO_RA_SIGNED_IN;
				s_points = (unsigned)e.value;
				{	/* the token kept (written only when it is new) */
					char t[64];
					if (!s_fake && X->token(t, sizeof t) == 0 && s_user[0] && strcmp(t, s_token)) {
						copy_n(s_token, sizeof s_token, t);
						account_store();
					}
					memset(t, 0, sizeof t);
				}
				snprintf(s_note, sizeof s_note, "Signed in as %s", e.title);
			} else {
				s_state = PICO_RA_SIGNED_OUT;
				if (s_login_refused) {		/* a wrong password, or a token no longer good */
					s_user[0] = s_token[0] = 0;
					account_store();
				}
				note(e.text[0] ? e.text : "Could not sign in");
			}
			break;
		case RA_EV_GAME:
			s_game_seen = 1;
			s_game_on = e.ok;
			if (e.ok && !playing) badges_load();	/* (before play: the card may be read) */
			if (e.ok) {
				struct ra_summary sm;
				X->summary(&sm);
				if (s_session_online && !s_fake && sm.verdict >= 0 && sm.verdict != s_verdict) {	/* (never the test server's) */
					s_verdict = sm.verdict;
					s_verdict_utc = pico_clock_is_set() ? pico_clock_utc() : 0;
					verdict_store();
				}
				if (sm.hardcore && sm.verdict == 0 && s_pref[PREF_HARDCORE] == HC_AUTO) {
					X->set_hardcore(0);		/* (going to softcore is allowed at any time) */
					sm.hardcore = 0;
					copy_n(s_hc_why, sizeof s_hc_why, "RetroAchievements does not validate this client yet");
				}
				c = card_new(PICO_RA_CARD_GAME);
				copy_n(c->head, sizeof c->head, sm.hardcore ? "RetroAchievements: hardcore" : "RetroAchievements: softcore");
				copy_n(c->title, sizeof c->title, e.title);
				if (!sm.hardcore && s_hc_why[0] && hc_wanted())
					copy_n(c->line, sizeof c->line, s_hc_why);
				else
					snprintf(c->line, sizeof c->line, "%d of %u achievements unlocked", e.value, e.id);
			} else {
				char l[80];
				snprintf(l, sizeof l, "game: no set: %s", e.text[0] ? e.text : "(no reason)");
				im_log(l);
			}
			if (!e.ok && !strstr(e.text, "Unknown game")) {
				/* a game with a set that could not be had (no network, no copy kept yet): said once;
				 * a game RetroAchievements does not know stays quiet */
				c = card_new(PICO_RA_CARD_NOTE);
				copy_n(c->head, sizeof c->head, "RetroAchievements");
				copy_n(c->title, sizeof c->title, "Achievements are off for now");
				snprintf(c->line, sizeof c->line, "%s", strstr(e.text, "esponse") || !e.text[0] ?
					 "No network, and this game was never played online: connect once" : e.text);
			}
			break;
		case RA_EV_RESET:		/* hardcore came on: the game starts again (pico_ra_reset_wanted) */
			s_reset_wanted = 1;
			break;
		case RA_EV_UNLOCK:
			c = card_new(PICO_RA_CARD_UNLOCK);
			copy_n(c->badge, sizeof c->badge, e.badge);
			copy_n(c->head, sizeof c->head, "Achievement unlocked");
			copy_n(c->title, sizeof c->title, e.title);
			copy_n(c->line, sizeof c->line, e.text);
			c->points = e.value;
			s_points += (unsigned)(e.value > 0 ? e.value : 0);
			break;
		case RA_EV_MASTERED:
			c = card_new(PICO_RA_CARD_MASTERED);
			copy_n(c->head, sizeof c->head, "Set mastered");
			copy_n(c->title, sizeof c->title, e.title);
			snprintf(c->line, sizeof c->line, "Every achievement, %d points", e.value);
			break;
		case RA_EV_PROGRESS: {
			/* a progress card only when the last card has gone: progress moves every frame */
			if (s_card_head != s_card_tail) break;
			c = card_new(PICO_RA_CARD_PROGRESS);
			copy_n(c->head, sizeof c->head, "Progress");
			copy_n(c->title, sizeof c->title, e.title);
			copy_n(c->line, sizeof c->line, e.text);
			break;
		}
		case RA_EV_QUEUED:
			if (s_nq < RA_QUEUE_MAX) {
				struct ra_queued *q = &S_QUEUE[s_nq++];
				memset(q, 0, sizeof *q);
				q->id = e.id;
				copy_n(q->hash, sizeof q->hash, e.hash);
				q->when = e.when;
				q->hardcore = e.ok ? 1 : 0;
				queue_store(playing);
			}
			break;
		case RA_EV_SERVER_ERROR:
			note(e.text);
			break;
		default:
			break;
		}
	}
}

/* ---- server calls ---- */

/* the kept answer, read into S_FBUF: its length, 0 none */
static unsigned copy_read(const char *api, const char *key)
{
	char name[96];
	unsigned n;
	copy_name(name, sizeof name, api, key);
	n = gba_sd_read_named(s_vol, RA_COPY_DIR, name, "json", S_FBUF, RA_FBUF_CAP - 1);
	if (n >= RA_FBUF_CAP - 1) return 0;		/* (cut short: not used) */
	S_FBUF[n] = 0;
	return n;
}

static void copy_write(const char *api, const char *key, const unsigned char *body, unsigned len)
{
	char name[96];
	copy_name(name, sizeof name, api, key);
	gba_sd_write_named(s_vol, RA_COPY_DIR, name, "json", body, len);
}

static int succeeded(const unsigned char *body, unsigned len)
{
	unsigned i;
	for (i = 0; i + 14 <= len && i < 64; i++)
		if (!memcmp(body + i, "\"Success\":true", 14)) return 1;
	return 0;
}

static void finish(struct ra_req *r, int status, const unsigned char *body, unsigned len)
{
	unsigned id = r->id;
	if (!strcmp(r->api, "login2")) s_login_refused = status == 401 || status == 403;
	memset(r, 0, sizeof *r);			/* (the password goes with it) */
	X->answer(id, status, (const char *)body, len);
}

static struct ra_req *req_find(unsigned id)
{
	int i;
	for (i = 0; i < RA_REQ_MAX; i++)
		if (S_REQ[i].used && S_REQ[i].id == id) return &S_REQ[i];
	return 0;
}

static void award_done(int status, const unsigned char *body, unsigned len)
{
	int r = X->award_answer(status, (const char *)body, len);
	if (r > 0 && body) {			/* the new score it gives */
		const char *k = strstr((const char *)body, "\"SoftcoreScore\":");
		if (k) {
			unsigned v = 0;
			for (k += 16; *k >= '0' && *k <= '9'; k++) v = v * 10 + (unsigned)(*k - '0');
			s_points = v;
		}
	}
	if (r != 0) {					/* the server has it, or never will: off the queue */
		if (r < 0) note("The server refused an unlock");
		memmove(S_QUEUE, S_QUEUE + 1, (unsigned)(s_nq - 1) * sizeof *S_QUEUE);
		s_nq--;
		queue_store(0);
	} else {
		s_award_wait_until = now_ms() + 60000u;	/* try again in a minute */
	}
}

/* oem cheevos:fake:1 - a test server inside the LK, for the console's own checks without an account:
 * any user signs in, every game has two achievements, one unlocking after 300 frames of play and one
 * after 200 frames that read the game's first byte (unsupported when the core's memory is not
 * there), unlocks are taken. Nothing it gives is kept on the card (no account, no copies); the unlock queue is. */
static unsigned fake_answer(const char *api, char *out, unsigned cap)
{
	if (!strcmp(api, "login2"))
		snprintf(out, cap, "{\"Success\":true,\"User\":\"pico-test\",\"DisplayName\":\"pico-test\",\"Token\":"
			 "\"pico-test-token\",\"Score\":0,\"SoftcoreScore\":0,\"Messages\":0}");
	else if (!strcmp(api, "achievementsets"))
		snprintf(out, cap, "{\"Success\":true,\"GameId\":1,\"Title\":\"Pico test\",\"ConsoleId\":%d,\"ImageIconUrl\":\"\","
			 "\"RichPresenceGameId\":1,\"RichPresencePatch\":\"\",\"Sets\":[{\"AchievementSetId\":1,\"GameId\":1,"
			 "\"Title\":\"Pico test\",\"Type\":\"core\",\"ImageIconUrl\":\"\",\"Achievements\":[{\"ID\":1,"
			 "\"Title\":\"Five seconds\",\"Description\":\"Play for five seconds\",\"Flags\":3,\"Points\":5,"
			 "\"MemAddr\":\"1=1.300.\",\"Author\":\"GammaOS\",\"BadgeName\":\"00000\",\"Created\":1,\"Modified\":1,"
			 "\"Type\":\"\",\"Rarity\":1,\"RarityHardcore\":1,\"BadgeURL\":\"\",\"BadgeLockedURL\":\"\"},"
			 "{\"ID\":2,\"Title\":\"Memory read\",\"Description\":\"The game's first byte, 200 frames\",\"Flags\":3,"
			 "\"Points\":1,\"MemAddr\":\"0xH0000=0xH0000.200.\",\"Author\":\"GammaOS\",\"BadgeName\":\"00100\","
			 "\"Created\":1,\"Modified\":1,\"Type\":\"\",\"Rarity\":1,\"RarityHardcore\":1,\"BadgeURL\":\"\","
			 "\"BadgeLockedURL\":\"\"}],\"Leaderboards\":[]}]}", s_console);
	else if (!strcmp(api, "startsession"))
		snprintf(out, cap, "{\"Success\":true,\"HardcoreUnlocks\":[],\"Unlocks\":[],\"ServerNow\":1}");
	else if (!strcmp(api, "awardachievement"))
		snprintf(out, cap, "{\"Success\":true,\"Score\":0,\"SoftcoreScore\":5,\"AchievementID\":1,"
			 "\"AchievementsRemaining\":0}");
	else
		snprintf(out, cap, "{\"Success\":true}");
	return (unsigned)strlen(out);
}

/* net: the network may be used (the menu, a game's start); card: the card may be read (not in play) */
static void pump(int net, int card)
{
	struct netsvc_status ns;
	int online, i;
	if (!X) return;
	if (s_http_id) {
		unsigned status = 0, len = 0;
		const unsigned char *body = 0;
		const char *why = "";
		int r = netsvc_http_poll(&status, &body, &len, &why);
		if (r == 0) return;
		if (s_http_badge) {	/* a badge: kept when it came, given up when not (fetched again next game) */
			char l[80];
			int t = r == 1 && status == 200 ? badge_take(s_bdg_want[0], body, len) : -200;
			snprintf(l, sizeof l, "badge %s: %s %u, %u bytes, %s %d", s_bdg_want[0], r == 1 ? "status" : "failed",
				 r == 1 ? status : 0, r == 1 ? len : 0, t ? "not kept" : "kept", t);
			im_log(l);
			memmove(s_bdg_want, s_bdg_want + 1, (unsigned)(s_bdg_wantn - 1) * sizeof s_bdg_want[0]);
			s_bdg_wantn--;
			if (!s_bdg_wantn || !(s_bdg_n % 10)) badges_store();
		} else if (s_http_award) {
			award_done(r == 1 ? (int)status : 0, r == 1 ? body : 0, r == 1 ? len : 0);
		} else {
			struct ra_req *q = req_find(s_http_id);
			if (q) {
				if (r == 1 && status == 200 && keepable(q->api) && succeeded(body, len) && card)
					copy_write(q->api, q->key, body, len);
				if (r == 1) finish(q, (int)status, body, len);
				else {
					unsigned n = card && keepable(q->api) ? copy_read(q->api, q->key) : 0;
					finish(q, n ? 200 : 0, n ? S_FBUF : 0, n);
				}
			}
		}
		if (r != -2) netsvc_http_release();
		s_http_id = 0;
		s_http_award = 0;
		s_http_badge = 0;
	}
	if (s_fake) {	/* the test server answers everything here and now */
		for (i = 0; i < RA_REQ_MAX; i++)
			if (S_REQ[i].used) {
				unsigned n = fake_answer(S_REQ[i].api, (char *)S_FBUF, 4096);
				finish(&S_REQ[i], 200, S_FBUF, n);
			}
		if (s_state == PICO_RA_SIGNED_IN && s_nq && !s_game_on) {
			static char url[256], post[640];
			if (X->award_request(&S_QUEUE[0], url, sizeof url, post, sizeof post) == 0) {
				unsigned n = fake_answer("awardachievement", (char *)S_FBUF, 4096);
				award_done(200, S_FBUF, n);
			}
		}
		/* its set's badges still come from the real picture server (nothing else of the
		 * account's ever reaches the network in this mode) */
		netsvc_get(&ns);
		online = net && ns.state == NETSVC_ONLINE;
		goto badges;
	}
	netsvc_get(&ns);
	online = net && ns.state == NETSVC_ONLINE;
	for (i = 0; i < RA_REQ_MAX; i++) {
		struct ra_req *q = &S_REQ[i];
		if (!q->used) continue;
		if (online) {
			if (netsvc_http_begin(q->host, q->path, q->secure, q->post[0] ? q->post : 0, (unsigned)strlen(q->post)) != 0)
				return;				/* the service is busy (box art): next time */
			s_http_id = q->id;
			memset(q->post, 0, sizeof q->post);	/* (sent: a password is not kept) */
			return;
		} else {
			unsigned n = card && keepable(q->api) ? copy_read(q->api, q->key) : 0;
			finish(q, n ? 200 : 0, n ? S_FBUF : 0, n);
		}
	}
	/* nothing of the blob's waiting: a kept unlock to the server */
	if (online && s_state == PICO_RA_SIGNED_IN && s_nq && !s_game_on && (int)(now_ms() - s_award_wait_until) >= 0) {
		static char url[256], post[640];
		struct ra_req tmp;
		const char *h, *slash;
		if (X->award_request(&S_QUEUE[0], url, sizeof url, post, sizeof post) != 0) {
			award_done(400, 0, 0);
			return;
		}
		h = strstr(url, "://");
		h = h ? h + 3 : url;
		slash = strchr(h, '/');
		if (!slash || (unsigned)(slash - h) >= sizeof tmp.host) return;
		memcpy(tmp.host, h, (unsigned)(slash - h));
		tmp.host[slash - h] = 0;
		if (netsvc_http_begin(tmp.host, slash, !strncmp(url, "https://", 8), post[0] ? post : 0, (unsigned)strlen(post)) != 0)
			return;
		s_http_id = 0xFFFFFFFFu;
		s_http_award = 1;
		return;
	}
badges:
	/* then the badges the last game lacked (the menu only, nothing else waiting) */
	if (online && s_bdg_wantn && !s_in_game) {
		char path[48];
		/* (LK's printf reads a precision as a width: %.8s would pad, never cut; the names end in a 0) */
		snprintf(path, sizeof path, "/Badge/%s.png", s_bdg_want[0]);
		if (netsvc_http_begin("media.retroachievements.org", path, 1, 0, 0) != 0) return;
		s_http_id = 0xFFFFFFFEu;
		s_http_badge = 1;
	}
}

/* ---- the menu ---- */

void pico_ra_card(fat_vol *v)
{
	if (!v || v == s_vol) return;
	s_vol = v;
	account_load();
	prefs_load();
	verdict_load();
	if (ra_map() == 0) queue_load();
	s_state = PICO_RA_SIGNED_OUT;
}

int pico_ra_state(void) { return s_load_failed && !X ? PICO_RA_NO_BLOB : s_state; }
const char *pico_ra_user(void) { return s_state == PICO_RA_SIGNED_IN || s_state == PICO_RA_SIGNING_IN ? s_user : ""; }
const char *pico_ra_note(void) { return s_note; }
unsigned pico_ra_points(void) { return s_points; }
unsigned pico_ra_waiting(void) { return (unsigned)s_nq; }

void pico_ra_sign_in(const char *user, const char *password)
{
	clock_snapshot();
	if (ra_load() != 0) return;
	if (s_state == PICO_RA_SIGNED_IN) X->logout();
	copy_n(s_user, sizeof s_user, user);
	s_token[0] = 0;
	s_state = PICO_RA_SIGNING_IN;
	note("Signing in");
	X->set_online(1);
	X->login_password(user, password);
}

/* the menu asks: kept unlocks go out now, not after the minute's wait a failure leaves */
void pico_ra_send_now(void) { s_award_wait_until = now_ms(); }

void pico_ra_sign_out(void)
{
	if (X && s_state == PICO_RA_SIGNED_IN) X->logout();
	s_user[0] = s_token[0] = 0;
	s_state = PICO_RA_SIGNED_OUT;
	s_points = 0;
	account_store();
	note("Signed out");
}

void pico_ra_menu_tick(void)
{
	static unsigned frames;
	if (!s_vol || s_in_game) return;		/* (a game's own menu: pico_ra_paused) */
	if (!(frames++ % 300)) clock_snapshot();
	/* the kept account, signed in once the menu has settled (from the network or the copy), and
	 * again every 30 s while it is not (no network and no copy yet, say), unless the server said no */
	if (s_state == PICO_RA_SIGNED_OUT && s_user[0] && s_token[0] && !s_login_refused && frames > 120 &&
	    (!s_sign_in_at || now_ms() - s_sign_in_at >= 30000u)) {
		s_sign_in_at = now_ms() | 1u;
		if (ra_load() == 0) {
			s_state = PICO_RA_SIGNING_IN;
			X->set_online(1);
			X->login_token(s_user, s_token);
		}
	}
	if (s_fake_login) {		/* oem cheevos:login (the test server): signed in from here */
		s_fake_login = 0;
		pico_ra_sign_in("pico-test", "pico-test");
	}
	if (s_dbg_in) {			/* oem cheevos:in:USER:PASS, as the Network tab's sign in */
		s_dbg_in = 0;
		pico_ra_sign_in(s_dbg_user, s_dbg_pass);
		memset(s_dbg_pass, 0, sizeof s_dbg_pass);
	}
	if (!X) return;
	X->set_online(1);
	X->idle();
	pump(1, 1);
	events(0);
}

/* oem cheevos:fake:1|0, oem cheevos:login (with the test server) */
void pico_ra_debug(const char *arg)
{
	if (!strncmp(arg, "fake:", 5)) s_fake = arg[5] == '1';
	else if (!strncmp(arg, "login", 5) && s_fake) s_fake_login = 1;
	else if (!strncmp(arg, "in:", 3)) {	/* the real server: the name up to the next ':', then the password */
		const char *u = arg + 3, *c = strchr(u, ':');
		unsigned n = c ? (unsigned)(c - u) : 0;
		if (c && n && n < sizeof s_dbg_user && strlen(c + 1) < sizeof s_dbg_pass) {
			memcpy(s_dbg_user, u, n);
			s_dbg_user[n] = 0;
			copy_n(s_dbg_pass, sizeof s_dbg_pass, c + 1);
			s_fake = 0;
			s_dbg_in = 1;
		}
	}
}

/* ---- a game ---- */

void pico_ra_game_begin(int type)
{
	static const unsigned char consoles[9] = { 4, 6, 5, 3, 1, 11, 15, 33, 7 };	/* GBA_CONSOLE_* order */
	s_console = type >= 0 && type < 9 ? consoles[type] : 0;
	s_ra_system = s_console;
	s_game_on = 0;
	s_paused_by_pref = 0;
	s_in_game = 1;
	netsvc_quiet(1);			/* (pico_ra_core_ready wakes it for the game's set, then it sleeps) */
}

void pico_ra_core_ready(const void *rom, unsigned rom_size, const struct pico_core_memory *mem)
{
	unsigned *ids = (unsigned *)S_FBUF, t0;	/* (the blob copies them) */
	int i;
	{
		char l[80];
		snprintf(l, sizeof l, "start: blob %d state %d console %d rom %u on %d http %u", X != 0, s_state, s_console,
			 rom_size, s_pref[PREF_ON], s_http_id);
		im_log(l);
	}
	if (!X || s_state != PICO_RA_SIGNED_IN || !s_console || !rom || !rom_size || !s_pref[PREF_ON]) { s_console = 0; return; }
	netsvc_quiet(0);
	clock_snapshot();
	ua_set(core_credit(s_console));		/* (the set and the session ask now) */
	for (i = 0; i < s_nq; i++) ids[i] = S_QUEUE[i].id | (S_QUEUE[i].hardcore ? 0x80000000u : 0);	/* (bit 31: hardcore) */
	X->set_local_unlocks(ids, s_nq);
	X->set_online(1);
	{	/* hardcore from the start, or softcore and why */
		struct netsvc_status ns;
		int want = hc_wanted();
		netsvc_get(&ns);
		s_session_online = ns.state == NETSVC_ONLINE;
		s_hc_why[0] = 0;
		if (want && !s_session_online) {
			want = 0;
			copy_n(s_hc_why, sizeof s_hc_why, "No network when the game started");
		}
		X->set_hardcore(want);
	}
	s_reset_wanted = 0;
	s_game_seen = 0;
	X->load_game((unsigned)s_console, rom, rom_size, mem);
	/* the set and the session: from the network (a few seconds at most) or the copies */
	t0 = now_ms();
	while (!s_game_seen && now_ms() - t0 < 10000u) {
		pump(1, 1);
		events(0);
		mtk_wdt_restart();
		thread_sleep(2);
	}
	if (!s_game_seen) im_log("start: no answer for the set within 10 s");
	if (!s_game_seen) {				/* too slow: no achievements this time */
		X->unload_game();
		s_console = 0;
		s_game_on = 0;
	}
	X->set_online(0);
	netsvc_quiet(1);
}

void pico_ra_frame(void)
{
	int i;
	if (!X || !s_game_on) return;
	if (!s_pref[PREF_ON]) { s_paused_by_pref = 1; return; }	/* turned off in the in-game menu */
	if (s_paused_by_pref) {		/* and on again: what was counted before no longer fits the memory */
		s_paused_by_pref = 0;
		X->progress_load(0, 0);
	}
	X->do_frame();
	/* nothing waits on the network in play: a call rcheevos makes now gets "not reached" */
	for (i = 0; i < RA_REQ_MAX; i++)
		if (S_REQ[i].used) {
			unsigned n = s_fake ? fake_answer(S_REQ[i].api, (char *)S_FBUF, 4096) : 0;
			finish(&S_REQ[i], n ? 200 : 0, n ? S_FBUF : 0, n);
		}
	events(1);
}

/* after pico_ra_frame: 1 once when the game must start again from its power on (hardcore came
 * on); the runner resets its core and then calls pico_ra_game_reset */
int pico_ra_reset_wanted(void)
{
	if (!s_reset_wanted) return 0;
	s_reset_wanted = 0;
	return 1;
}

/* the game was reset (the runner's own Reset too): rcheevos starts its counts again */
void pico_ra_game_reset(void) { if (X && s_game_on) X->reset(); }

/* hardcore holds now: the gates (no state loads, no rewind, ...) */
int pico_ra_hardcore(void) { return hc_now(); }

/* The in-game menu pauses the game: in hardcore rcheevos limits how often (rc_client_can_pause);
 * 0 not now, with a card saying how long to play on first. Always 1 outside hardcore. */
int pico_ra_may_pause(void)
{
	unsigned frames = 0;
	struct pico_ra_card *c;
	if (!hc_now() || X->can_pause(&frames)) return 1;
	c = card_new(PICO_RA_CARD_NOTE);
	copy_n(c->head, sizeof c->head, "Hardcore");
	copy_n(c->title, sizeof c->title, "The menu can open again soon");
	snprintf(c->line, sizeof c->line, "Pausing is limited in hardcore: %u s to go", (frames + 59u) / 60u);
	return 0;
}

void pico_ra_paused(void)
{
	static unsigned last;
	int i;
	if (!X || !s_game_on) return;
	if (now_ms() - last < 1000u) return;
	last = now_ms();
	X->idle();
	for (i = 0; i < RA_REQ_MAX; i++)
		if (S_REQ[i].used) finish(&S_REQ[i], 0, 0, 0);
	events(1);
}

void pico_ra_game_end(void)
{
	if (X && s_console) X->unload_game();
	s_console = 0;
	s_game_on = 0;
	s_in_game = 0;
	if (X) ua_set(0);			/* the menu's calls are no game's */
	netsvc_quiet(0);			/* the menu: the network as it was */
	s_card_head = s_card_tail;
}

/* a save state was loaded: what rcheevos counted (hits, deltas, progress) no longer fits the memory,
 * so it starts again from the state (no unlock can come from the jump) */
void pico_ra_state_loaded(void) { if (X && s_game_on) X->progress_load(0, 0); }

/* ---- a save state's progress, in a file beside it ----
 * rcheevos counts hits and deltas over many frames; a state loaded without them would start every
 * count again. Beside <game>.st1 lies <game>.rp1 (the slot's last character): "GPRS", version 1,
 * the RetroAchievements game id, the length, then rcheevos' progress. The state's own file is
 * untouched (other tools read it). A file of another game, or none, starts the counts again. */
#define RP_HDR 16

static void rp_ext(const char *state_ext, char out[4])
{
	unsigned n = (unsigned)strlen(state_ext);
	out[0] = 'r'; out[1] = 'p'; out[2] = n ? state_ext[n - 1] : '0'; out[3] = 0;
}

void pico_ra_state_saved(fat_vol *v, const char *dir, const char *rom, const char *state_ext)
{
	struct ra_summary sm;
	char ext[4], nm[288];
	unsigned n = 0;
	/* only for an account that plays with achievements (no files for everyone else); a state
	 * saved with the set not loaded gets an empty one, so an older file is never taken for it */
	if (!X || !v || s_state != PICO_RA_SIGNED_IN || !s_ra_system) return;
	memset(S_FBUF, 0, RP_HDR);
	memcpy(S_FBUF, "GPRS", 4);
	S_FBUF[4] = 1;
	if (s_game_on) {
		X->summary(&sm);
		n = X->progress_size();
		if (n > RA_FBUF_CAP - RP_HDR || X->progress_save(S_FBUF + RP_HDR, n) != 0) n = 0;
		memcpy(S_FBUF + 8, &sm.game_id, 4);
	}
	memcpy(S_FBUF + 12, &n, 4);
	rp_ext(state_ext, ext);
	gba_sd_state_name(rom, ext, nm, sizeof nm);
	card_writer_put(v, dir, nm, S_FBUF, RP_HDR + n, 0, 0, 0, 0);
	{ char l[80]; snprintf(l, sizeof l, "state %s: progress kept, %u bytes", ext, n); im_log(l); }
}

void pico_ra_state_loaded_from(fat_vol *v, const char *dir, const char *rom, const char *state_ext)
{
	struct ra_summary sm;
	char ext[4];
	unsigned n, len = 0, id = 0;
	if (!X || !s_game_on) return;
	rp_ext(state_ext, ext);
	n = v ? gba_sd_read_named(v, dir, rom, ext, S_FBUF, RA_FBUF_CAP) : 0;
	X->summary(&sm);
	if (n >= RP_HDR && !memcmp(S_FBUF, "GPRS", 4) && S_FBUF[4] == 1) {
		memcpy(&id, S_FBUF + 8, 4);
		memcpy(&len, S_FBUF + 12, 4);
	}
	if (len && id == sm.game_id && RP_HDR + len <= n) X->progress_load(S_FBUF + RP_HDR, len);
	else X->progress_load(0, 0);			/* none, or not this game's: counts start again */
	{
		char l[80];
		snprintf(l, sizeof l, "state %s: %s, %u bytes", ext, len && id == sm.game_id && RP_HDR + len <= n ?
			 "progress back" : "counts start again", len);
		im_log(l);
	}
}

unsigned pico_ra_progress_size(void) { return X && s_game_on ? X->progress_size() : 0; }
int pico_ra_progress_save(void *buf, unsigned cap) { return X && s_game_on ? X->progress_save(buf, cap) : -1; }
void pico_ra_progress_load(const void *buf, unsigned len) { if (X && s_game_on) X->progress_load(buf, len); }

/* ---- the in-game menu's list ---- */

int pico_ra_game_loaded(void) { return X && s_game_on; }

/* The in-game menu's Achievements page, in our memory: the account and the player's settings first,
 * then, with the game's set loaded, how far it has got and each achievement (unlocked ones after
 * the rest, as rcheevos groups them). */
static int s_list_n;

static struct ra_row *row_add(const char *title, const char *value, const char *text)
{
	struct ra_row *r = &S_LIST[s_list_n++];
	copy_n(r->title, sizeof r->title, title);
	copy_n(r->value, sizeof r->value, value ? value : "");
	copy_n(r->text, sizeof r->text, text ? text : "");
	r->unlocked = 0;
	r->sw = -1;
	r->pref = -1;
	return r;
}

static void row_switch(const char *title, int pref, const char *text)
{
	struct ra_row *r = row_add(title, s_pref[pref] ? "On" : "Off", text);
	r->sw = s_pref[pref];
	r->pref = pref;
}

/* what the page says about this game now */
static const char *status_text(void)
{
	if (pico_ra_state() == PICO_RA_NO_BLOB) return "Not on the card";
	if (s_state != PICO_RA_SIGNED_IN) return "Sign in first";
	if (s_game_on) return !s_pref[PREF_ON] ? "Off for now" : hc_now() ? "Hardcore" : "Softcore";
	if (!s_pref[PREF_ON]) return "Off";
	return "No set for this game";
}

int pico_ra_list(void)
{
	int n, i;
	s_list_n = 0;
	if (!s_ra_system) return 0;
	row_add("Account", s_state == PICO_RA_SIGNED_IN ? s_user : "Not signed in",
		s_state == PICO_RA_SIGNED_IN ? "Unlocks count as softcore" : "Sign in on the menu's Network tab");
	row_switch("Achievements", PREF_ON, s_game_on || s_pref[PREF_ON] ? "Counting for this game" :
		   "From the next game started: this one started with them off");
	row_switch("Unlock cards", PREF_UNLOCK_CARDS, "A card over the game when one unlocks");
	row_switch("Progress cards", PREF_PROGRESS_CARDS, "A card when an achievement with a count moves on");
	row_switch("Game start card", PREF_START_CARD, "The game's name and how far you are, at its start");
	{
		struct ra_row *r = row_add("Hardcore", s_pref[PREF_HARDCORE] == HC_ON ? "On" :
					   s_pref[PREF_HARDCORE] == HC_OFF ? "Off" : "Automatic",
					   s_pref[PREF_HARDCORE] == HC_AUTO ? "Hardcore once RetroAchievements validates this client; "
					   "turning it on restarts the game" : "Turning it on restarts the game; no state loads or "
					   "rewind in hardcore");
		r->pref = PREF_HARDCORE;
	}
	row_add("RetroAchievements", s_verdict == 1 ? "Validated" : s_verdict == 0 ? "Not validated yet" : "Not known yet",
		s_verdict == 0 ? "Hardcore unlocks count as softcore until RetroAchievements validates GammaOS Pico" :
		s_verdict == 1 ? "Hardcore unlocks count" : "Known after a game with achievements is played online");
	if (s_nq) {
		char v[16];
		snprintf(v, sizeof v, "%d", s_nq);
		row_add("Unlocks waiting", v, "Sent from the menu once there is a network");
	}
	row_add("Status", status_text(), s_game_on ? (!hc_now() && hc_wanted() ? s_hc_why : "") : s_note);
	if (!X || !s_game_on) return s_list_n;
	{	/* how far the set has got: achievements, then points */
		struct ra_summary sm;
		char v[32];
		X->summary(&sm);
		snprintf(v, sizeof v, "%u of %u", sm.unlocked, sm.total);
		row_add("Unlocked", v, "");
		snprintf(v, sizeof v, "%u of %u", sm.points_unlocked, sm.points_total);
		row_add("Points", v, "");
	}
	n = X->achievement_count();
	for (i = 0; i < n && s_list_n < RA_LIST_MAX; i++) {
		struct ra_achievement a;
		struct ra_row *r;
		if (X->achievement(i, &a) != 0) continue;
		r = row_add(a.title, "", a.text);
		r->unlocked = a.state == RA_ACH_UNLOCKED;
		if (r->unlocked) copy_n(r->value, sizeof r->value, "Unlocked");
		else if (a.state == RA_ACH_UNSUPPORTED) copy_n(r->value, sizeof r->value, "Unsupported");
		else if (a.measured[0]) snprintf(r->value, sizeof r->value, "%s", a.measured);
		else snprintf(r->value, sizeof r->value, "%u points", a.points);
	}
	return s_list_n;
}

/* row i: -1 a row only looked at, 0/1 a switch and its state */
int pico_ra_row_switch(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].sw : -1; }

/* Left, Right or A on row i: a switch flips (kept on the card); 1 when something changed */
int pico_ra_row_change(int i)
{
	struct ra_row *r;
	if (i < 0 || i >= s_list_n) return 0;
	r = &S_LIST[i];
	if (r->pref < 0 || r->pref >= PREFS) return 0;
	if (r->pref == PREF_HARDCORE) {	/* Off, On, Automatic; on takes a reset, off is at once */
		s_pref[PREF_HARDCORE] = (unsigned char)((s_pref[PREF_HARDCORE] + 1) % 3);
		copy_n(r->value, sizeof r->value, s_pref[PREF_HARDCORE] == HC_ON ? "On" :
		       s_pref[PREF_HARDCORE] == HC_OFF ? "Off" : "Automatic");
		if (X && s_game_on) {
			int want = hc_wanted() && s_session_online, cur = hc_now();
			if (want && !cur) { s_hc_why[0] = 0; X->set_hardcore(1); }	/* RA_EV_RESET follows */
			else if (!want && cur) X->set_hardcore(0);
			else if (want && !s_session_online) copy_n(s_hc_why, sizeof s_hc_why, "No network when the game started");
		}
		prefs_store();
		return 1;
	}
	s_pref[r->pref] ^= 1;
	r->sw = s_pref[r->pref];
	copy_n(r->value, sizeof r->value, r->sw ? "On" : "Off");
	if (r->pref == PREF_ON && !r->sw) s_card_head = s_card_tail;	/* off: the cards up go too */
	prefs_store();
	return 1;
}

/* the in-game menu has the Achievements page: the game is of a system RetroAchievements knows */
int pico_ra_igm_page(void) { return s_in_game && s_ra_system; }

const char *pico_ra_row_title(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].title : ""; }
const char *pico_ra_row_value(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].value : ""; }
const char *pico_ra_row_text(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].text : ""; }

/* the in-game menu's Achievements row: "12/40", or why there is no count */
const char *pico_ra_short(void)
{
	static char b[24];
	struct ra_summary s;
	if (pico_ra_state() == PICO_RA_NO_BLOB) return "";
	if (s_state != PICO_RA_SIGNED_IN) return "Sign in";
	if (!s_pref[PREF_ON]) return "Off";
	if (!X || !s_game_on) return "No set";
	X->summary(&s);
	snprintf(b, sizeof b, "%u/%u", s.unlocked, s.total);
	return b;
}

int pico_ra_item(int i, unsigned *id, char *title, unsigned tcap, char *line, unsigned lcap, int *unlocked, unsigned *points)
{
	struct ra_achievement a;
	if (!X || X->achievement(i, &a) != 0) return -1;
	if (id) *id = a.id;
	copy_n(title, tcap, a.title);
	if (a.measured[0] && a.state != RA_ACH_UNLOCKED) snprintf(line, lcap, "%s (%s)", a.text, a.measured);
	else copy_n(line, lcap, a.text);
	if (unlocked) *unlocked = a.state == RA_ACH_UNLOCKED;
	if (points) *points = a.points;
	return 0;
}

void pico_ra_game_summary(char *line, unsigned cap)
{
	struct ra_summary s;
	if (!X) { copy_n(line, cap, ""); return; }
	X->summary(&s);
	if (!s.game_loaded) copy_n(line, cap, "No achievements for this game");
	else snprintf(line, cap, "%u of %u unlocked, %u of %u points", s.unlocked, s.total, s.points_unlocked, s.points_total);
}
