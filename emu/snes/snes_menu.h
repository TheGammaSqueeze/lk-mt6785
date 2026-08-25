/*
 * Portable SNES home-menu logic + drawing (no LK dependencies), driven by the
 * LK driver on-device and by a host render harness for local validation against
 * the web app. The caller provides work memory (rnode pools + wallpaper cache):
 * fixed DRAM on LK, malloc on the host.
 */
#ifndef SNES_MENU_H
#define SNES_MENU_H

#include "snes_render.h"

typedef struct {
	int left, right, up, down, a, b, start, select;   /* level: 1 = pressed */
} snes_input;

typedef struct {
	const snes_pack *pk;
	snes_scene home, bg;
	snes_rnode *wall;
	snes_rnode *homemenu;         /* home-state subtree in the home scene */
	snes_rnode *menubar;          /* menubar_upper subtree in the home scene */
	snes_rnode *mb_btn[5];        /* the 5 menubar icon buttons (cm1..cm5) */
	snes_rnode *mb_active[5];      /* per-button cyan active-highlight sprite node */
	float mb_scale[5];             /* per-icon focus scale (animates toward 1.2) */
	snes_rnode *mb_caption[5];     /* per-icon caption bubble (shown when focused) */
	snes_rnode *overlay[5];       /* settings overlays opened per icon */

	snes_rnode *resume;           /* resume/suspend-point menu overlay */
	int state;                    /* 0 home, 1 menubar, 2 submenu, 3 resume */
	int mb_focus;                 /* selected menubar icon 0..4 */
	int open;                     /* open overlay index, -1 if none */

	/* roster sort: order[] indirects the carousel/filmstrip into the game table */
	unsigned short order[128];
	int ngames;
	int sort_rule;                /* 0 title, 1 publisher, 2 players, 3 release */
	float sort_label_t;           /* seconds remaining to show the sort-name label */
	uint32_t *wp;                 /* wallpaper cache, WP_W*WP_H u32 (caller-provided) */
	int wp_ready;
	uint32_t *chrome;             /* cached static home chrome, VW*VH u32 (0 alpha = uncovered) */
	int chrome_ready;
	float scroll;

	int focus;                    /* selected game index */
	float car_x;                  /* legacy smooth carousel x (filmstrip) */
	float car_target;
	float sel_world;              /* focused card world x (walks the dead zone) */
	float cont_shift;             /* container scroll offset, animates back to 0 */

	/* authored game-card frame (from sys_game_card.scn): the cartridge-display
	 * sprite drawn around each boxart. active = focused, norm = unfocused. */
	const snes_spr_entry *card_act, *card_norm, *card_dot, *card_dot_on;
	float card_fw, card_fh;       /* frame draw size (native*3 = 252x276) */
	float screen_w, screen_h, screen_oy;  /* boxart area within the card */

	/* input edge state */
	int pl, pr, pu, pd, pa, pb, ps;

	/* sound event queue (res-hash of sounds to play; driver drains it) */
	uint32_t sndq[8];
	int sndh, sndt;

	/* cached font-name hashes */
	uint32_t f_title, f_l, f_s;
	/* cached sfx/bgm res hashes (0 if absent) */
	uint32_t sfx_move, sfx_decide, sfx_cancel, sfx_up, bgm;
} snes_menu;

/* wallpaper cache dims (caller allocates WP_CACHE_W*WP_CACHE_H u32). */
#define WP_CACHE_W 1536
#define WP_CACHE_H 720

/* Initialise. home_pool/bg_pool are snes_rnode arrays of the given capacities;
 * wp is a WP_CACHE_W*WP_CACHE_H u32 buffer. Returns 0 on success. */
int snes_menu_init(snes_menu *m, const snes_pack *pk,
		   snes_rnode *home_pool, unsigned home_cap,
		   snes_rnode *bg_pool, unsigned bg_cap,
		   uint32_t *wp, uint32_t *chrome);

void snes_menu_update(snes_menu *m, const snes_input *in, float dt);
void snes_menu_render(snes_menu *m, snes_target *t);

/* Drain one queued sound (res hash) to play, or 0 if none. */
uint32_t snes_menu_next_sound(snes_menu *m);

#endif
