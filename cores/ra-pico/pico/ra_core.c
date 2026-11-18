/*
 * ra-pico: rc_client (rcheevos) behind the export table of ra_core_abi.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * What this file does around rc_client:
 *   - server calls: each one gets a number and goes to the LK (request); its answer comes back
 *     through answer(), is kept, and reaches rc_client's callback from deliver(), never from inside
 *     the call that made the request (rc_client may be in the middle of something then).
 *   - offline: unlocks and rich presence never wait for the network. An unlock is answered here
 *     with the score it brings and what is left of the set (so mastery still shows), and goes to
 *     the LK as an RA_EV_QUEUED event to keep. award_request() later signs the real call with the
 *     seconds since the unlock. Unlocks still queued are written into a session answer's unlock
 *     list (set_local_unlocks), so a set read from the LK's copy shows them.
 *   - memory: rc_libretro's regions over the core's libretro map and blocks.
 *   - events: rc_client's, as ra_event records in a small ring the LK empties.
 *   - softcore only: hardcore is switched off before anything else and never on again.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_client.h"
#include "rc_api_runtime.h"
#include "rc_consoles.h"
#include "rc_version.h"
#include "rc_libretro.h"

#include "ra_core_abi.h"
#include "pico_libc.h"

static const struct ra_core_imports *g_im;
static rc_client_t *g_client;

/* ---- helpers ------------------------------------------------------------------------------------ */

static void copy_text(char *dst, unsigned cap, const char *src)
{
	unsigned i = 0;
	if (!cap) return;
	if (src)
		for (; src[i] && i + 1u < cap; i++) dst[i] = src[i];
	dst[i] = 0;
}

static void say(const char *fmt, const char *a, unsigned n)
{
	char line[192];
	if (!g_im || !g_im->log) return;
	snprintf(line, sizeof line, fmt, a ? a : "", n);
	g_im->log(line);
}

/* the newlib clock (time(), rcheevos' unlock times) asks this */
long pico_host_seconds(void)
{
	return g_im && g_im->utc ? (long)g_im->utc() : 0;
}

/* ---- events -------------------------------------------------------------------------------------- */

#define EV_RING 32
static struct ra_event ev_ring[EV_RING];
static unsigned ev_head, ev_tail;	/* head: next to read; tail: next to write */

static struct ra_event *ev_push(int kind)
{
	struct ra_event *e;
	if (ev_tail - ev_head >= EV_RING) ev_head++;	/* full: the oldest goes */
	e = &ev_ring[ev_tail++ % EV_RING];
	memset(e, 0, sizeof *e);
	e->kind = kind;
	return e;
}

static int ex_next_event(struct ra_event *out)
{
	if (ev_head == ev_tail) return 0;
	*out = ev_ring[ev_head++ % EV_RING];
	return 1;
}

static void ev_achievement(int kind, const rc_client_achievement_t *a)
{
	struct ra_event *e = ev_push(kind);
	if (!a) return;
	e->id = a->id;
	e->value = (int)a->points;
	copy_text(e->title, sizeof e->title, a->title);
	copy_text(e->text, sizeof e->text, a->description);
	copy_text(e->badge, sizeof e->badge, a->badge_name);
}

static void on_event(const rc_client_event_t *event, rc_client_t *client)
{
	struct ra_event *e;
	(void)client;
	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		ev_achievement(RA_EV_UNLOCK, event->achievement);
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED: {
		const rc_client_game_t *g = rc_client_get_game_info(g_client);
		rc_client_user_game_summary_t s;
		rc_client_get_user_game_summary(g_client, &s);
		e = ev_push(RA_EV_MASTERED);
		e->value = (int)s.points_core;
		copy_text(e->title, sizeof e->title, g ? g->title : "");
		copy_text(e->badge, sizeof e->badge, g ? g->badge_name : "");
		break;
	}
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		ev_achievement(RA_EV_PROGRESS, event->achievement);
		if (event->achievement) {
			e = &ev_ring[(ev_tail - 1u) % EV_RING];
			copy_text(e->text, sizeof e->text, event->achievement->measured_progress);
			e->value = (int)(event->achievement->measured_percent + .5f);
		}
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
		ev_push(RA_EV_PROGRESS_HIDE);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		ev_achievement(RA_EV_CHALLENGE, event->achievement);
		ev_ring[(ev_tail - 1u) % EV_RING].value = event->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW;
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		e = ev_push(RA_EV_SERVER_ERROR);
		if (event->server_error) copy_text(e->text, sizeof e->text, event->server_error->error_message);
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		ev_push(RA_EV_DISCONNECTED);
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		ev_push(RA_EV_RECONNECTED);
		break;
	default:	/* leaderboards (hardcore only), reset (to enter hardcore), subsets: not here */
		break;
	}
}

/* ---- server calls -------------------------------------------------------------------------------- */

enum { CALL_FREE, CALL_SENT, CALL_ANSWERED };
#define CALLS 16
static struct call {
	int state;
	unsigned id;
	rc_client_server_callback_t callback;
	void *data;
	char api[24];
	int status;
	char *body;		/* the answer, on the heap, 0 terminated */
	unsigned len;
} calls[CALLS];
static unsigned next_call_id = 1;
static int in_call;		/* inside server_call or a callback: answers wait for deliver() */
static int g_online;

/* the unlocks still on the card's queue, written into session answers */
static unsigned *local_ids;
static int local_n;

/* a form field's value ("r=login2&u=me"): 1 found */
static int form_value(const char *form, const char *name, char *out, unsigned cap)
{
	unsigned n = (unsigned)strlen(name);
	const char *p = form;
	while (p && *p) {
		if (!strncmp(p, name, n) && p[n] == '=') {
			unsigned i = 0;
			p += n + 1;
			while (*p && *p != '&' && i + 1u < cap) out[i++] = *p++;
			out[i] = 0;
			return 1;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
	if (cap) out[0] = 0;
	return 0;
}

/* a session answer with the queued unlocks added to its "Unlocks" list: a new heap copy, or 0 */
static char *session_with_local(const char *body, unsigned len, unsigned *out_len)
{
	static const char key[] = "\"Unlocks\":[";
	long long now = pico_host_seconds();
	unsigned add_cap = (unsigned)local_n * 40u + 32u, at = 0, i;
	char *add, *out;
	const char *k;
	if (!local_n || !body) return 0;
	add = malloc(add_cap);
	if (!add) return 0;
	for (i = 0; i < (unsigned)local_n; i++)
		at += (unsigned)snprintf(add + at, add_cap - at, "%s{\"ID\":%u,\"When\":%lld}", i ? "," : "", local_ids[i], now);
	out = malloc(len + at + 32u);
	if (!out) { free(add); return 0; }
	k = strstr(body, key);
	if (k) {
		unsigned head = (unsigned)(k - body) + (unsigned)strlen(key);
		int empty = body[head] == ']';
		memcpy(out, body, head);
		memcpy(out + head, add, at);
		if (!empty) out[head + at++] = ',';
		memcpy(out + head + at, body + head, len - head);
		*out_len = len + at;
	} else {
		/* no list: one before the closing brace */
		const char *end = body + len;
		unsigned head, n;
		while (end > body && end[-1] != '}') end--;
		if (end == body) { free(add); free(out); return 0; }
		head = (unsigned)(end - 1 - body);
		memcpy(out, body, head);
		n = (unsigned)snprintf(out + head, at + 32u, ",\"Unlocks\":[%s]", add);
		memcpy(out + head + n, body + head, len - head);
		*out_len = len + n;
	}
	out[*out_len] = 0;
	free(add);
	return out;
}

/* the calls that can be answered here: unlocks (always: kept by the LK instead), rich presence
 * (while offline). 1 answered. */
static int answer_here(const char *api, const char *post, struct call *c)
{
	char buf[160];
	if (!strcmp(api, "awardachievement")) {
		char a[16], hash[40];
		const rc_client_user_t *u = rc_client_get_user_info(g_client);
		const rc_client_achievement_t *ach;
		rc_client_user_game_summary_t s;
		struct ra_event *e;
		unsigned id, remaining;
		form_value(post, "a", a, sizeof a);
		form_value(post, "m", hash, sizeof hash);
		id = (unsigned)strtoul(a, 0, 10);
		ach = rc_client_get_achievement_info(g_client, id);
		rc_client_get_user_game_summary(g_client, &s);
		/* rc_client counts this one as unlocked already */
		remaining = s.num_core_achievements > s.num_unlocked_achievements ?
				    s.num_core_achievements - s.num_unlocked_achievements : 0;
		snprintf(buf, sizeof buf,
			 "{\"Success\":true,\"Score\":%u,\"SoftcoreScore\":%u,\"AchievementID\":%u,\"AchievementsRemaining\":%u}",
			 (unsigned)(u ? u->score : 0), (unsigned)((u ? u->score_softcore : 0) + (ach ? ach->points : 0)), id,
			 remaining);
		e = ev_push(RA_EV_QUEUED);
		e->id = id;
		e->when = pico_host_seconds();
		copy_text(e->hash, sizeof e->hash, hash);
	} else if (!g_online && !strcmp(api, "ping")) {
		snprintf(buf, sizeof buf, "{\"Success\":true}");
	} else {
		return 0;
	}
	c->status = 200;
	c->len = (unsigned)strlen(buf);
	c->body = malloc(c->len + 1u);
	if (!c->body) { c->status = 0; c->len = 0; }
	else memcpy(c->body, buf, c->len + 1u);
	c->state = CALL_ANSWERED;
	return 1;
}

static void deliver(void);

static void server_call(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callback_data,
			rc_client_t *client)
{
	struct call *c = 0;
	char key[48];
	int i;
	(void)client;
	for (i = 0; i < CALLS; i++)
		if (calls[i].state == CALL_FREE) { c = &calls[i]; break; }
	if (!c) {	/* more than CALLS at once: tell rc_client to try again */
		rc_api_server_response_t r;
		memset(&r, 0, sizeof r);
		r.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		r.body = "";
		callback(&r, callback_data);
		return;
	}
	memset(c, 0, sizeof *c);
	c->id = next_call_id++;
	if (!next_call_id) next_call_id = 1;
	c->callback = callback;
	c->data = callback_data;
	c->state = CALL_SENT;
	form_value(request->post_data ? request->post_data : "", "r", c->api, sizeof c->api);
	in_call++;
	if (!answer_here(c->api, request->post_data ? request->post_data : "", c)) {
		const char *post = request->post_data ? request->post_data : "";
		if (!form_value(post, "m", key, sizeof key) && !form_value(post, "g", key, sizeof key) &&
		    !form_value(post, "u", key, sizeof key))
			key[0] = 0;
		g_im->request(c->id, request->url, request->post_data, request->content_type, c->api, key);
	}
	in_call--;
	deliver();
}

static void ex_answer(unsigned id, int status, const char *body, unsigned len)
{
	struct call *c = 0;
	int i;
	for (i = 0; i < CALLS; i++)
		if (calls[i].state == CALL_SENT && calls[i].id == id) { c = &calls[i]; break; }
	if (!c) return;	/* not ours, or answered already */
	c->body = 0;
	c->len = 0;
	if (body && len && !strcmp(c->api, "startsession") && status == 200)
		c->body = session_with_local(body, len, &c->len);
	if (!c->body && body) {
		c->body = malloc(len + 1u);
		if (c->body) { memcpy(c->body, body, len); c->body[len] = 0; c->len = len; }
	}
	c->status = c->body || !body ? status : 0;
	c->state = CALL_ANSWERED;
	deliver();
}

/* the answers that came, to rc_client: outside any of its own calls */
static void deliver(void)
{
	int i, again = 1;
	if (in_call) return;
	while (again) {
		again = 0;
		for (i = 0; i < CALLS; i++) {
			struct call *c = &calls[i];
			rc_api_server_response_t r;
			if (c->state != CALL_ANSWERED) continue;
			memset(&r, 0, sizeof r);
			r.body = c->body ? c->body : "";
			r.body_length = c->len;
			/* not reached: rc_client tries again later (and says it is disconnected) */
			r.http_status_code = c->status ? c->status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
			in_call++;
			c->callback(&r, c->data);
			in_call--;
			free(c->body);
			c->body = 0;
			c->state = CALL_FREE;
			again = 1;	/* the callback may have answered another */
		}
	}
}

static void ex_set_online(int on) { g_online = on != 0; }

/* ---- memory ---------------------------------------------------------------------------------------- */

/* a core's pico_memdesc is read as libretro's descriptor: the same layout, checked here */
_Static_assert(sizeof(struct pico_memdesc) == sizeof(struct retro_memory_descriptor), "pico_memdesc size");
_Static_assert(offsetof(struct pico_memdesc, ptr) == offsetof(struct retro_memory_descriptor, ptr), "pico_memdesc ptr");
_Static_assert(offsetof(struct pico_memdesc, start) == offsetof(struct retro_memory_descriptor, start), "pico_memdesc start");
_Static_assert(offsetof(struct pico_memdesc, len) == offsetof(struct retro_memory_descriptor, len), "pico_memdesc len");
_Static_assert(offsetof(struct pico_memdesc, addrspace) == offsetof(struct retro_memory_descriptor, addrspace),
	       "pico_memdesc addrspace");
_Static_assert(PICO_MEMDESC_SYSTEM_RAM == RETRO_MEMDESC_SYSTEM_RAM && PICO_MEMDESC_CONST == RETRO_MEMDESC_CONST,
	       "pico_memdesc flags");

static struct pico_core_memory g_mem;
static rc_libretro_memory_regions_t g_regions;
static int g_regions_up;

static void core_memory_info(uint32_t id, rc_libretro_core_memory_info_t *info)
{
	info->data = 0;
	info->size = 0;
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM: info->data = g_mem.save_ram; info->size = g_mem.save_ram_size; break;
	case RETRO_MEMORY_SYSTEM_RAM: info->data = g_mem.system_ram; info->size = g_mem.system_ram_size; break;
	case RETRO_MEMORY_VIDEO_RAM: info->data = g_mem.video_ram; info->size = g_mem.video_ram_size; break;
	default: break;
	}
	if (!info->data) info->size = 0;
}

static uint32_t read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
	(void)client;
	if (!g_regions_up) return 0;
	return rc_libretro_memory_read(&g_regions, address, buffer, num_bytes);
}

/* ---- the account ------------------------------------------------------------------------------------ */

static void login_done(int result, const char *error_message, rc_client_t *client, void *userdata)
{
	struct ra_event *e = ev_push(RA_EV_LOGIN);
	(void)client; (void)userdata;
	e->ok = result == RC_OK;
	if (e->ok) {
		const rc_client_user_t *u = rc_client_get_user_info(g_client);
		copy_text(e->title, sizeof e->title, u ? u->display_name : "");
		e->value = u ? (int)u->score_softcore : 0;
	} else {
		copy_text(e->text, sizeof e->text, error_message ? error_message : rc_error_str(result));
	}
}

static void ex_login_password(const char *user, const char *password)
{
	if (g_client) rc_client_begin_login_with_password(g_client, user, password, login_done, 0);
}

static void ex_login_token(const char *user, const char *token)
{
	if (g_client) rc_client_begin_login_with_token(g_client, user, token, login_done, 0);
}

static void ex_logout(void)
{
	if (g_client) rc_client_logout(g_client);
}

static int ex_token(char *out, unsigned cap)
{
	const rc_client_user_t *u = g_client ? rc_client_get_user_info(g_client) : 0;
	if (!u || !u->token || strlen(u->token) + 1u > cap) return -1;
	copy_text(out, cap, u->token);
	return 0;
}

/* ---- the game ------------------------------------------------------------------------------------- */

static void ex_set_local_unlocks(const unsigned *ids, int n)
{
	free(local_ids);
	local_ids = 0;
	local_n = 0;
	if (n <= 0 || !ids) return;
	local_ids = malloc((unsigned)n * sizeof *local_ids);
	if (!local_ids) return;
	memcpy(local_ids, ids, (unsigned)n * sizeof *local_ids);
	local_n = n;
}

static void load_done(int result, const char *error_message, rc_client_t *client, void *userdata)
{
	struct ra_event *e = ev_push(RA_EV_GAME);
	(void)client; (void)userdata;
	e->ok = result == RC_OK && rc_client_is_game_loaded(g_client);
	if (e->ok) {
		const rc_client_game_t *g = rc_client_get_game_info(g_client);
		rc_client_user_game_summary_t s;
		rc_client_get_user_game_summary(g_client, &s);
		copy_text(e->title, sizeof e->title, g ? g->title : "");
		copy_text(e->badge, sizeof e->badge, g ? g->badge_name : "");
		e->value = (int)s.num_unlocked_achievements;
		e->id = s.num_core_achievements;
	} else {
		copy_text(e->text, sizeof e->text, error_message ? error_message : rc_error_str(result));
	}
}

static void ex_load_game(unsigned console, const void *rom, unsigned rom_size, const struct pico_core_memory *mem)
{
	struct retro_memory_map map;
	if (!g_client) return;
	if (g_regions_up) { rc_libretro_memory_destroy(&g_regions); g_regions_up = 0; }
	memset(&g_mem, 0, sizeof g_mem);
	if (mem) g_mem = *mem;
	map.descriptors = (const struct retro_memory_descriptor *)g_mem.descriptors;
	map.num_descriptors = g_mem.num_descriptors;
	g_regions_up = rc_libretro_memory_init(&g_regions, g_mem.num_descriptors ? &map : 0, core_memory_info, console);
	if (!g_regions_up) say("ra: no memory regions for console %s%u", "", console);
	rc_client_begin_identify_and_load_game(g_client, console, 0, (const uint8_t *)rom, rom_size, load_done, 0);
}

static void ex_unload_game(void)
{
	if (g_client) rc_client_unload_game(g_client);
	if (g_regions_up) { rc_libretro_memory_destroy(&g_regions); g_regions_up = 0; }
}

static void ex_reset(void) { if (g_client) rc_client_reset(g_client); }
static void ex_do_frame(void) { if (g_client) rc_client_do_frame(g_client); deliver(); }
static void ex_idle(void) { if (g_client) rc_client_idle(g_client); deliver(); }

/* ---- unlocks kept on the card --------------------------------------------------------------------- */

static int ex_award_request(const struct ra_queued *q, char *url, unsigned url_cap, char *post, unsigned post_cap)
{
	const rc_client_user_t *u = g_client ? rc_client_get_user_info(g_client) : 0;
	rc_api_award_achievement_request_t p;
	rc_api_request_t r;
	long long now = pico_host_seconds();
	int rc;
	if (!u || !u->token || !q || !q->id) return -1;
	memset(&p, 0, sizeof p);
	p.username = u->username;
	p.api_token = u->token;
	p.achievement_id = q->id;
	p.hardcore = 0;
	p.game_hash = q->hash[0] ? q->hash : 0;
	/* the server takes the unlock as that long ago (0 when the time is not known on either side) */
	if (q->when > 0 && now > q->when) p.seconds_since_unlock = (uint32_t)(now - q->when);
	rc = rc_api_init_award_achievement_request(&r, &p);
	if (rc != RC_OK) return -1;
	rc = (r.url && strlen(r.url) < url_cap && (!r.post_data || strlen(r.post_data) < post_cap)) ? 0 : -1;
	if (!rc) {
		copy_text(url, url_cap, r.url);
		copy_text(post, post_cap, r.post_data ? r.post_data : "");
	}
	rc_api_destroy_request(&r);
	return rc;
}

static int ex_award_answer(int status, const char *body, unsigned len)
{
	rc_api_server_response_t sr;
	rc_api_award_achievement_response_t resp;
	int rc, ok;
	if (!status || status >= 500 || status == 429 || !body) return 0;	/* later */
	memset(&sr, 0, sizeof sr);
	sr.body = body;
	sr.body_length = len;
	sr.http_status_code = status;
	rc = rc_api_process_award_achievement_server_response(&resp, &sr);
	ok = rc == RC_OK && resp.response.succeeded;
	if (!ok && resp.response.error_message) say("ra: unlock refused: %s%u", resp.response.error_message, 0);
	rc_api_destroy_award_achievement_response(&resp);
	if (ok) return 1;
	return rc == RC_OK || rc == RC_INVALID_JSON ? -1 : 0;
}

/* ---- what the menu shows -------------------------------------------------------------------------- */

static void ex_summary(struct ra_summary *out)
{
	const rc_client_user_t *u;
	const rc_client_game_t *g;
	memset(out, 0, sizeof *out);
	if (!g_client) return;
	u = rc_client_get_user_info(g_client);
	if (u) {
		out->logged_in = 1;
		copy_text(out->user, sizeof out->user, u->display_name);
		out->score = u->score_softcore;
	}
	g = rc_client_get_game_info(g_client);
	if (g && rc_client_is_game_loaded(g_client)) {
		rc_client_user_game_summary_t s;
		out->game_loaded = 1;
		out->game_id = g->id;
		copy_text(out->game, sizeof out->game, g->title);
		copy_text(out->hash, sizeof out->hash, g->hash);
		rc_client_get_user_game_summary(g_client, &s);
		out->unlocked = s.num_unlocked_achievements;
		out->total = s.num_core_achievements;
		out->points_unlocked = s.points_unlocked;
		out->points_total = s.points_core;
		rc_client_get_rich_presence_message(g_client, out->rich, sizeof out->rich);
	}
}

static rc_client_achievement_list_t *g_list;
static const rc_client_achievement_t **g_flat;
static const uint8_t *g_flat_bucket;
static int g_flat_n;

static int ex_achievement_count(void)
{
	unsigned b, i;
	int n = 0;
	if (g_list) { rc_client_destroy_achievement_list(g_list); g_list = 0; }
	free(g_flat); g_flat = 0;
	free((void *)g_flat_bucket); g_flat_bucket = 0;
	g_flat_n = 0;
	if (!g_client || !rc_client_is_game_loaded(g_client)) return 0;
	g_list = rc_client_create_achievement_list(g_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
						   RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
	if (!g_list) return 0;
	for (b = 0; b < g_list->num_buckets; b++) n += (int)g_list->buckets[b].num_achievements;
	if (!n) return 0;
	g_flat = malloc((unsigned)n * sizeof *g_flat);
	g_flat_bucket = malloc((unsigned)n);
	if (!g_flat || !g_flat_bucket) return 0;
	for (b = 0; b < g_list->num_buckets; b++)
		for (i = 0; i < g_list->buckets[b].num_achievements; i++) {
			g_flat[g_flat_n] = g_list->buckets[b].achievements[i];
			((uint8_t *)g_flat_bucket)[g_flat_n++] = g_list->buckets[b].bucket_type;
		}
	return g_flat_n;
}

static int ex_achievement(int i, struct ra_achievement *out)
{
	const rc_client_achievement_t *a;
	memset(out, 0, sizeof *out);
	if (i < 0 || i >= g_flat_n) return -1;
	a = g_flat[i];
	out->id = a->id;
	out->points = a->points;
	switch (g_flat_bucket[i]) {
	case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED:
	case RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED: out->state = RA_ACH_UNLOCKED; break;
	case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED: out->state = RA_ACH_UNSUPPORTED; break;
	case RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE: out->state = RA_ACH_ALMOST; break;
	default: out->state = a->unlocked ? RA_ACH_UNLOCKED : RA_ACH_LOCKED; break;
	}
	out->percent = a->measured_progress[0] ? (int)(a->measured_percent + .5f) : -1;
	out->when = a->unlocked ? (long long)a->unlock_time : 0;
	copy_text(out->title, sizeof out->title, a->title);
	copy_text(out->text, sizeof out->text, a->description);
	copy_text(out->measured, sizeof out->measured, a->measured_progress);
	copy_text(out->badge, sizeof out->badge, a->badge_name);
	return 0;
}

/* ---- save states ---------------------------------------------------------------------------------- */

static unsigned ex_progress_size(void)
{
	return g_client && rc_client_is_game_loaded(g_client) ? (unsigned)rc_client_progress_size(g_client) : 0;
}

static int ex_progress_save(void *buf, unsigned cap)
{
	if (!g_client) return -1;
	return rc_client_serialize_progress_sized(g_client, (uint8_t *)buf, cap) == RC_OK ? 0 : -1;
}

static int ex_progress_load(const void *buf, unsigned len)
{
	if (!g_client) return -1;
	return rc_client_deserialize_progress_sized(g_client, len ? (const uint8_t *)buf : 0, len) == RC_OK ? 0 : -1;
}

/* ---- set up --------------------------------------------------------------------------------------- */

static rc_clock_t now_ms(const rc_client_t *client)
{
	(void)client;
	return g_im && g_im->now_ms ? (rc_clock_t)g_im->now_ms() : 0;
}

static void rc_log(const char *message, const rc_client_t *client)
{
	(void)client;
	if (g_im && g_im->log) g_im->log(message);
}

static void ex_heap_init(void *base, unsigned size) { pico_heap_init(base, size); }

static int ex_init(void)
{
	if (g_client) return 0;
	memset(calls, 0, sizeof calls);
	ev_head = ev_tail = 0;
	g_client = rc_client_create(read_memory, server_call);
	if (!g_client) return -1;
	rc_client_set_hardcore_enabled(g_client, 0);	/* softcore only */
	rc_client_set_unofficial_enabled(g_client, 0);
	rc_client_set_encore_mode_enabled(g_client, 0);
	rc_client_set_spectator_mode_enabled(g_client, 0);
	rc_client_set_event_handler(g_client, on_event);
	rc_client_set_get_time_millisecs_function(g_client, now_ms);
	rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, rc_log);
	return 0;
}

static void ex_shutdown(void)
{
	int i;
	if (g_list) { rc_client_destroy_achievement_list(g_list); g_list = 0; }
	free(g_flat); g_flat = 0;
	free((void *)g_flat_bucket); g_flat_bucket = 0;
	g_flat_n = 0;
	if (g_regions_up) { rc_libretro_memory_destroy(&g_regions); g_regions_up = 0; }
	if (g_client) { rc_client_destroy(g_client); g_client = 0; }
	for (i = 0; i < CALLS; i++) { free(calls[i].body); calls[i].body = 0; calls[i].state = CALL_FREE; }
	ex_set_local_unlocks(0, 0);
}

static void ex_user_agent_clause(char *out, unsigned cap)
{
	if (g_client) rc_client_get_user_agent_clause(g_client, out, cap);
	else if (cap) snprintf(out, cap, "rcheevos/%s", RCHEEVOS_VERSION_STRING);
}

static const struct ra_core_exports g_exports = {
	.magic = RA_CORE_ABI_MAGIC,
	.version = RA_CORE_ABI_VERSION,
	.heap_init = ex_heap_init,
	.init = ex_init,
	.shutdown = ex_shutdown,
	.user_agent_clause = ex_user_agent_clause,
	.set_online = ex_set_online,
	.login_password = ex_login_password,
	.login_token = ex_login_token,
	.logout = ex_logout,
	.token = ex_token,
	.set_local_unlocks = ex_set_local_unlocks,
	.load_game = ex_load_game,
	.unload_game = ex_unload_game,
	.reset = ex_reset,
	.do_frame = ex_do_frame,
	.idle = ex_idle,
	.answer = ex_answer,
	.next_event = ex_next_event,
	.award_request = ex_award_request,
	.award_answer = ex_award_answer,
	.summary = ex_summary,
	.achievement_count = ex_achievement_count,
	.achievement = ex_achievement,
	.progress_size = ex_progress_size,
	.progress_save = ex_progress_save,
	.progress_load = ex_progress_load,
};

__attribute__((used, section(".text.ra_core_blob_init")))
const struct ra_core_exports *ra_core_blob_init(const struct ra_core_imports *imp)
{
	g_im = imp;
	return &g_exports;
}
