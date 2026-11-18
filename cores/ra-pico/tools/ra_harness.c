/*
 * ra-pico PC harness: ra_core.c built for the PC with rcheevos, driven the way the LK will drive the
 * blob, against a small server of our own in this file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * The "LK" side here: requests are answered after the call that made them returns (as the LK
 * will, from its network or its copies), every successful answer is kept per call and key (the
 * LK's copies on the card), and while "offline" the kept copy answers. Unlocks the blob hands
 * over (RA_EV_QUEUED) go into a queue and are sent later with award_request().
 *
 * The game: an NES-sized RAM of 2 KB the test writes into. The set (four achievements):
 *   9200001  5 points  0xH0004=1            the pulse
 *   9200002 10 points  0xH0004=1.3.         the pulse three times
 *   9200003  5 points  0xH0010=7            unlocked offline before this run (local unlocks)
 *   9200004 10 points  M:0xH0020>=50        measured, for progress
 *
 * Exit status 0 when every check passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ra_core_abi.h"

#ifdef RA_HARNESS_BLOB
/* the blob itself (out/ra-pico.blob), under qemu-arm: put at its slot the way the LK will */
#include <sys/mman.h>
static unsigned char *g_slot;
static unsigned g_span;
static const struct ra_core_exports *ra_core_blob_init(const struct ra_core_imports *imp)
{
	const unsigned *h = (const unsigned *)g_slot;
	return ((ra_core_entry_fn)(void *)(g_slot + h[2]))(imp);
}
static int load_blob(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	const unsigned *h;
	if (!f) { perror(path); return -1; }
	g_slot = mmap((void *)RA_SLOT_PA, RA_SLOT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (g_slot == MAP_FAILED) { perror("mmap"); fclose(f); return -1; }
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > (long)RA_SLOT_SIZE || fread(g_slot, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
	fclose(f);
	h = (const unsigned *)g_slot;
	if (h[0] != RA_CORE_ABI_MAGIC || h[1] != RA_CORE_ABI_VERSION || h[3] != (unsigned)n || h[4] > RA_SLOT_SIZE) {
		fprintf(stderr, "bad blob header\n");
		return -1;
	}
	g_span = (h[4] + 15u) & ~15u;	/* the BSS is already zero (a fresh mapping) */
	__builtin___clear_cache((char *)g_slot, (char *)g_slot + n);
	printf("blob: %ld bytes, span %u\n", n, h[4]);
	return 0;
}
#else
const struct ra_core_exports *ra_core_blob_init(const struct ra_core_imports *imp);
#endif

static const struct ra_core_exports *X;
static int fails;

#define CHECK(c, what) do { if (c) printf("  ok   %s\n", what); else { printf("  FAIL %s\n", what); fails++; } } while (0)

/* ---- the server ------------------------------------------------------------------------------------ */

static const struct { unsigned id, points; const char *title, *mem; } k_set[] = {
	{ 9200001, 5, "Pulse", "0xH0004=1" },
	{ 9200002, 10, "Three pulses", "0xH0004=1.3." },
	{ 9200003, 5, "Seven", "0xH0010=7" },
	{ 9200004, 10, "Count to fifty", "M:0xH0020>=50" },
};
#define K_SET_N 4
static unsigned srv_unlocked[16];
static int srv_n;
static unsigned srv_score = 40;
static unsigned srv_last_o;	/* the seconds since unlock of the last award */

static int srv_has(unsigned id)
{
	int i;
	for (i = 0; i < srv_n; i++) if (srv_unlocked[i] == id) return 1;
	return 0;
}

static int form(const char *f, const char *name, char *out, unsigned cap)
{
	size_t n = strlen(name);
	const char *p = f;
	while (p && *p) {
		if (!strncmp(p, name, n) && p[n] == '=') {
			unsigned i = 0;
			p += n + 1;
			while (*p && *p != '&' && i + 1 < cap) out[i++] = *p++;
			out[i] = 0;
			return 1;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
	out[0] = 0;
	return 0;
}

static int server(const char *post, char *body, unsigned cap)
{
	char r[32], v[64];
	unsigned at = 0;
	int i;
	form(post, "r", r, sizeof r);
	if (!strcmp(r, "login2")) {
		form(post, "u", v, sizeof v);
		if (!form(post, "p", r, sizeof r) && !form(post, "t", r, sizeof r)) return 401;
		snprintf(body, cap, "{\"Success\":true,\"User\":\"%s\",\"DisplayName\":\"%s\",\"Token\":\"tok0123456789\","
			 "\"Score\":0,\"SoftcoreScore\":%u,\"Messages\":0}", v, v, srv_score);
		return 200;
	}
	if (!strcmp(r, "achievementsets")) {
		at += (unsigned)snprintf(body + at, cap - at, "{\"Success\":true,\"GameId\":77001,\"Title\":\"Harness Quest\","
					 "\"ConsoleId\":7,\"ImageIconUrl\":\"\",\"RichPresenceGameId\":77001,"
					 "\"RichPresencePatch\":\"Display:\\nTesting\\n\",\"Sets\":[{\"AchievementSetId\":77001,"
					 "\"GameId\":77001,\"Title\":\"Harness Quest\",\"Type\":\"core\",\"ImageIconUrl\":\"\","
					 "\"Achievements\":[");
		for (i = 0; i < K_SET_N; i++)
			at += (unsigned)snprintf(body + at, cap - at,
						 "%s{\"ID\":%u,\"Title\":\"%s\",\"Description\":\"d\",\"Flags\":3,\"Points\":%u,"
						 "\"MemAddr\":\"%s\",\"Author\":\"GammaOS\",\"BadgeName\":\"%05u\",\"Created\":1,"
						 "\"Modified\":1,\"Type\":\"\",\"Rarity\":1,\"RarityHardcore\":1,\"BadgeURL\":\"\","
						 "\"BadgeLockedURL\":\"\"}", i ? "," : "", k_set[i].id, k_set[i].title,
						 k_set[i].points, k_set[i].mem, k_set[i].id % 100000);
		snprintf(body + at, cap - at, "],\"Leaderboards\":[]}]}");
		return 200;
	}
	if (!strcmp(r, "startsession")) {
		at += (unsigned)snprintf(body + at, cap - at, "{\"Success\":true,\"HardcoreUnlocks\":[],\"Unlocks\":[");
		for (i = 0; i < srv_n; i++)
			at += (unsigned)snprintf(body + at, cap - at, "%s{\"ID\":%u,\"When\":1700000000}", i ? "," : "", srv_unlocked[i]);
		snprintf(body + at, cap - at, "],\"ServerNow\":1700000000}");
		return 200;
	}
	if (!strcmp(r, "ping")) { snprintf(body, cap, "{\"Success\":true}"); return 200; }
	if (!strcmp(r, "awardachievement")) {
		unsigned id, pts = 0, left = 0;
		int had;
		form(post, "a", v, sizeof v);
		id = (unsigned)strtoul(v, 0, 10);
		form(post, "o", v, sizeof v);
		srv_last_o = (unsigned)strtoul(v, 0, 10);
		for (i = 0; i < K_SET_N; i++) if (k_set[i].id == id) pts = k_set[i].points;
		if (!pts) { snprintf(body, cap, "{\"Success\":false,\"Error\":\"Unknown achievement\"}"); return 404; }
		had = srv_has(id);
		if (!had) { srv_unlocked[srv_n++] = id; srv_score += pts; }
		for (i = 0; i < K_SET_N; i++) left += !srv_has(k_set[i].id);
		snprintf(body, cap, "{\"Success\":%s,\"Score\":0,\"SoftcoreScore\":%u,\"AchievementID\":%u,"
			 "\"AchievementsRemaining\":%u}",
			 had ? "false,\"Error\":\"User already has this achievement unlocked.\"" : "true", srv_score, id, left);
		return 200;
	}
	return 404;
}

/* ---- the LK's side ---------------------------------------------------------------------------------- */

static int online = 1;
static struct pend { unsigned id; char api[24], key[48], post[512]; } pend[16];
static int npend;
static struct keep { char api[24], key[48], body[8192]; } keep[8];
static int nkeep;
static struct ra_queued queue[16];
static int nqueue;
static int requests_seen[8];	/* login, sets, session, award, ping */

static void im_request(unsigned id, const char *url, const char *post, const char *ctype, const char *api, const char *key)
{
	(void)url; (void)ctype;
	if (!strcmp(api, "login2")) requests_seen[0]++;
	else if (!strcmp(api, "achievementsets")) requests_seen[1]++;
	else if (!strcmp(api, "startsession")) requests_seen[2]++;
	else if (!strcmp(api, "awardachievement")) requests_seen[3]++;
	else if (!strcmp(api, "ping")) requests_seen[4]++;
	if (npend == 16) return;
	pend[npend].id = id;
	snprintf(pend[npend].api, sizeof pend[npend].api, "%s", api);
	snprintf(pend[npend].key, sizeof pend[npend].key, "%s", key);
	snprintf(pend[npend].post, sizeof pend[npend].post, "%s", post ? post : "");
	npend++;
}

static struct keep *kept(const char *api, const char *key)
{
	int i;
	for (i = 0; i < nkeep; i++) if (!strcmp(keep[i].api, api) && !strcmp(keep[i].key, key)) return &keep[i];
	return 0;
}

/* every waiting request answered: from the server, or offline from the copies */
static void pump(void)
{
	static char body[8192];
	while (npend) {
		struct pend p = pend[0];
		int status;
		memmove(pend, pend + 1, (size_t)(npend - 1) * sizeof *pend);
		npend--;
		if (online) {
			status = server(p.post, body, sizeof body);
			if (status == 200 && strstr(body, "\"Success\":true")) {
				struct keep *k = kept(p.api, p.key);
				if (!k && nkeep < 8) k = &keep[nkeep++];
				if (k) {
					snprintf(k->api, sizeof k->api, "%s", p.api);
					snprintf(k->key, sizeof k->key, "%s", p.key);
					snprintf(k->body, sizeof k->body, "%s", body);
				}
			}
			X->answer(p.id, status, body, (unsigned)strlen(body));
		} else {
			struct keep *k = kept(p.api, p.key);
			if (k) X->answer(p.id, 200, k->body, (unsigned)strlen(k->body));
			else X->answer(p.id, 0, 0, 0);
		}
	}
}

static unsigned long long fake_ms;
static long long fake_utc = 1763400000;
static unsigned long long im_now_ms(void) { return fake_ms; }
static long long im_utc(void) { return fake_utc; }
static void im_log(const char *line) { printf("    [rc] %s\n", line); }

static const struct ra_core_imports k_im = { im_request, im_now_ms, im_utc, im_log };

/* the events since the last call: counted by kind, queued unlocks kept */
static int events[16];
static struct ra_event last[16];
static void drain(void)
{
	struct ra_event e;
	while (X->next_event(&e)) {
		if (e.kind >= 0 && e.kind < 16) { events[e.kind]++; last[e.kind] = e; }
		if (e.kind == RA_EV_QUEUED && nqueue < 16) {
			queue[nqueue].id = e.id;
			snprintf(queue[nqueue].hash, sizeof queue[nqueue].hash, "%s", e.hash);
			queue[nqueue].when = e.when;
			nqueue++;
		}
		printf("    event %d ok %d id %u value %d \"%s\" \"%s\"\n", e.kind, e.ok, e.id, e.value, e.title, e.text);
	}
}

static unsigned char ram[2048];
static void frames(int n)
{
	int i;
	for (i = 0; i < n; i++) { X->do_frame(); fake_ms += 16; pump(); }
	drain();
}

int main(int argc, char **argv)
{
	static unsigned char rom[16 + 32768];
	static unsigned char heap[1];
	struct pico_core_memory mem;
	struct ra_summary s;
	char url[512], post[512], ua[64];
	unsigned local[1] = { 9200003 };
	int i, n, pings_before;

#ifdef RA_HARNESS_BLOB
	if (argc < 2 || load_blob(argv[1]) != 0) { fprintf(stderr, "usage: %s out/ra-pico.blob\n", argv[0]); return 2; }
	X = ra_core_blob_init(&k_im);
	CHECK(X && X->magic == RA_CORE_ABI_MAGIC && X->version == RA_CORE_ABI_VERSION, "the export table");
	X->heap_init(g_slot + g_span, RA_SLOT_SIZE - g_span);
	(void)heap;
#else
	(void)argc; (void)argv;
	X = ra_core_blob_init(&k_im);
	CHECK(X && X->magic == RA_CORE_ABI_MAGIC && X->version == RA_CORE_ABI_VERSION, "the export table");
	X->heap_init(heap, sizeof heap);
#endif
	CHECK(X->init() == 0, "rc_client made");
	X->user_agent_clause(ua, sizeof ua);
	printf("  user agent clause: %s\n", ua);
	CHECK(!strncmp(ua, "rcheevos/", 9), "user agent clause");

	printf("login\n");
	X->set_online(1);
	X->login_password("tester", "secret");
	pump();
	drain();
	CHECK(events[RA_EV_LOGIN] == 1 && last[RA_EV_LOGIN].ok, "signed in with a password");
	CHECK(X->token(url, sizeof url) == 0 && !strcmp(url, "tok0123456789"), "the token to keep");

	printf("a game, one unlock from an earlier offline run still queued\n");
	memcpy(rom, "NES\x1a\x02\x01", 6);
	for (i = 16; i < (int)sizeof rom; i++) rom[i] = (unsigned char)(i * 7);
	memset(&mem, 0, sizeof mem);
	mem.system_ram = ram;
	mem.system_ram_size = sizeof ram;
	X->set_local_unlocks(local, 1);
	X->load_game(7 /* RC_CONSOLE_NINTENDO */, rom, sizeof rom, &mem);
	pump();
	drain();
	CHECK(events[RA_EV_GAME] == 1 && last[RA_EV_GAME].ok, "the set loaded");
	X->summary(&s);
	printf("  %s, %u of %u, %u of %u points, hash %s\n", s.game, s.unlocked, s.total, s.points_unlocked, s.points_total, s.hash);
	CHECK(s.total == 4 && s.unlocked == 1, "the queued unlock shows as unlocked");

	printf("play: the pulse\n");
	frames(3);
	ram[4] = 1;
	frames(1);
	ram[4] = 0;
	frames(1);
	CHECK(events[RA_EV_UNLOCK] == 1 && last[RA_EV_UNLOCK].id == 9200001, "unlock event for the pulse");
	CHECK(events[RA_EV_QUEUED] == 1 && queue[0].id == 9200001 && strlen(queue[0].hash) == 32, "the unlock queued with the game's hash");
	CHECK(requests_seen[3] == 0, "no award reached the network during play");
	ram[4] = 1; frames(1); ram[4] = 0; frames(1);
	ram[4] = 1; frames(1); ram[4] = 0; frames(1);
	CHECK(events[RA_EV_UNLOCK] == 2 && last[RA_EV_UNLOCK].id == 9200002, "the hit count unlock");

	printf("progress and save states\n");
	ram[0x20] = 10; frames(2);
	ram[0x20] = 30; frames(2);
	CHECK(events[RA_EV_PROGRESS] >= 1, "a progress indicator");
	n = (int)X->progress_size();
	{
		unsigned char *st = malloc((size_t)n);
		CHECK(n > 0 && X->progress_save(st, (unsigned)n) == 0, "progress saved");
		CHECK(X->progress_load(st, (unsigned)n) == 0, "progress loaded");
		free(st);
	}
	ram[0x20] = 50; frames(2);
	CHECK(events[RA_EV_MASTERED] == 1, "mastery shown offline (the set answered locally)");
	X->summary(&s);
	CHECK(s.unlocked == 4, "all four unlocked");

	printf("the list\n");
	n = X->achievement_count();
	CHECK(n == 4, "four in the list");
	for (i = 0; i < n; i++) {
		struct ra_achievement a;
		X->achievement(i, &a);
		printf("    %u %-16s state %d %s\n", a.id, a.title, a.state, a.measured);
	}

	printf("the queue reaches the server later\n");
	fake_utc += 3600;	/* an hour on */
	for (i = 0; i < nqueue; i++) {
		static char body[1024];
		int status, r;
		CHECK(X->award_request(&queue[i], url, sizeof url, post, sizeof post) == 0, "award request made");
		CHECK(strstr(post, "r=awardachievement") && strstr(post, "&h=0"), "softcore award");
		status = server(post, body, sizeof body);
		r = X->award_answer(status, body, (unsigned)strlen(body));
		printf("    %u: o=%u -> %d\n", queue[i].id, srv_last_o, r);
		CHECK(r == 1 && srv_last_o >= 3600, "the server has it, an hour after");
	}
	{	/* again: already there is still a success */
		static char body[1024];
		int st, r;
		X->award_request(&queue[0], url, sizeof url, post, sizeof post);
		st = server(post, body, sizeof body);
		r = X->award_answer(st, body, (unsigned)strlen(body));
		printf("    again: %d %s -> %d\n", st, body, r);
		CHECK(r == 1, "a second send counts as done");
		CHECK(X->award_answer(0, 0, 0) == 0, "no answer: later");
	}

	printf("offline: the next start from the kept copies\n");
	X->unload_game();
	X->shutdown();
	memset(events, 0, sizeof events);
	CHECK(X->init() == 0, "rc_client made again");
	online = 0;
	X->set_online(0);
	X->login_token("tester", "tok0123456789");
	pump();
	drain();
	CHECK(events[RA_EV_LOGIN] == 1 && last[RA_EV_LOGIN].ok, "signed in from the kept answer");
	X->set_local_unlocks(local, 1);
	X->load_game(7, rom, sizeof rom, &mem);
	pump();
	drain();
	X->summary(&s);
	printf("  %s, %u of %u\n", s.game, s.unlocked, s.total);
	CHECK(events[RA_EV_GAME] == 1 && last[RA_EV_GAME].ok && s.unlocked == 1, "the set from the kept copy, the queued one unlocked");
	ram[4] = 0; frames(2); ram[4] = 1; frames(1); ram[4] = 0;
	CHECK(events[RA_EV_UNLOCK] == 1, "unlocks offline");
	pings_before = requests_seen[4];
	for (i = 0; i < 200; i++) { X->idle(); fake_ms += 1000; pump(); }
	drain();
	CHECK(requests_seen[4] == pings_before, "rich presence answered locally offline");
	X->unload_game();
	X->shutdown();

	printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
	return fails ? 1 : 0;
}
