/*
 * pico_input.c - the console's buttons for every core, the hotkeys and the Controls page. See
 * pico_input.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 */
#include <stdio.h>
#include <string.h>

#include "pico_input.h"
#include "pico_igm.h"
#include "sd_fat.h"
#include "gba_sd_save.h"

/* the menus' words (pico_i18n.c, in the card builds); English without it */
extern const char *tr(const char *en) __attribute__((weak));
static const char *trw(const char *en) { return tr ? tr(en) : en; }

#define CTL_DIR "/gammaos/controls"

/* ---- the board's key lines ---- */

enum { LN_GPIO, LN_KEY };	/* a GPIO (active low), or a key of the MediaTek keypad / PMIC */
struct pi_line { unsigned char kind, n; };

#if defined(KONKR_BLOCK)
/* the gamepad node's fifteen (MODE among them) and the gpio-keys node's four, then the keypad's
 * volume keys (the PMIC home key line, matrix key 0) */
static const struct pi_line k_lines[] = {
	{ LN_GPIO, 25 }, { LN_GPIO, 27 }, { LN_GPIO, 28 }, { LN_GPIO, 26 },	/* up down left right */
	{ LN_GPIO, 4 }, { LN_GPIO, 5 }, { LN_GPIO, 3 }, { LN_GPIO, 6 },		/* A B X Y */
	{ LN_GPIO, 14 }, { LN_GPIO, 10 }, { LN_GPIO, 15 }, { LN_GPIO, 9 },	/* L1 R1 L2 R2 */
	{ LN_GPIO, 7 }, { LN_GPIO, 24 }, { LN_GPIO, 22 },			/* START SELECT, the side key (menu) */
	{ LN_GPIO, 17 }, { LN_GPIO, 21 }, { LN_GPIO, 19 }, { LN_GPIO, 16 },	/* MODE, side key 2, back, volume down */
	{ LN_KEY, 0x11 }, { LN_KEY, 0x00 },
};
static const char *const k_pb_name[PB_EXTRA] = { "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
	"L1", "R1", "L2", "R2", "START", "SELECT", "Menu" };
#elif defined(KONKR_KPA)
/* the gpio-keys node: the caps' X is GPIO 84, Y 85 (the labels in the device tree); L2 and R2 are
 * the lines it calls the stick clicks. Then its other lines (key_rc, key_lc, "KEY_=") and the
 * keypad's two keys. */
static const struct pi_line k_lines[] = {
	{ LN_GPIO, 89 }, { LN_GPIO, 79 }, { LN_GPIO, 78 }, { LN_GPIO, 80 },
	{ LN_GPIO, 82 }, { LN_GPIO, 83 }, { LN_GPIO, 84 }, { LN_GPIO, 85 },
	{ LN_GPIO, 92 }, { LN_GPIO, 81 }, { LN_GPIO, 94 }, { LN_GPIO, 93 },
	{ LN_GPIO, 90 }, { LN_GPIO, 91 }, { LN_GPIO, 86 },
	{ LN_GPIO, 57 }, { LN_GPIO, 58 }, { LN_GPIO, 87 },
	{ LN_KEY, 0x11 }, { LN_KEY, 0x00 },
};
static const char *const k_pb_name[PB_EXTRA] = { "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
	"L", "R", "L2", "R2", "START", "SELECT", "Menu" };
#elif !defined(PICO_INPUT_TEST)
/* the AYANEO Pocket Air Mini: its gpio-keys (A and B, Start and Select the other way round from the
 * device tree's names, as the shell has them), key_rc as R2, key_lc as L2; its stick plays the
 * d-pad (read_lines) */
static const struct pi_line k_lines[] = {
	{ LN_GPIO, 89 }, { LN_GPIO, 79 }, { LN_GPIO, 78 }, { LN_GPIO, 80 },
	{ LN_GPIO, 83 }, { LN_GPIO, 82 }, { LN_GPIO, 84 }, { LN_GPIO, 85 },
	{ LN_GPIO, 92 }, { LN_GPIO, 81 }, { LN_GPIO, 58 }, { LN_GPIO, 57 },
	{ LN_GPIO, 91 }, { LN_GPIO, 90 }, { LN_GPIO, 86 },
	{ LN_GPIO, 87 }, { LN_GPIO, 93 }, { LN_GPIO, 94 },
};
static const char *const k_pb_name[PB_EXTRA] = { "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
	"L", "R", "L2", "R2", "START", "SELECT", "Menu" };
#else
/* the PC test: the lines are only numbers */
static const struct pi_line k_lines[] = {
	{ LN_GPIO, 0 }, { LN_GPIO, 1 }, { LN_GPIO, 2 }, { LN_GPIO, 3 }, { LN_GPIO, 4 }, { LN_GPIO, 5 },
	{ LN_GPIO, 6 }, { LN_GPIO, 7 }, { LN_GPIO, 8 }, { LN_GPIO, 9 }, { LN_GPIO, 10 }, { LN_GPIO, 11 },
	{ LN_GPIO, 12 }, { LN_GPIO, 13 }, { LN_GPIO, 14 }, { LN_GPIO, 15 }, { LN_GPIO, 16 }, { LN_KEY, 0x11 },
};
static const char *const k_pb_name[PB_EXTRA] = { "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
	"L", "R", "L2", "R2", "START", "SELECT", "Menu" };
#endif
#define NLINES ((int)(sizeof k_lines / sizeof k_lines[0]))

/* the names a map file keeps the buttons under */
static const char *const k_pb_tok[PB_EXTRA] = { "up", "down", "left", "right", "a", "b", "x", "y",
	"l", "r", "l2", "r2", "start", "select", "menu" };

#ifdef PICO_INPUT_TEST
extern unsigned int pico_input_test_lines(void);	/* the test's buttons, a PB_ mask */
extern unsigned int pico_input_test_ms(void);
static unsigned int read_lines(void) { return pico_input_test_lines(); }
static unsigned int ms_now(void) { return pico_input_test_ms(); }
void pico_input_init(void) { }
#else
extern int mt_get_gpio_in(unsigned int pin);
extern int mt_set_gpio_mode(unsigned int pin, unsigned int mode);
extern int mt_set_gpio_dir(unsigned int pin, unsigned int dir);
extern int mt_set_gpio_smt(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_enable(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_select(unsigned int pin, unsigned int select);
extern int mtk_detect_key(unsigned short key);
extern unsigned int gpt4_get_current_tick(void);
static unsigned int ms_now(void) { return gpt4_get_current_tick() / 13000u; }

static int s_inited;
/* every line as every button pin is set: GPIO mode, input, Schmitt trigger, pull up (a line left
 * as it comes out of reset floats, and its noise would read as presses) */
void pico_input_init(void)
{
	int i;
	if (s_inited) return;
	for (i = 0; i < NLINES; i++) {
		unsigned int p = k_lines[i].n | 0x80000000u;
		if (k_lines[i].kind != LN_GPIO) continue;
		mt_set_gpio_mode(p, 0);
		mt_set_gpio_dir(p, 0);
		mt_set_gpio_smt(p, 1);
		mt_set_gpio_pull_enable(p, 1);
		mt_set_gpio_pull_select(p, 1);
	}
	{	/* the lines rise through a debounce capacitor each (over 1 ms on the Block): a read
		 * before then would see every button pressed */
		unsigned int t0 = gpt4_get_current_tick();
		while (gpt4_get_current_tick() - t0 < 13000u * 5u) ;
	}
	s_inited = 1;
}
/* the keypad's keys go through the PMIC (its home key line) and log each read while held: read at
 * most every 8 ms, however often the frame asks (the GPIOs are a register each, read every time) */
static unsigned int s_keys, s_keys_tick, s_keys_read;
static unsigned int read_lines(void)
{
	unsigned int m = 0, now = gpt4_get_current_tick();
	int i, fresh;
	if (!s_inited) pico_input_init();
	fresh = !s_keys_read || now - s_keys_tick >= 13000u * 8u;
	if (fresh) { s_keys = 0; s_keys_tick = now; s_keys_read = 1; }
	for (i = 0; i < NLINES; i++) {
		if (k_lines[i].kind == LN_GPIO) {
			if (mt_get_gpio_in(k_lines[i].n | 0x80000000u) == 0) m |= 1u << i;
		} else if (fresh && mtk_detect_key(k_lines[i].n)) s_keys |= 1u << i;
	}
	m |= s_keys;
	{	/* a board with a stick: it plays the d-pad (up 1, down 2, left 4, right 8; none on the Konkrs) */
		extern unsigned int ayaneo_joypad_dpad(void);
		unsigned int d = ayaneo_joypad_dpad();
		if (d & 1u) m |= 1u << PB_UP;
		if (d & 2u) m |= 1u << PB_DOWN;
		if (d & 4u) m |= 1u << PB_LEFT;
		if (d & 8u) m |= 1u << PB_RIGHT;
	}
	return m;
}
#endif

/* oem input: buttons pressed for a while, as if held */
static unsigned int s_fake, s_fake_until;
/* a spare line that reads pressed from the first read on is driven by something else, not a
 * button: it is left out until it ever reads let go */
static unsigned int s_stuck, s_stuck_known;

unsigned int pico_input_raw(void)
{
	unsigned int m = read_lines(), now = ms_now();
	if (!s_stuck_known) { s_stuck = m & ~((1u << PB_EXTRA) - 1u); s_stuck_known = 1; }
	s_stuck &= m;
	m &= ~s_stuck;
	if (s_fake_until && (int)(s_fake_until - now) > 0) m |= s_fake;
	else s_fake_until = 0;
	return m;
}

int pico_input_buttons(void) { return NLINES; }

const char *pico_input_button_name(int b, char *buf)
{
	/* the caps as printed on the console (A, L1, START...); the d-pad's directions and Menu in the
	 * language in use */
	if (b >= 0 && b < PB_EXTRA) return b <= PB_RIGHT || b == PB_MENU ? trw(k_pb_name[b]) : k_pb_name[b];
	snprintf(buf, 24, trw("Button %d"), b - PB_EXTRA + 1);
	return buf;
}

/* ---- the jobs ---- */

enum { J_IN, J_TURBO, J_HOT };
struct pi_job {
	const char *tok;	/* its name in a map file */
	const char *name;	/* on the page (English; the menu translates) */
	unsigned char kind;
	unsigned char out;	/* J_IN, J_TURBO: the PJ_ bit; J_HOT: the PH_ */
	unsigned char def;	/* the button it has unless the map says otherwise (NB none) */
};
#define NB 0xFF
#define MAXJOBS 32

#define DPAD { "up", "Up", J_IN, PJ_UP, PB_UP }, { "down", "Down", J_IN, PJ_DOWN, PB_DOWN }, \
	{ "left", "Left", J_IN, PJ_LEFT, PB_LEFT }, { "right", "Right", J_IN, PJ_RIGHT, PB_RIGHT }
#define HOTKEYS { "ff", "Fast forward", J_HOT, PH_FF, PB_R2 }, \
	{ "ff_hold", "Fast forward (hold)", J_HOT, PH_FF_HOLD, NB }, \
	{ "rewind", "Rewind", J_HOT, PH_RW, NB }, \
	{ "rewind_hold", "Rewind (hold)", J_HOT, PH_RW_HOLD, PB_L2 }, \
	{ "save", "Save state", J_HOT, PH_SAVE, NB }, { "load", "Load state", J_HOT, PH_LOAD, NB }, \
	{ "restart", "Reset Game", J_HOT, PH_RESTART, NB }, { "menu", "Open the menu", J_HOT, PH_MENU, NB }

/* the GBA: X fires A, Y fires B, as the turbo buttons always did */
static const struct pi_job k_gba[] = {
	{ "a", "A", J_IN, PJ_A, PB_A }, { "b", "B", J_IN, PJ_B, PB_B },
	{ "l", "L", J_IN, PJ_L, PB_L }, { "r", "R", J_IN, PJ_R, PB_R },
	{ "start", "START", J_IN, PJ_START, PB_START }, { "select", "SELECT", J_IN, PJ_SELECT, PB_SELECT },
	DPAD,
	{ "turbo_a", "Turbo A", J_TURBO, PJ_A, PB_X }, { "turbo_b", "Turbo B", J_TURBO, PJ_B, PB_Y },
	HOTKEYS,
};
static const struct pi_job k_gb[] = {
	{ "a", "A", J_IN, PJ_A, PB_A }, { "b", "B", J_IN, PJ_B, PB_B },
	{ "start", "START", J_IN, PJ_START, PB_START }, { "select", "SELECT", J_IN, PJ_SELECT, PB_SELECT },
	DPAD,
	{ "turbo_a", "Turbo A", J_TURBO, PJ_A, PB_X }, { "turbo_b", "Turbo B", J_TURBO, PJ_B, PB_Y },
	HOTKEYS,
};
/* armNES fires its turbo buttons itself (its X and Y); L and R are the Zapper's away and trigger */
static const struct pi_job k_nes[] = {
	{ "a", "A", J_IN, PJ_A, PB_A }, { "b", "B", J_IN, PJ_B, PB_B },
	{ "start", "START", J_IN, PJ_START, PB_START }, { "select", "SELECT", J_IN, PJ_SELECT, PB_SELECT },
	DPAD,
	{ "turbo_a", "Turbo A", J_IN, PJ_X, PB_X }, { "turbo_b", "Turbo B", J_IN, PJ_Y, PB_Y },
	{ "l", "Zapper away (L)", J_IN, PJ_L, PB_L }, { "r", "Zapper trigger (R)", J_IN, PJ_R, PB_R },
	HOTKEYS,
	{ "special", "Swap Disk Side", J_HOT, PH_SPECIAL, NB },
};
static const struct pi_job k_snes[] = {
	{ "a", "A", J_IN, PJ_A, PB_A }, { "b", "B", J_IN, PJ_B, PB_B },
	{ "x", "X", J_IN, PJ_X, PB_X }, { "y", "Y", J_IN, PJ_Y, PB_Y },
	{ "l", "L", J_IN, PJ_L, PB_L }, { "r", "R", J_IN, PJ_R, PB_R },
	{ "start", "START", J_IN, PJ_START, PB_START }, { "select", "SELECT", J_IN, PJ_SELECT, PB_SELECT },
	DPAD,
	{ "turbo_a", "Turbo A", J_TURBO, PJ_A, NB }, { "turbo_b", "Turbo B", J_TURBO, PJ_B, NB },
	HOTKEYS,
};
/* Genesis Plus GX: the pad's B is libretro's B, C its A, A its Y; the six button pad's X is L, Y
 * is X, Z is R, Mode is Select */
static const struct pi_job k_md[] = {
	{ "a", "A", J_IN, PJ_Y, PB_Y }, { "b", "B", J_IN, PJ_B, PB_B }, { "c", "C", J_IN, PJ_A, PB_A },
	{ "x", "X", J_IN, PJ_L, PB_L }, { "y", "Y", J_IN, PJ_X, PB_X }, { "z", "Z", J_IN, PJ_R, PB_R },
	{ "start", "START", J_IN, PJ_START, PB_START }, { "mode", "MODE", J_IN, PJ_SELECT, PB_SELECT },
	DPAD,
	HOTKEYS,
};
/* the Master System, Game Gear and SG-1000: button 1 is libretro's B, button 2 its A */
static const struct pi_job k_sms[] = {
	{ "1", "1", J_IN, PJ_B, PB_B }, { "2", "2", J_IN, PJ_A, PB_A },
	{ "start", "START", J_IN, PJ_START, PB_START },
	DPAD,
	HOTKEYS,
};

/* ---- the map in force ---- */

static struct {
	int live;			/* a game is running */
	int system;
	const struct pi_job *job;
	int nj;
	unsigned int map[MAXJOBS];	/* each job's buttons (a mask) */
	int own;			/* the game has its own map */
	fat_vol *v;
	char game[160];
	char sys_tok[8];
	const struct igm_core *igm;
	int special;			/* the core's item the Special hotkey runs, -1 none */
	int netplay;
	unsigned int hold;		/* buttons kept out of the game until released */
	unsigned int hot_was;		/* the hotkeys that have fired and are still held (PH_ bits) */
	unsigned char hot_n[PH_COUNT];	/* the frames each has been held in a row */
	unsigned int note_until;	/* a hotkey's status line is awaited until then */
	int note_job;			/* (its hotkey) */
	int mapped;			/* the map holds something (the defaults before any game) */
} S = { .job = k_gba, .nj = (int)(sizeof k_gba / sizeof k_gba[0]), .special = -1 };

static const char *sys_tok(int system)
{
	switch (system) {
	case GBA_CONSOLE_GB: return "gb";
	case GBA_CONSOLE_GBC: return "gbc";
	case GBA_CONSOLE_GBA: return "gba";
	case GBA_CONSOLE_SNES: return "snes";
	case GBA_CONSOLE_GENESIS: return "md";
	case GBA_CONSOLE_SMS: return "sms";
	case GBA_CONSOLE_GG: return "gg";
	case GBA_CONSOLE_SG: return "sg";
	case GBA_CONSOLE_NES: return "nes";
	default: return 0;
	}
}
static void jobs_of(int system, const struct pi_job **job, int *nj)
{
#define USE(t) do { *job = t; *nj = (int)(sizeof t / sizeof t[0]); } while (0)
	switch (system) {
	case GBA_CONSOLE_GB: case GBA_CONSOLE_GBC: USE(k_gb); break;
	case GBA_CONSOLE_SNES: USE(k_snes); break;
	case GBA_CONSOLE_GENESIS: USE(k_md); break;
	case GBA_CONSOLE_SMS: case GBA_CONSOLE_GG: case GBA_CONSOLE_SG: USE(k_sms); break;
	case GBA_CONSOLE_NES: USE(k_nes); break;
	default: USE(k_gba); break;
	}
#undef USE
}

static void map_defaults(void)
{
	int i;
	S.mapped = 1;
	for (i = 0; i < S.nj; i++) S.map[i] = S.job[i].def == NB ? 0u : 1u << S.job[i].def;
}

/* a button's name in a file: "a", "l2", the spare ones "ex1".. (out holds 12) */
static void tok_of(int b, char *out)
{
	if (b < PB_EXTRA) snprintf(out, 12, "%s", k_pb_tok[b]);
	else snprintf(out, 12, "ex%u", (unsigned int)(b - PB_EXTRA + 1) % 100u);
}
static int button_of(const char *t, int n)
{
	int i;
	for (i = 0; i < PB_EXTRA; i++)
		if ((int)strlen(k_pb_tok[i]) == n && !strncmp(k_pb_tok[i], t, (size_t)n)) return i == PB_MENU ? -1 : i;
	if (n >= 3 && t[0] == 'e' && t[1] == 'x') {
		int k = 0;
		for (i = 2; i < n; i++) { if (t[i] < '0' || t[i] > '9') return -1; k = k * 10 + (t[i] - '0'); }
		if (k >= 1 && PB_EXTRA + k - 1 < NLINES) return PB_EXTRA + k - 1;
	}
	return -1;
}

/* a map file's text read into the map: "map" first, then "<job>=<button>[+<button>..]" or "<job>=-";
 * what it does not name stays as it was. 0 when it is not a map (a game's file put back to none). */
/* The map file's form: "map" (GammaOS Pico 1.1.1, where L2 toggled rewind by default), "map 2"
 * (since 1.1.2: L2 rewinds while held). A file records every job, the defaults too, so a 1.1.1 map
 * whose rewind is still the toggle on L2 alone, with no button held for it, had only kept the old
 * default: it takes the new one. One whose player moved either keeps it. */
#define MAP_FORM 2
static int job_index(int kind, int out);
static void map_old_rewind(void)
{
	int t = job_index(J_HOT, PH_RW), h = job_index(J_HOT, PH_RW_HOLD);
	if (t >= 0 && h >= 0 && S.map[t] == (1u << PB_L2) && !S.map[h]) {
		S.map[t] = 0;
		S.map[h] = 1u << PB_L2;
	}
}

static int map_parse(char *t, unsigned int n)
{
	char *p = t, *e, *end = t + n;
	int first = 1, form = 1;
	while (p < end) {
		char *eq;
		int i, nl;
		for (e = p; e < end && *e != '\n'; e++) ;
		nl = (int)(e - p);
		if (nl && p[nl - 1] == '\r') nl--;
		if (first) {
			if (nl < 3 || memcmp(p, "map", 3) || (nl > 3 && p[3] != ' ')) return 0;
			if (nl > 4) form = p[4] - '0';
			first = 0;
			p = e + 1;
			continue;
		}
		eq = memchr(p, '=', (size_t)nl);
		if (eq) {
			int kl = (int)(eq - p);
			for (i = 0; i < S.nj; i++)
				if ((int)strlen(S.job[i].tok) == kl && !strncmp(S.job[i].tok, p, (size_t)kl)) break;
			if (i < S.nj) {
				char *v = eq + 1, *ve = p + nl;
				unsigned int m = 0;
				int cnt = 0, bad = 0;
				if (!(ve - v == 1 && *v == '-')) {
					while (v < ve) {
						char *q = v;
						int b;
						while (q < ve && *q != '+') q++;
						b = button_of(v, (int)(q - v));
						if (b < 0) bad = 1;
						else { m |= 1u << b; cnt++; }
						v = q + 1;
					}
				}
				/* the game's buttons take one, a hotkey up to three */
				if (!bad && cnt <= (S.job[i].kind == J_HOT ? 3 : 1)) S.map[i] = m;
			}
		}
		p = e + 1;
	}
	if (!first && form < 2) map_old_rewind();
	return !first;
}

static int map_read(const char *dir, const char *name)
{
	char buf[1024];
	unsigned int n;
	if (!S.v) return 0;
	n = gba_sd_read_named(S.v, dir, name, "map", (unsigned char *)buf, sizeof buf - 1);
	if (!n) return 0;
	buf[n] = 0;
	return map_parse(buf, n);
}

/* the map as a file (cap bytes at least 1024) */
static unsigned int map_text(char *out, unsigned int cap)
{
	unsigned int len = 0;
	int i, b;
	len += (unsigned int)snprintf(out + len, cap - len, "map %d\n", MAP_FORM);
	for (i = 0; i < S.nj && len + 64 < cap; i++) {
		int any = 0;
		len += (unsigned int)snprintf(out + len, cap - len, "%s=", S.job[i].tok);
		for (b = 0; b < NLINES; b++) {
			char t[12];
			if (!(S.map[i] & (1u << b))) continue;
			tok_of(b, t);
			len += (unsigned int)snprintf(out + len, cap - len, "%s%s", any ? "+" : "", t);
			any = 1;
		}
		if (!any) out[len++] = '-';
		out[len++] = '\n';
	}
	return len;
}

static void map_save(void)
{
	static char buf[1024];	/* the writer copies it at once */
	char dir[48];
	unsigned int len;
	if (!S.v || !S.live) return;
	len = map_text(buf, sizeof buf);
	if (S.own) {
		snprintf(dir, sizeof dir, CTL_DIR "/%s", S.sys_tok);
		gba_sd_write_named_bg(S.v, dir, S.game, "map", buf, len);
	} else gba_sd_write_named_bg(S.v, CTL_DIR, S.sys_tok, "map", buf, len);
}

/* the game's own map dropped: its file says so ("-"), and the system's is read again */
static void own_drop(void)
{
	char dir[48];
	snprintf(dir, sizeof dir, CTL_DIR "/%s", S.sys_tok);
	if (S.v) gba_sd_write_named_bg(S.v, dir, S.game, "map", "-\n", 2);
	S.own = 0;
	map_defaults();
	map_read(CTL_DIR, S.sys_tok);
}

static struct {
	int on;			/* listening */
	int row;		/* the row it listens for */
	int job;
	int state;		/* LS_* */
	unsigned int got;	/* the buttons pressed so far (a hotkey's) */
	unsigned int t0;
	unsigned int ignore;	/* lines held all along */
} Lst;
enum { LS_RELEASE, LS_LISTEN, LS_AFTER };

void pico_input_game(fat_vol *v, int system, const char *game, const struct igm_core *igm, int special_item)
{
	char dir[48];
	const char *st = sys_tok(system);
	pico_input_game_end();
	jobs_of(system, &S.job, &S.nj);
	S.system = system;
	S.v = v;
	S.igm = igm;
	S.special = special_item;
	S.netplay = 0;
	snprintf(S.game, sizeof S.game, "%s", game ? game : "");
	snprintf(S.sys_tok, sizeof S.sys_tok, "%s", st ? st : "gba");
	map_defaults();
	S.own = 0;
	if (st && v) {
		map_read(CTL_DIR, st);
		snprintf(dir, sizeof dir, CTL_DIR "/%s", st);
		if (game && game[0]) {
			unsigned int keep[MAXJOBS];
			memcpy(keep, S.map, sizeof keep);
			if (map_read(dir, game)) S.own = 1;
			else memcpy(S.map, keep, sizeof keep);
		}
	}
	S.live = 1;
	S.hold = pico_input_raw();	/* what started the game (A on its name) stays out of it until let go */
	S.hot_was = ~0u;		/* a hotkey held at the start runs only once it is pressed again */
	memset(S.hot_n, 0, sizeof S.hot_n);
}

void pico_input_game_end(void)
{
	S.live = 0;
	S.igm = 0;
	S.special = -1;
	S.note_until = 0;
	Lst.on = 0;
	/* the menus and the next game's start keep the buttons as they always were */
	S.job = k_gba; S.nj = (int)(sizeof k_gba / sizeof k_gba[0]);
	map_defaults();
}

/* netplay: the game's buttons go on, hotkeys that would change one side only do not */
void pico_input_netplay(int on) { S.netplay = on; }

/* ---- the pad ---- */

static int job_index(int kind, int out)
{
	int i;
	for (i = 0; i < S.nj; i++) if (S.job[i].kind == kind && S.job[i].out == out) return i;
	return -1;
}

int pico_input_hot_down(int hk)
{
	int i;
	unsigned int m;
	if (!S.mapped) map_defaults();
	i = job_index(J_HOT, hk);
	if (i < 0 || !S.map[i]) return 0;
	if (Lst.on && S.live) return 0;		/* the Controls page is listening */
	m = pico_input_raw();
	return (m & S.map[i]) == S.map[i];
}

/* the turbo's beat: on for half of each twelfth of a second, the rate the GBA's always had */
static int turbo_beat(void) { return (ms_now() * 24u / 1000u) & 1u; }

unsigned int pico_input_pad(void)
{
	unsigned int raw = pico_input_raw(), out = 0, hot = 0;
	int i, beat = -1;
	if (!S.mapped) map_defaults();
	S.hold &= raw;
	if (Lst.on) return 0;
	for (i = 0; i < S.nj; i++)	/* a hotkey's buttons, all held: not the game's */
		if (S.job[i].kind == J_HOT && S.map[i] && (raw & S.map[i]) == S.map[i]) hot |= S.map[i];
	raw &= ~(S.hold | hot);
	for (i = 0; i < S.nj; i++) {
		if (!(raw & S.map[i])) continue;
		if (S.job[i].kind == J_IN) out |= 1u << S.job[i].out;
		else if (S.job[i].kind == J_TURBO) {
			if (beat < 0) beat = turbo_beat();
			if (beat) out |= 1u << S.job[i].out;
		}
	}
	return out;
}

unsigned int pico_input_gba_keys(void)
{
	unsigned int p = pico_input_pad(), k = 0;
	if (p & (1u << PJ_A)) k |= 0x001u;
	if (p & (1u << PJ_B)) k |= 0x002u;
	if (p & (1u << PJ_SELECT)) k |= 0x004u;
	if (p & (1u << PJ_START)) k |= 0x008u;
	if (p & (1u << PJ_RIGHT)) k |= 0x010u;
	if (p & (1u << PJ_LEFT)) k |= 0x020u;
	if (p & (1u << PJ_UP)) k |= 0x040u;
	if (p & (1u << PJ_DOWN)) k |= 0x080u;
	if (p & (1u << PJ_R)) k |= 0x100u;
	if (p & (1u << PJ_L)) k |= 0x200u;
	return k;
}
unsigned int pico_input_gb_keys(void) { return pico_input_gba_keys() & 0xFFu; }

void pico_input_hold_back(void)
{
	S.hold = pico_input_raw();
	Lst.on = 0;
}

int pico_input_taken(int b)
{
	int i;
	unsigned int bit = 1u << b;
	for (i = 0; i < S.nj; i++)
		if ((S.map[i] & bit) && !(S.job[i].def != NB && S.job[i].def == b)) return 1;
	return 0;
}

/* a key of the keypad (mtk_detect_key's code) is given a job: the volume keys leave it alone */
int pico_input_key_taken(int code)
{
	int i;
	for (i = 0; i < NLINES; i++)
		if (k_lines[i].kind == LN_KEY && k_lines[i].n == code) return pico_input_taken(i);
	return 0;
}

/* ---- the hotkeys ---- */

extern volatile int g_dbg_igm_toggle;	/* a tap of the menu key (every runner takes it) */
extern void pico_ra_toast(const char *head, const char *line, int tone) __attribute__((weak));
/* a status line's tone: the cores say "failed", "off", "no ..." when it did not happen */
static int note_tone(const char *t)
{
	const char *p;
	for (p = t; *p; p++) {
		if (!strncmp(p, "fail", 4) || !strncmp(p, "Fail", 4) || !strncmp(p, " off", 4) || !strncmp(p, "No ", 3) ||
		    !strncmp(p, "not ", 4) || !strncmp(p, "empty", 5) || !strncmp(p, "Empty", 5)) return -1;
	}
	return 1;
}

static void hot_run(int hk)
{
	const struct igm_core *c = S.igm;
	int item = -1;
	if (hk == PH_MENU) { g_dbg_igm_toggle = 1; return; }
	if (!c || !c->change || S.netplay) return;
	if (hk == PH_SAVE) item = c->save_item;
	else if (hk == PH_LOAD) item = c->load_item;
	else if (hk == PH_RESTART) item = c->reset_item;
	else if (hk == PH_SPECIAL) item = S.special;
	if (item < 0) return;
	if (c->status) c->status[0] = 0;
	S.note_job = job_index(J_HOT, hk);
	c->change(item, 0, 1);
	/* what it says (a state is written from the card's thread: "State saved" comes a moment later) */
	S.note_until = (ms_now() + 4000u) | 1u;
}

void pico_input_frame(int menu_open)
{
	unsigned int raw, now = 0;
	int i;
	if (!S.live) return;
	if (S.note_until) {
		const struct igm_core *c = S.igm;
		if (c && c->status && c->status[0]) {
			if (pico_ra_toast) pico_ra_toast(S.note_job >= 0 ? S.job[S.note_job].name : "", c->status, note_tone(c->status));
			c->status[0] = 0;
			S.note_until = 0;
		} else if ((int)(ms_now() - S.note_until) > 0) S.note_until = 0;
	}
	if (Lst.on) { S.hot_was = ~0u; memset(S.hot_n, 0, sizeof S.hot_n); return; }
	raw = pico_input_raw();
	for (i = 0; i < S.nj; i++)
		if (S.job[i].kind == J_HOT && S.map[i] && (raw & S.map[i]) == S.map[i]) now |= 1u << S.job[i].out;
	/* a hotkey runs once its buttons have been held three frames in a row (a line's glitch never
	 * loads a state), then not again until they are let go */
	for (i = 0; i < PH_COUNT; i++) {
		if (!(now & (1u << i))) { S.hot_n[i] = 0; S.hot_was &= ~(1u << i); continue; }
		if (S.hot_n[i] < 255) S.hot_n[i]++;
		if (S.hot_n[i] < 3 || (S.hot_was & (1u << i))) continue;
		S.hot_was |= 1u << i;
		if (i == PH_FF || i == PH_FF_HOLD || i == PH_RW || i == PH_RW_HOLD) continue;	/* (read where they act) */
		if (menu_open && i != PH_MENU) continue;
		hot_run(i);
	}
}

/* ---- the Controls page ---- */

/* rows: Buttons for, the game's buttons' heading and rows, the hotkeys' heading and rows, Reset */
enum { R_SCOPE, R_HEAD_GAME };
static int nin(void) { int i, n = 0; for (i = 0; i < S.nj; i++) if (S.job[i].kind != J_HOT) n++; return n; }
static int njobs_shown(void)
{
	int i, n = 0;
	for (i = 0; i < S.nj; i++) if (S.job[i].kind != J_HOT || S.job[i].out != PH_SPECIAL || S.special >= 0) n++;
	return n;
}
/* the job a row is, -1 for the other rows */
static int row_job(int r)
{
	int i, k, ni = nin();
	if (r < 2) return -1;
	r -= 2;
	if (r < ni) {
		for (i = 0, k = 0; i < S.nj; i++) if (S.job[i].kind != J_HOT && k++ == r) return i;
		return -1;
	}
	r -= ni + 1;	/* the hotkeys' heading */
	if (r < 0) return -1;
	for (i = 0, k = 0; i < S.nj; i++) {
		if (S.job[i].kind != J_HOT) continue;
		if (S.job[i].out == PH_SPECIAL && S.special < 0) continue;
		if (k++ == r) return i;
	}
	return -1;
}
static int row_reset(void) { return 2 + njobs_shown() + 1; }

int pico_input_page(void) { return S.live; }
void pico_input_page_open(void) { Lst.on = 0; }
int pico_input_rows(void) { return S.live ? row_reset() + 1 : 0; }

int pico_input_row_head(int i) { return i == R_HEAD_GAME || i == 2 + nin(); }

const char *pico_input_row_title(int i)
{
	int j;
	if (i == R_SCOPE) return "Buttons for";
	if (i == R_HEAD_GAME) return "Game buttons";
	if (i == 2 + nin()) return "Hotkeys";
	if (i == row_reset()) return "Reset to default";
	j = row_job(i);
	return j >= 0 ? S.job[j].name : "";
}

static char s_val[80];
const char *pico_input_row_value(int i)
{
	int j, b, any = 0;
	unsigned int len = 0;
	if (i == R_SCOPE) return trw(S.own ? "This game" : "All games");
	j = row_job(i);
	if (j < 0) return 0;
	if (Lst.on && Lst.job == j)
		return trw(Lst.state == LS_RELEASE ? "Let go of the buttons" : S.job[j].kind == J_HOT ? "Press up to 3 buttons" : "Press a button");
	for (b = 0; b < NLINES; b++) {
		char nb[24];
		if (!(S.map[j] & (1u << b))) continue;
		len += (unsigned int)snprintf(s_val + len, sizeof s_val - len, "%s%s", any ? " + " : "",
					      pico_input_button_name(b, nb));
		any = 1;
		if (len >= sizeof s_val - 1) break;
	}
	return any ? s_val : trw("None");
}

int pico_input_row_change(int i, int dir)
{
	int j;
	if (!S.live) return 0;
	if (i == R_SCOPE) {
		if (S.own) own_drop();
		else { S.own = 1; map_save(); }	/* the game's own, the system's to start with */
		return 1;
	}
	if (i == row_reset()) {
		if (dir) return 0;
		map_defaults();
		map_save();
		return 1;
	}
	j = row_job(i);
	if (j < 0) return 0;
	if (dir) {	/* Left / Right: none, then each button in turn (the menu key never) */
		int b = -1, k;
		for (k = 0; k < NLINES; k++) if (S.map[j] == (1u << k)) { b = k; break; }
		if (S.map[j] && b < 0) b = -1;	/* a hotkey's buttons together: from none */
		for (k = 0; k < NLINES + 1; k++) {
			b += dir;
			if (b < -1) b = NLINES - 1;
			if (b >= NLINES) b = -1;
			if (b != PB_MENU) break;
		}
		S.map[j] = b < 0 ? 0u : 1u << b;
		map_save();
		return 1;
	}
	Lst.on = 1;
	Lst.row = i;
	Lst.job = j;
	Lst.state = LS_RELEASE;
	Lst.got = 0;
	Lst.t0 = ms_now();
	Lst.ignore = 0;
	return 1;
}

int pico_input_busy(void) { return Lst.on; }

void pico_input_listen(void)
{
	unsigned int raw = pico_input_raw(), now = ms_now();
	if (!Lst.on) return;
	raw &= ~(1u << PB_MENU);
	switch (Lst.state) {
	case LS_RELEASE:	/* the A that chose the row let go first; a line held 2 s is left out */
		if (!(raw & ~Lst.ignore)) { Lst.state = LS_LISTEN; Lst.t0 = now; Lst.ignore &= raw; }
		else if (now - Lst.t0 > 2000u) Lst.ignore = raw;
		break;
	case LS_LISTEN: {
		unsigned int m = raw & ~Lst.ignore;
		if (S.job[Lst.job].kind != J_HOT) {
			if (m) {
				int b = 0;
				while (!(m & (1u << b))) b++;
				S.map[Lst.job] = 1u << b;
				map_save();
				Lst.state = LS_AFTER;
			} else if (now - Lst.t0 > 8000u) Lst.on = 0;
			break;
		}
		/* a hotkey: every button pressed until they are all let go, three at most */
		if (m) {
			unsigned int g = Lst.got | m;
			int c = 0, b;
			for (b = 0; b < NLINES; b++) if (g & (1u << b)) c++;
			if (c <= 3) Lst.got = g;
		} else if (Lst.got) {
			S.map[Lst.job] = Lst.got;
			map_save();
			Lst.on = 0;
		} else if (now - Lst.t0 > 8000u) Lst.on = 0;
		break;
	}
	case LS_AFTER:		/* the button given let go before the menu takes keys again */
		if (!(raw & ~Lst.ignore)) Lst.on = 0;
		break;
	}
	if (!Lst.on) S.hold = pico_input_raw();
}

/* ---- oem input ---- */

void pico_input_diag(char *out, int cap)
{
	char t[1024];
	unsigned int n, i;
	int len = snprintf(out, (size_t)cap, "input: %s live %d own %d raw %06x pad %04x hold %06x listen %d/%d stuck %06x | ",
			   S.sys_tok[0] ? S.sys_tok : "-", S.live, S.own, pico_input_raw(), pico_input_pad(), S.hold,
			   Lst.on, Lst.state, s_stuck);
	n = map_text(t, sizeof t);
	for (i = 0; i < n && len < cap - 1; i++) out[len++] = t[i] == '\n' ? ' ' : t[i];
	out[len < cap ? len : cap - 1] = 0;
}

/* oem input:<hex PB_ mask>:<ms> presses those buttons for that long; oem input:toast shows a
 * hotkey's note */
void pico_input_debug(const char *arg)
{
	unsigned int m = 0, ms = 0;
	const char *p = arg;
	if (!strncmp(arg, "toast", 5)) {	/* a hotkey's note, as a save would show it */
		if (pico_ra_toast) pico_ra_toast("Save state", "State saved", 1);
		return;
	}
	while (*p && *p != ':') {
		char c = *p++;
		m = m * 16u + (unsigned int)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
	}
	if (*p == ':') for (p++; *p >= '0' && *p <= '9'; p++) ms = ms * 10u + (unsigned int)(*p - '0');
	if (!ms) ms = 200;
	s_fake = m;
	s_fake_until = (ms_now() + ms) | 1u;
}
