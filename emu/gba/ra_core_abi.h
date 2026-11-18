/*
 * ra_core_abi.h - the contract between the LK and the RetroAchievements blob (ra-pico).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * RetroAchievements' own library, rcheevos (rc_client, MIT), runs as a blob the LK copies from the
 * card into its slot (RA_SLOT_*) when achievements are on, and drives through the export table
 * below. The blob only wraps rc_client: it opens no files and talks to no network. Its server
 * calls go out through request() and their answers come back through answer(); the LK decides
 * whether that is the network or, with none, the answers it keeps on the card. Softcore only:
 * hardcore is never turned on.
 *
 * Offline: unlocks never wait for the network. Each one is answered at once inside the blob (the
 * player's score and the set's remaining count worked out here) and handed to the LK as an
 * RA_EV_QUEUED event: the achievement, the game's hash and when, which the LK keeps on the card.
 * When the network is there again, award_request() makes the real server call for each kept one,
 * with the seconds since the unlock, so the server records the time it happened. Unlocks still
 * in that queue are given back with set_local_unlocks() before a game loads, so a set read from
 * the card's copy shows them unlocked.
 *
 * The game's memory: the running core's libretro memory map and memory blocks (pico_core_memory),
 * read in place (core and blob share the address space). Events (unlocks, progress, a game's set
 * loaded) wait in a queue the LK empties with next_event().
 */
#ifndef RA_CORE_ABI_H
#define RA_CORE_ABI_H

#define RA_CORE_ABI_MAGIC   0x31414152u   /* "RAA1" */
#define RA_CORE_ABI_VERSION 2u            /* blob.ld's header carries it too: change both together */

/* the slot: right after the Wi-Fi DMA memory (pico_bt.h), kept out of the rewind arena with it;
 * the image at its start, the heap the LK hands to heap_init after it */
#define RA_SLOT_PA          0xBE600000u
#define RA_SLOT_SIZE        0x00400000u

/* the game's memory, as the running core describes it */
#include "pico_core_memory.h"

/* what happened, for the LK to show or keep (next_event) */
enum {
	RA_EV_NONE,
	RA_EV_LOGIN,          /* ok: 1 signed in (title: the name shown, value: softcore points), 0 refused (text: why) */
	RA_EV_GAME,           /* ok: 1 the set loaded (title: the game; value: unlocked, id: total), 0 none or failed (text) */
	RA_EV_UNLOCK,         /* id, title, text (description), value: points, badge */
	RA_EV_MASTERED,       /* title: the game, value: points of the set */
	RA_EV_PROGRESS,       /* id, title, text: "23/50", value: percent (0..100), badge */
	RA_EV_PROGRESS_HIDE,
	RA_EV_CHALLENGE,      /* id, title, badge: a challenge shows (value 1) or goes (value 0) */
	RA_EV_SERVER_ERROR,   /* text */
	RA_EV_QUEUED,         /* an unlock to keep on the card: id, hash, when; ok 1 earned in hardcore */
	RA_EV_DISCONNECTED,   /* rcheevos could not reach the server for a while */
	RA_EV_RECONNECTED,
	RA_EV_RESET           /* hardcore came on: the game must start again from its power on (the
	                       * LK resets the core, then calls reset()) */
};

struct ra_event {
	int kind;
	int ok;
	unsigned id;
	int value;
	long long when;       /* RA_EV_QUEUED: the unlock's time, seconds since 1970 (0 not known) */
	char hash[33];        /* RA_EV_QUEUED: the game's hash */
	char title[96];
	char text[160];
	char badge[16];       /* the badge's name ("12345"), for its picture */
};

/* an unlock kept on the card until the server has it */
struct ra_queued {
	unsigned id;          /* the achievement */
	char hash[33];        /* the game's hash */
	unsigned char hardcore;   /* 1 earned in hardcore (in what was padding: the size is unchanged) */
	long long when;       /* seconds since 1970, 0 not known */
};

/* one achievement, for the menu's list (achievement()) */
enum { RA_ACH_LOCKED, RA_ACH_UNLOCKED, RA_ACH_UNSUPPORTED, RA_ACH_ALMOST };
struct ra_achievement {
	unsigned id;
	unsigned points;
	int state;            /* RA_ACH_* */
	int percent;          /* measured progress, -1 none */
	long long when;       /* unlocked: when (seconds since 1970), else 0 */
	char title[96];
	char text[160];
	char measured[24];    /* "23/50" or "" */
	char badge[16];
};

struct ra_summary {
	int logged_in;
	char user[64];
	unsigned score;               /* softcore points */
	int game_loaded;
	unsigned game_id;
	char game[96];
	char hash[33];
	unsigned unlocked, total;     /* this game's achievements */
	unsigned points_unlocked, points_total;
	char rich[128];               /* rich presence */
	int hardcore;                 /* rcheevos counts in hardcore now */
	/* RetroAchievements' verdict on this client, from the game's set: 1 validated (hardcore
	 * unlocks count), 0 not (its set carries the warning entry, id 101000001 and up: hardcore
	 * unlocks count as softcore), -1 not known (no set read yet, or one without achievements) */
	int verdict;
	unsigned score_hardcore;      /* hardcore points */
};

/* Services the LK gives the blob. */
struct ra_core_imports {
	/* a server call: the URL, the form (POST body, may be 0) and its content type; api is the
	 * call's name ("login2", "achievementsets", "startsession", ...) and key what it is about (the
	 * user, a game's hash or id), for the LK's copies on the card. Its answer comes back through
	 * answer(id); never from inside this call. */
	void (*request)(unsigned id, const char *url, const char *post, const char *content_type, const char *api,
			const char *key);
	unsigned long long (*now_ms)(void);    /* a millisecond clock */
	long long (*utc)(void);                /* seconds since 1970 (0 not known) */
	void (*log)(const char *line);
};

struct ra_core_exports {
	unsigned int magic;                    /* RA_CORE_ABI_MAGIC */
	unsigned int version;                  /* RA_CORE_ABI_VERSION */
	void (*heap_init)(void *base, unsigned size);
	int  (*init)(void);                    /* 0 ready */
	void (*shutdown)(void);
	/* rcheevos' part of the HTTP user agent ("rcheevos/12.2.1"), for the LK to add to its own */
	void (*user_agent_clause)(char *out, unsigned cap);

	/* 1 while server calls can reach the network; 0 (the default) answers the ones that can wait
	 * (unlocks, rich presence) inside the blob */
	void (*set_online)(int on);

	/* the account */
	void (*login_password)(const char *user, const char *password);
	void (*login_token)(const char *user, const char *token);
	void (*logout)(void);
	/* after RA_EV_LOGIN ok: the token to keep (on the card) */
	int  (*token)(char *out, unsigned cap);

	/* the unlocks still on the card's queue (any game), before load_game; an id with bit 31 set was
	 * earned in hardcore */
	void (*set_local_unlocks)(const unsigned *ids, int n);
	/* a game: its console (rc_consoles.h RC_CONSOLE_*), its ROM as loaded, the core's memory */
	void (*load_game)(unsigned console, const void *rom, unsigned rom_size, const struct pico_core_memory *mem);
	void (*unload_game)(void);
	void (*reset)(void);                   /* the game was reset */
	void (*do_frame)(void);                /* after every emulated frame */
	void (*idle)(void);                    /* about once a second while the game is paused */

	/* a server call's answer: HTTP status (0 when the server was not reached), the body */
	void (*answer)(unsigned id, int status, const char *body, unsigned len);
	int  (*next_event)(struct ra_event *ev);

	/* the server call for a kept unlock (signed in): 0 made, url and post filled */
	int  (*award_request)(const struct ra_queued *q, char *url, unsigned url_cap, char *post, unsigned post_cap);
	/* that call's answer: 1 the server has it (or had it), 0 try again later, -1 refused for good */
	int  (*award_answer)(int status, const char *body, unsigned len);

	/* what the menu shows */
	void (*summary)(struct ra_summary *out);
	int  (*achievement_count)(void);       /* builds the list achievement() reads */
	int  (*achievement)(int i, struct ra_achievement *out);

	/* rcheevos' progress in a save state: size, save into buf, load (0 bytes: start over) */
	unsigned (*progress_size)(void);
	int  (*progress_save)(void *buf, unsigned cap);
	int  (*progress_load)(const void *buf, unsigned len);

	/* version 2: hardcore. On: from the next reset (a game running gets RA_EV_RESET); off: at once */
	void (*set_hardcore)(int on);
	/* the in-game menu may pause the game now: 1 yes; 0 not yet in hardcore (*frames_left until it
	 * may, rc_client_can_pause) */
	int  (*can_pause)(unsigned *frames_left);
};

typedef const struct ra_core_exports *(*ra_core_entry_fn)(const struct ra_core_imports *imp);

#endif
