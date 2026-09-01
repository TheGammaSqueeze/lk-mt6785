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
	int lb, rb;                                       /* L/R shoulder: page jump */
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

	/* Display submenu (option_display) interactivity: the screen-mode line is a
	 * 3-item radio row [CRTFilter, 4:3, DotByDot] in visual order. The cursor
	 * (cursor_area blue box) moves independently of the selection (radiobtn_on);
	 * only A commits the selection to the cursor. sub_rep_* is the L/R auto-repeat
	 * (navFire, DISP_HOLD_DELAY/RATE). */
	int disp_cur;                 /* mode-line cursor index 0..2 (visual order) */
	int disp_sel;                 /* committed screen mode 0..2 (default 1 = 4:3) */
	/* Display frame carousel: 12 items (0 = none, 1..11 = frame themes), a 4-wide
	 * scroll window. disp_zone 0 = mode line, 1 = frame carousel. */
	int disp_zone;                /* 0 mode-line focus, 1 frame-carousel focus */
	int frame_sel;                /* frame cursor 0..11 */
	int frame_scroll;             /* scroll-window start 0..8 */
	int frame_applied;            /* applied frame 0..11 (default 0 = none) */
	float sub_rep_t;              /* submenu D-pad auto-repeat countdown (navFire) */
	int sub_rep_ctrl;             /* held D-pad for repeat: 1 up, 2 down, 3 left, 4 right, 0 none */

	/* Options submenu (option_settings): a vertical list of 3 toggle rows
	 * (setting0/1/2, top->bottom) + the System Reset button. opt_cur is the cursor
	 * row 0..3; opt_on bit i = toggle i on/off (default all on). */
	int opt_cur;                  /* Options cursor row 0..3 */
	unsigned opt_on;              /* toggle on/off bitmask (bit i = setting i) */

	/* Language submenu (option_languages): 2D radio grid, 4 rows x 2 cols in
	 * row-major order [language01,05, 02,06, 03,07, 04,08]. lang_cur = cursor cell,
	 * lang_sel = the selected (radiobtn_on) language, both 0..7 (default 0 = English). */
	int lang_cur;                 /* Language cursor grid index 0..7 */
	int lang_sel;                 /* selected language grid index 0..7 */

	/* Legal (copyright) screen: IP Notice / Open Source Software tabs + page-scroll. */
	int legal_tab;                /* 0 = IP Notice, 1 = Open Source Software */
	int legal_scroll;             /* first visible line index in the active tab */

	/* Options System Reset long-press (sys_button_longpress) + confirm dialog. */
	float reset_t;                /* OK-held time over the reset row (0..RESET_LONGPRESS_SEC) */
	int reset_armed;             /* armed on the A press edge over the reset row */
	int reset_dlg_open;          /* Yes/No confirm dialog showing */
	int dlg_focus;               /* dialog button focus: 0 = Cancel, 1 = Reset */
	float reset_dlg_y;           /* dialog slide offset, eases -720 -> 0 on open */
	float reset_dlg_t;           /* dialog open elapsed (0.2s outExpo slide-in) */
	int mb_focus;                 /* selected menubar icon 0..4 */
	int open;                     /* open overlay index, -1 if none */
	float open_y;                 /* submenu slide offset (world up), eases to 0 */
	int closing;                  /* submenu sliding back up/off before it hides */
	float close_t;                /* close-slide elapsed time (cubic ease-in) */
	float close_target;           /* close-slide open_y target (+up submenu / -down resume) */
	float clock;                  /* free-running seconds accumulator (cursor blink anim) */
	float cur_slide_t;            /* carousel<->menubar cursor slide elapsed (>=CUR_SLIDE_DUR = idle) */
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
	/* 4:3 fast-path: the ASP_WALL_S-warped wallpaper pre-rendered so draw_wp_43 is a
	 * per-row memcpy-scroll instead of a per-pixel gather (the gather straddled the
	 * vsync budget on device). Caller-provided WP43_H*WP43_PERIOD u32; 0 = gather. */
	uint32_t *wp43;
	int wp43_ready;
	uint32_t *chrome;             /* cached static home chrome, VW*VH u32 (0 alpha = uncovered) */
	int chrome_ready;
	/* resume/suspend-list panel cache: the panel is static once slid in, so render it
	 * ONCE into an fb-size overlay (0 alpha uncovered) and composite it each settled
	 * frame instead of re-walking its scene subtree (the resume state's dominant cost).
	 * Caller-provides an fb-size buffer (pitch*H u32); 0 = always render live. */
	uint32_t *rcache;
	int rcache_ready, rcache_y0, rcache_y1;
	int rcache_op0, rcache_op1;   /* fully-opaque backing band of the resume panel */
	float rcache_sel;             /* sel_world the panel was cached for (chevron pos) */
	/* settled-submenu panel cache: the open submenu (state 2) is static content that
	 * was re-walking the whole scene graph every frame (~56ms = 15fps). Cache it into
	 * the rcache buffer (shared; the two states are mutually exclusive) once settled and
	 * composite it, rebuilding only when the panel or a selection changes (sub_key). */
	int sub_ready, sub_y0, sub_y1;
	int sub_op0, sub_op1;         /* fully-opaque row band (the panel backing) */
	unsigned sub_key;             /* signature of open + all interactive submenu state */
	int wp_skip0, wp_skip1;       /* transient: draw_wp_43 skips rows [skip0,skip1) (hidden by an opaque overlay) */
	/* focused-card body tile: the blue (active) card body is identical for every GBA
	 * cart, so cache it ONCE (like ctile) and blit it + draw only the pulsing cursor
	 * live - kills the last per-frame live card render (~2.7ms on the A55). */
	uint32_t *fct;
	int fct_ready, fct_aspect, no_cursor;
	/* Card-tile cache (single-buffer 60fps): each game's NORMAL card body (dark frame +
	 * boxart + player icon + resume dots, dim=1, no cursor) is rendered ONCE into a
	 * CT_W*CT_H straight-RGBA tile and blitted per frame by draw_carousel instead of ~6
	 * live sprite blits. Direct-mapped by game index (slot = gi % ctile_cap) so a sustained
	 * held scroll reuses tiles and never rebuilds. Caller provides the buffer + slot array. */
	uint32_t *ctile;              /* ctile_cap * CT_W*CT_H u32, or 0 = disabled (live render) */
	int *ctile_gi;                /* per-slot cached game index, -1 = empty (ctile_cap ints) */
	int ctile_cap;                /* number of tile slots */
	int ctile_aspect;             /* aspect the cached tiles were built for; -1 = none */
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
	float jmp_t;                  /* L/R shoulder page-jump auto-repeat countdown */
	int   jmp_dir;                /* held shoulder: -1 L, +1 R, 0 none */
	float car_tween;              /* current card-slide tween time (HGAP/rate) */
	int   car_navd;               /* a nav happened this frame: skip one cont_shift step (web tween starts next frame) */

	int focus;                    /* selected game index */
	float car_x;                  /* legacy smooth carousel x (filmstrip) */
	float car_target;
	float sel_world;              /* focused card world x (walks the dead zone) */
	float cont_shift;             /* container scroll offset, animates back to 0 */
	int   prev_focus;             /* card focused before the last nav (blue-frame crossfade) */
	float xfade_t;                /* seconds left in the 0.2s blue-frame crossfade */
	float resume_dim;             /* 0..1 fade of the resume card-darken (non-focused cards -> 0.5) */
	float resume_expl_t;          /* seconds since the empty resume list opened (explanation hold+fade) */

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

	/* ---- GBA-from-SD adaptation ----
	 * The SNES engine renders 1:1, but the roster is the microSD GBA ROM list
	 * (runtime, not baked in the pack), the per-card boxart is a single GBA
	 * cartridge placeholder image, and A/Start launches the focused ROM. */
	int gba_mode;                       /* 1 = GBA roster/cart/launch active */
	const char *const *gba_names;       /* ngames display names (ROM file names) */
	const snes_img_entry *gba_cart_img; /* cartridge placeholder, drawn as every boxart */
	int launch;                         /* focused ROM index to launch, -1 = none */
	int pstart;                         /* prev-frame Start (edge detect for launch) */
} snes_menu;

/* Switch the menu to GBA roster mode: names[0..n) are the SD ROM display names,
 * cart is the placeholder cartridge image drawn as every card's boxart. Call
 * after snes_menu_init. */
void snes_menu_set_gba_roster(snes_menu *m, const char *const *names, int n,
			      const snes_img_entry *cart);
/* If A/Start launched a ROM, returns its index and clears the flag; else -1. */
int snes_menu_take_launch(snes_menu *m);

/* Overwrite the label of Options toggle row idx (0..2) in place; lets the LK layer
 * repurpose the cosmetic toggles as real settings. 0 ok, <0 if it does not fit. */
int snes_menu_relabel_option(snes_menu *m, int idx, const char *text);
/* Re-apply the Options toggle sprites after the LK layer seeds opt_on. */
void snes_menu_apply_options(snes_menu *m);

/* wallpaper cache dims (caller allocates WP_CACHE_W*WP_CACHE_H u32). */
#define WP_CACHE_W 1536
#define WP_CACHE_H 720
/* 4:3 warped-wallpaper cache: one horizontal period in SCREEN pixels (WP_CACHE_W *
 * ASP_WALL_S ~= 2701) x 960 rows. Caller allocates WP43_PERIOD*WP43_H u32 (~10.4MB). */
#define WP43_PERIOD 2701
#define WP43_H 960
/* Rows fully hidden behind the opaque 4:3 chrome bars (menubar top / SUPER NINTENDO
 * bottom): the wallpaper memcpy skips [0,WP43_TOP_SKIP) and [WP43_BOT_KEEP,960) to cut
 * memory traffic on the A55. Host-measured covered = [0,95] + [870,959]; these are a
 * conservative subset (8px margins) and clip-vs-noclip is validated pixel-identical. */
#define WP43_TOP_SKIP 88
#define WP43_BOT_KEEP 878
/* Provide the 4:3 warped-wallpaper cache backing store (WP43_PERIOD*WP43_H u32), or
 * 0 to keep the per-pixel gather. */
void snes_menu_set_wp43(snes_menu *m, uint32_t *buf);
void snes_menu_prewarm(snes_menu *m);
/* Provide the resume-panel overlay cache (fb-size: pitch*H u32), or 0 to disable. */
void snes_menu_set_rcache(snes_menu *m, uint32_t *buf);

/* Initialise. home_pool/bg_pool are snes_rnode arrays of the given capacities;
 * wp is a WP_CACHE_W*WP_CACHE_H u32 buffer. Returns 0 on success. */
int snes_menu_init(snes_menu *m, const snes_pack *pk,
		   snes_rnode *home_pool, unsigned home_cap,
		   snes_rnode *bg_pool, unsigned bg_cap,
		   uint32_t *wp, uint32_t *chrome);

/* Provide the card-tile cache backing store (see snes_menu struct). buf must hold
 * cap * SNES_CT_W * SNES_CT_H u32; gi must hold cap ints. Pass buf=0 to disable the
 * cache (draw_carousel then renders every card live, the b369be3 path). */
void snes_menu_set_ctile(snes_menu *m, uint32_t *buf, int *gi, int cap);
/* Provide the focused (blue) card-body tile cache (SNES_CT_W*SNES_CT_H u32), or 0 to
 * render the focused card live each frame. Identical for all GBA carts. */
void snes_menu_set_fct(snes_menu *m, uint32_t *buf);
/* tile dimensions (one card body); exported so callers can size the buffer */
#define SNES_CT_W 320
#define SNES_CT_H 360

void snes_menu_update(snes_menu *m, const snes_input *in, float dt);
void snes_menu_render(snes_menu *m, snes_target *t);

/* Drain one queued sound (res hash) to play, or 0 if none. */
uint32_t snes_menu_next_sound(snes_menu *m);

#endif
