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
	snes_rnode *mb_icon[5];        /* per-button btn_icon (only this scales to 1.2 on focus) */
	float mb_scale[5];             /* per-icon focus scale (animates toward 1.2) */
	float mb_cell_y0[5];           /* authored button tf[5] (base for focus-down nudge) */
	snes_rnode *mb_caption[5];     /* per-icon caption bubble (shown when focused) */
	snes_rnode *overlay[5];       /* settings overlays opened per icon */

	snes_rnode *gametitle;        /* game-title bar (caption_title); raised in resume */
	snes_rnode *resume;           /* resume/suspend-point menu overlay */
	int state;                    /* 0 home, 1 menubar, 2 submenu, 3 resume */
	int mb_focus;                 /* selected menubar icon 0..4 */
	int open;                     /* open overlay index, -1 if none */
	float open_y;                 /* submenu slide offset (world up), eases to 0 */
	int closing;                  /* submenu sliding back up/off before it hides */
	float close_t;                /* close-slide elapsed time (cubic ease-in) */
	float close_target;           /* close-slide open_y target (+up submenu / -down resume) */
	float clock;                  /* free-running seconds accumulator (cursor blink anim) */
	float cap_t;                  /* time in menubar since focus change (caption delay) */
	float cap_s;                  /* menubar caption scale, eases 0->1 (outExpo, after delay) */
	float hl_s;                   /* menubar focus highlight (cursor) scale, eases 0->1 fast */

	/* roster sort: order[] indirects the carousel/filmstrip into the game table */
	unsigned short order[128];
	int ngames;
	int sort_rule;                /* 0 title, 1 publisher, 2 players, 3 release */
	float sort_label_t;           /* seconds remaining to show the sort-name label */
	uint32_t *wp;                 /* wallpaper cache, WP_W*WP_H u32 (caller-provided) */
	int wp_ready;
	uint32_t *chrome;             /* cached static home chrome, VW*VH u32 (0 alpha = uncovered) */
	int chrome_ready;
	int aspect;                   /* 0 = native 16:9 (letterboxed); 1 = 4:3 (fills the 960 panel) */
	float scroll;

	/* wallpaper parallax (ports CloverScrollBG at a fixed 30fps step) */
	float bg_acc;                 /* dt accumulator for the fixed 30fps parallax step */
	float scr_speed;              /* current_scroll_speed (smoothed anim units/sec) */
	float scr_dir;                /* scroll_dir (+1 right / -1 left, follows last nav) */
	float cur_scroll_time;        /* cursor_scroll_time (burst window countdown) */
	float cur_scroll_spd;         /* cursor_scroll_speed (burst magnitude, signed) */

	/* carousel D-pad auto-repeat (ports GUI.H REPEAT_DELAY/REPEAT_RATE) */
	float rep_t;                  /* time held in the current direction */
	int   rep_dir;                /* held direction: -1 left, +1 right, 0 none */
	int   rep_primed;             /* initial REPEAT_DELAY elapsed -> now REPEAT_RATE */
	float car_tween;              /* current card-slide tween time (HGAP/rate) */

	int focus;                    /* selected game index */
	float car_x;                  /* legacy smooth carousel x (filmstrip) */
	float car_target;
	float sel_world;              /* focused card world x (walks the dead zone) */
	float cont_shift;             /* container scroll offset, animates back to 0 */
	int   prev_focus;             /* card focused before the last nav (blue-frame crossfade) */
	float xfade_t;                /* seconds left in the 0.2s blue-frame crossfade */
	float resume_dim;             /* 0..1 fade of the resume card-darken (non-focused cards -> 0.5) */

	/* authored game-card frame (from sys_game_card.scn): the cartridge-display
	 * sprite drawn around each boxart. active = focused, norm = unfocused. */
	const snes_spr_entry *card_act, *card_norm, *card_dot, *card_dot_on;
	const snes_spr_entry *card_pi;   /* player-count icon (icon_1P base); variants vary sy */
	float card_pi_wx, card_pi_wy;    /* player icon local pos in the card scene */
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
