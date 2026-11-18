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
#include "core_file.h"
#include "card_writer.h"
#include "gba_sd_save.h"
#include "../../platform/common/pico/pico_netsvc.h"

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
#define RA_LIST_MAX	96
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
struct ra_row { char title[96], value[32], text[160]; int unlocked; };
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
static unsigned s_http_id;			/* the call on the network now (0 none) */
static int s_http_award;			/* that call is a kept unlock's (S_QUEUE[0]) */
static unsigned s_award_wait_until;		/* ms: no unlock sent before (after a failure) */

static int s_console;				/* rcheevos' console of the game picked, 0 none */
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

/* the queue as a file: "GPRQ", version, count, then the records */
static unsigned queue_image(unsigned char *out, unsigned cap)
{
	unsigned len = 8 + (unsigned)s_nq * sizeof(struct ra_queued);
	if (len > cap) return 0;
	memcpy(out, "GPRQ", 4);
	out[4] = 1;
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
	if (n < 8 || memcmp(S_FBUF, "GPRQ", 4) || S_FBUF[4] != 1) return;
	cnt = S_FBUF[5] | (unsigned)S_FBUF[6] << 8;
	if (cnt > RA_QUEUE_MAX || 8 + cnt * sizeof(struct ra_queued) > n) return;
	memcpy(S_QUEUE, S_FBUF + 8, cnt * sizeof(struct ra_queued));
	s_nq = (int)cnt;
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

static int ra_load(void)
{
	ra_core_entry_fn entry;
	const unsigned *hdr = (const unsigned *)RA_MEM_PA;
	unsigned span, heap;
	int err;
	char ua[96], clause[48];
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
	heap = RA_IO_PA - (RA_MEM_PA + span);
	X->heap_init((void *)(RA_MEM_PA + span), heap);
	if (X->init() != 0) { X = 0; s_load_failed = 1; return -1; }
	clause[0] = 0;
	X->user_agent_clause(clause, sizeof clause);
	snprintf(ua, sizeof ua, "%s %s", RA_UA, clause);
	netsvc_user_agent(ua);
	return 0;
}

/* ---- the cards over the game ---- */

#define CARDS 4
#define CARD_MS 4000u
static struct pico_ra_card s_cards[CARDS];
static unsigned s_card_head, s_card_tail, s_card_t0;

static struct pico_ra_card *card_new(int kind)
{
	struct pico_ra_card *c;
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
			if (e.ok) {
				c = card_new(PICO_RA_CARD_GAME);
				copy_n(c->head, sizeof c->head, "RetroAchievements");
				copy_n(c->title, sizeof c->title, e.title);
				snprintf(c->line, sizeof c->line, "%d of %u achievements unlocked", e.value, e.id);
			}
			break;
		case RA_EV_UNLOCK:
			c = card_new(PICO_RA_CARD_UNLOCK);
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
			 "\"Points\":1,\"MemAddr\":\"0xH0000=0xH0000.200.\",\"Author\":\"GammaOS\",\"BadgeName\":\"00000\","
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
		if (s_http_award) {
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
		return;
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
	}
}

/* ---- the menu ---- */

void pico_ra_card(fat_vol *v)
{
	if (!v || v == s_vol) return;
	s_vol = v;
	account_load();
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
}

/* ---- a game ---- */

void pico_ra_game_begin(int type)
{
	static const unsigned char consoles[9] = { 4, 6, 5, 3, 1, 11, 15, 33, 7 };	/* GBA_CONSOLE_* order */
	s_console = type >= 0 && type < 9 ? consoles[type] : 0;
	s_game_on = 0;
	s_in_game = 1;
	netsvc_quiet(1);			/* (pico_ra_core_ready wakes it for the game's set, then it sleeps) */
}

void pico_ra_core_ready(const void *rom, unsigned rom_size, const struct pico_core_memory *mem)
{
	unsigned *ids = (unsigned *)S_FBUF, t0;	/* (the blob copies them) */
	int i;
	if (!X || s_state != PICO_RA_SIGNED_IN || !s_console || !rom || !rom_size) { s_console = 0; return; }
	netsvc_quiet(0);
	clock_snapshot();
	for (i = 0; i < s_nq; i++) ids[i] = S_QUEUE[i].id;
	X->set_local_unlocks(ids, s_nq);
	X->set_online(1);
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
	X->do_frame();
	/* nothing waits on the network in play: a call rcheevos makes now gets "not reached" */
	for (i = 0; i < RA_REQ_MAX; i++)
		if (S_REQ[i].used) {
			unsigned n = s_fake ? fake_answer(S_REQ[i].api, (char *)S_FBUF, 4096) : 0;
			finish(&S_REQ[i], n ? 200 : 0, n ? S_FBUF : 0, n);
		}
	events(1);
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
	netsvc_quiet(0);			/* the menu: the network as it was */
	s_card_head = s_card_tail;
}

/* a save state was loaded: what rcheevos counted (hits, deltas, progress) no longer fits the memory,
 * so it starts again from the state (no unlock can come from the jump) */
void pico_ra_state_loaded(void) { if (X && s_game_on) X->progress_load(0, 0); }

unsigned pico_ra_progress_size(void) { return X && s_game_on ? X->progress_size() : 0; }
int pico_ra_progress_save(void *buf, unsigned cap) { return X && s_game_on ? X->progress_save(buf, cap) : -1; }
void pico_ra_progress_load(const void *buf, unsigned len) { if (X && s_game_on) X->progress_load(buf, len); }

/* ---- the in-game menu's list ---- */

int pico_ra_game_loaded(void) { return X && s_game_on; }

/* the list for the in-game menu, in our memory: unlocked ones after the rest, as rcheevos groups them */
static int s_list_n;
int pico_ra_list(void)
{
	int n, i;
	s_list_n = 0;
	if (!X || !s_game_on) return 0;
	n = X->achievement_count();
	for (i = 0; i < n && s_list_n < RA_LIST_MAX; i++) {
		struct ra_achievement a;
		struct ra_row *r = &S_LIST[s_list_n];
		if (X->achievement(i, &a) != 0) continue;
		copy_n(r->title, sizeof r->title, a.title);
		copy_n(r->text, sizeof r->text, a.text);
		r->unlocked = a.state == RA_ACH_UNLOCKED;
		if (r->unlocked) copy_n(r->value, sizeof r->value, "Unlocked");
		else if (a.state == RA_ACH_UNSUPPORTED) copy_n(r->value, sizeof r->value, "Unsupported");
		else if (a.measured[0]) snprintf(r->value, sizeof r->value, "%s", a.measured);
		else snprintf(r->value, sizeof r->value, "%u points", a.points);
		s_list_n++;
	}
	return s_list_n;
}

const char *pico_ra_row_title(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].title : ""; }
const char *pico_ra_row_value(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].value : ""; }
const char *pico_ra_row_text(int i) { return i >= 0 && i < s_list_n ? S_LIST[i].text : ""; }

/* the in-game menu's Achievements row: "12/40" */
const char *pico_ra_short(void)
{
	static char b[24];
	struct ra_summary s;
	if (!X || !s_game_on) return "";
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
