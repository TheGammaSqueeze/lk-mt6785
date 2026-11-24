/*
 * pico_input_test.c - pico_input.c on the PC.
 *
 *   pico_input_test.sh
 *
 * The buttons are a mask the test sets, the clock a counter it moves, the card a folder (the named
 * reads and writes stubbed over it). Checked: the buttons as they always were with no map, a
 * remapped button and two jobs on one button, turbo's beat, a hotkey's buttons kept from the game,
 * the buttons that closed the menu held back, the Controls page listening for a button and for a
 * hotkey's three, Left / Right stepping past the menu key, the system's map and a game's own
 * written and read back, a game put back to the system's map, and hotkeys running the core's items.
 */
#include "pico_input.h"
#include "pico_igm.h"
#include "gba_sd_save.h"
#include "sd_fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *s_root;
static unsigned int s_lines, s_ms = 1000;
unsigned int pico_input_test_lines(void) { return s_lines; }
unsigned int pico_input_test_ms(void) { return s_ms; }
volatile int g_dbg_igm_toggle;
const char *tr(const char *en) { return en; }

static void named(char *p, int cap, const char *dir, const char *rom, const char *ext)
{
	char base[512];
	const char *dot = strrchr(rom, '.');
	int n = dot ? (int)(dot - rom) : (int)strlen(rom);
	snprintf(base, sizeof base, "%.*s", n, rom);
	snprintf(p, cap, "%s%s/%s.%s", s_root, dir, base, ext);
}
uint32_t gba_sd_read_named(fat_vol *v, const char *dir, const char *romname, const char *ext, unsigned char *dst, uint32_t cap)
{
	char p[1024];
	FILE *f;
	uint32_t n;
	(void)v;
	named(p, sizeof p, dir, romname, ext);
	if (!(f = fopen(p, "rb"))) return 0;
	n = (uint32_t)fread(dst, 1, cap, f);
	fclose(f);
	return n;
}
static int s_writes;
int gba_sd_write_named_bg(fat_vol *v, const char *dir, const char *romname, const char *ext, const void *buf, uint32_t len)
{
	char p[1024], cmd[1200];
	FILE *f;
	(void)v;
	snprintf(cmd, sizeof cmd, "mkdir -p '%s%s'", s_root, dir);
	if (system(cmd)) return -1;
	named(p, sizeof p, dir, romname, ext);
	if (!(f = fopen(p, "wb"))) return -1;
	fwrite(buf, 1, len, f);
	fclose(f);
	s_writes++;
	return 0;
}
static char s_last_note[160];
void pico_ra_toast(const char *head, const char *line, int tone)
{
	snprintf(s_last_note, sizeof s_last_note, "%s|%s|%d", head, line, tone);
}

/* a core's menu: its save, load, restart and a special item */
enum { IT_SLOT, IT_SAVE, IT_LOAD, IT_RESET, IT_EXIT, IT_DISK };
static char s_status[96];
static int s_ran[8];
static int change(int item, int dir, int act)
{
	(void)dir;
	if (act && item >= 0 && item < 8) s_ran[item]++;
	if (act && item == IT_SAVE) snprintf(s_status, sizeof s_status, "State saved");
	if (act && item == IT_LOAD) snprintf(s_status, sizeof s_status, "Load failed");
	return 0;
}
static struct igm_core s_igm = { .slot_item = IT_SLOT, .save_item = IT_SAVE, .load_item = IT_LOAD,
	.reset_item = IT_RESET, .exit_item = IT_EXIT, .change = change, .status = s_status };

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define B(x) (1u << (x))
#define J(x) (1u << (x))

static int row_of(const char *title)
{
	int i, n = pico_input_rows();
	for (i = 0; i < n; i++) if (!strcmp(pico_input_row_title(i), title)) return i;
	return -1;
}
/* frames pass with these buttons held */
static void frames(unsigned int lines, int n)
{
	int i;
	s_lines = lines;
	for (i = 0; i < n; i++) {
		s_ms += 16;
		if (pico_input_busy()) pico_input_listen();
		pico_input_frame(0);
	}
}
static char *slurp(const char *rel)
{
	static char buf[4096];
	char p[1024];
	FILE *f;
	size_t n;
	snprintf(p, sizeof p, "%s%s", s_root, rel);
	if (!(f = fopen(p, "rb"))) return 0;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

int main(int argc, char **argv)
{
	fat_vol vol;
	int r, r2, i;
	unsigned int beats = 0, seen = 0;
	char *t;
	s_root = argc > 1 ? argv[1] : "/tmp/pico_input_card";
	memset(&vol, 0, sizeof vol);

	/* before any game: R2 toggles fast forward, L2 rewinds while held (no rewind toggle) */
	s_lines = B(PB_R2);
	CHECK(pico_input_hot_down(PH_FF) && !pico_input_hot_down(PH_RW), "R2 is fast forward with no game");
	s_lines = B(PB_L2);
	CHECK(pico_input_hot_down(PH_RW_HOLD) && !pico_input_hot_down(PH_RW), "L2 is the rewind hold with no game");

	/* the GBA, no map on the card: as always */
	s_lines = 0;
	pico_input_game(&vol, GBA_CONSOLE_GBA, "My Game.gba", &s_igm, -1);
	s_lines = B(PB_A) | B(PB_L) | B(PB_UP);
	CHECK(pico_input_gba_keys() == (0x001u | 0x200u | 0x040u), "GBA keys %03x", pico_input_gba_keys());
	CHECK(pico_input_pad() == (J(PJ_A) | J(PJ_L) | J(PJ_UP)), "pad %04x", pico_input_pad());
	/* turbo: X fires A on a beat */
	s_lines = B(PB_X);
	for (i = 0; i < 40; i++) { s_ms += 5; if (pico_input_pad() & J(PJ_A)) beats++; else seen++; }
	CHECK(beats && seen, "turbo beats %u off %u", beats, seen);
	/* the buttons that started the game stay out until let go */
	s_lines = B(PB_A);
	pico_input_game(&vol, GBA_CONSOLE_GBA, "My Game.gba", &s_igm, -1);
	CHECK(pico_input_pad() == 0, "A held at the start reaches the game");
	s_lines = 0; pico_input_pad();
	s_lines = B(PB_A);
	CHECK(pico_input_pad() == J(PJ_A), "A pressed again does not reach the game");

	/* the Controls page */
	CHECK(pico_input_page() && pico_input_rows() > 10, "rows %d", pico_input_rows());
	CHECK(pico_input_row_head(1) && pico_input_row_head(row_of("Hotkeys")), "headings");
	r = row_of("A");
	CHECK(r > 1 && !strcmp(pico_input_row_value(r), "A"), "A row %d '%s'", r, r > 0 ? pico_input_row_value(r) : "");
	/* A on the row: listens; the A that chose it is let go, then the spare Button 1 is pressed */
	s_lines = B(PB_A);
	pico_input_row_change(r, 0);
	CHECK(pico_input_busy(), "not listening");
	frames(B(PB_A), 3);
	CHECK(!strcmp(pico_input_row_value(r), "Let go of the buttons"), "'%s'", pico_input_row_value(r));
	frames(0, 2);
	CHECK(!strcmp(pico_input_row_value(r), "Press a button"), "'%s'", pico_input_row_value(r));
	frames(B(PB_EXTRA), 2);
	CHECK(pico_input_busy(), "stopped listening before the button was let go");
	frames(0, 2);
	CHECK(!pico_input_busy(), "still listening");
	CHECK(!strcmp(pico_input_row_value(r), "Button 1"), "'%s'", pico_input_row_value(r));
	s_lines = B(PB_EXTRA);
	CHECK(pico_input_gba_keys() == 0x001u, "Button 1 is not A: %03x", pico_input_gba_keys());
	s_lines = B(PB_A);
	CHECK(pico_input_gba_keys() == 0, "A still plays A");
	/* B on Button 1 too: two jobs on one button */
	r2 = row_of("B");
	s_lines = 0; pico_input_row_change(r2, 0); frames(0, 2); frames(B(PB_EXTRA), 1); frames(0, 1);
	s_lines = B(PB_EXTRA);
	CHECK(pico_input_gba_keys() == 0x003u, "Button 1 for A and B: %03x", pico_input_gba_keys());
	/* the system's map on the card */
	t = slurp("/gammaos/controls/gba.map");
	CHECK(t && !strncmp(t, "map 2\n", 6) && strstr(t, "\na=ex1\n") && strstr(t, "\nb=ex1\n") && strstr(t, "\nff=r2\n"),
	      "gba.map: %s", t ? t : "(none)");

	/* Left / Right: from B's Button 1 back to None and on past the buttons, never the menu key */
	pico_input_row_change(r2, 1);
	for (i = 0; i < 40 && strcmp(pico_input_row_value(r2), "Menu"); i++) pico_input_row_change(r2, 1);
	CHECK(i == 40, "Left / Right reached the menu key");
	while (strcmp(pico_input_row_value(r2), "B")) pico_input_row_change(r2, -1);

	/* a hotkey of three: Select, R and Button 2 */
	r = row_of("Save state");
	s_lines = 0; pico_input_row_change(r, 0); frames(0, 2);
	frames(B(PB_SELECT), 2); frames(B(PB_SELECT) | B(PB_R), 2); frames(B(PB_SELECT) | B(PB_R) | B(PB_EXTRA + 1), 2);
	frames(B(PB_SELECT) | B(PB_R) | B(PB_EXTRA + 1) | B(PB_START), 2);	/* a fourth: not taken */
	frames(0, 2);
	CHECK(!pico_input_busy() && !strcmp(pico_input_row_value(r), "R + SELECT + Button 2"), "'%s'", pico_input_row_value(r));
	/* held together: the game sees none of them, the core saves once, and says so */
	memset(s_ran, 0, sizeof s_ran);
	frames(B(PB_SELECT), 1);
	CHECK(pico_input_gba_keys() == 0x004u, "Select alone is the game's");
	frames(B(PB_SELECT) | B(PB_R) | B(PB_EXTRA + 1), 3);
	CHECK(pico_input_gba_keys() == 0, "the hotkey's buttons reach the game: %03x", pico_input_gba_keys());
	CHECK(s_ran[IT_SAVE] == 1, "saved %d times", s_ran[IT_SAVE]);
	frames(0, 1);
	CHECK(!strcmp(s_last_note, "Save state|State saved|1"), "note '%s'", s_last_note);
	/* load on Button 2 alone (Left / Right): a refusal is red */
	r = row_of("Load state");
	while (strcmp(pico_input_row_value(r), "Button 2")) pico_input_row_change(r, 1);
	frames(B(PB_EXTRA + 1), 2); frames(0, 1);
	CHECK(s_ran[IT_LOAD] == 0, "two frames of a press (a glitch) loaded a state");
	frames(B(PB_EXTRA + 1), 3); frames(0, 1);
	CHECK(s_ran[IT_LOAD] == 1 && !strcmp(s_last_note, "Load state|Load failed|-1"), "load %d '%s'", s_ran[IT_LOAD], s_last_note);
	/* the menu hotkey taps the menu key */
	r = row_of("Open the menu");
	while (strcmp(pico_input_row_value(r), "Button 3")) pico_input_row_change(r, 1);
	g_dbg_igm_toggle = 0;
	frames(B(PB_EXTRA + 2), 3);
	CHECK(g_dbg_igm_toggle == 1, "no tap of the menu key");
	frames(0, 1);
	/* netplay: no states */
	pico_input_netplay(1);
	frames(B(PB_EXTRA + 1), 3); frames(0, 1);
	CHECK(s_ran[IT_LOAD] == 1, "a state loaded in netplay");
	pico_input_netplay(0);
	/* fast forward moved to Button 1 alone: R2 no longer */
	r = row_of("Fast forward");
	while (strcmp(pico_input_row_value(r), "Button 1")) pico_input_row_change(r, 1);
	s_lines = B(PB_R2);
	CHECK(!pico_input_hot_down(PH_FF), "R2 still fast forward");
	s_lines = B(PB_EXTRA);
	CHECK(pico_input_hot_down(PH_FF), "Button 1 not fast forward");
	CHECK(pico_input_taken(PB_EXTRA) && !pico_input_taken(PB_A + 0 * 0 + 4), "taken");

	/* the menu closes with A held: A stays out of the game until let go */
	s_lines = B(PB_B) | B(PB_UP);
	pico_input_hold_back();
	CHECK(pico_input_pad() == 0, "held back");
	s_lines = B(PB_B);
	CHECK(pico_input_pad() == 0, "B let go of Up: B still held back");
	s_lines = 0; pico_input_pad();
	s_lines = B(PB_B);
	CHECK(pico_input_gba_keys() == 0x002u, "after the release");
	s_lines = B(PB_EXTRA);
	CHECK(pico_input_gba_keys() == 0, "Button 1 is fast forward's: the game does not see it");

	/* This game: its own map, the system's to start with */
	r = row_of("Buttons for");
	CHECK(!strcmp(pico_input_row_value(r), "All games"), "'%s'", pico_input_row_value(r));
	pico_input_row_change(r, 1);
	CHECK(!strcmp(pico_input_row_value(r), "This game"), "'%s'", pico_input_row_value(r));
	r2 = row_of("B");
	while (strcmp(pico_input_row_value(r2), "Y")) pico_input_row_change(r2, 1);
	t = slurp("/gammaos/controls/gba/My Game.map");
	CHECK(t && strstr(t, "\nb=y\n") && strstr(t, "\na=ex1\n"), "game map: %s", t ? t : "(none)");
	t = slurp("/gammaos/controls/gba.map");
	CHECK(t && strstr(t, "\nb=b\n"), "the system's map changed: %s", t ? t : "(none)");
	/* another game: the system's; this one again: its own */
	pico_input_game(&vol, GBA_CONSOLE_GBA, "Other.gba", &s_igm, -1);
	s_lines = 0; pico_input_pad(); s_lines = B(PB_B);
	CHECK(pico_input_gba_keys() == 0x002u, "Other: %03x", pico_input_gba_keys());
	pico_input_game(&vol, GBA_CONSOLE_GBA, "My Game.gba", &s_igm, -1);
	s_lines = 0; pico_input_pad(); s_lines = B(PB_Y);
	CHECK(pico_input_gba_keys() == 0x002u, "My Game: Y not B (%03x)", pico_input_gba_keys());
	CHECK(!strcmp(pico_input_row_value(row_of("Buttons for")), "This game"), "scope after reload");
	/* back to All games: the system's map */
	pico_input_row_change(row_of("Buttons for"), 1);
	pico_input_game(&vol, GBA_CONSOLE_GBA, "My Game.gba", &s_igm, -1);
	s_lines = 0; pico_input_pad(); s_lines = B(PB_Y);
	CHECK(!(pico_input_gba_keys() & 0x002u), "still the game's own map");
	CHECK(!strcmp(pico_input_row_value(row_of("Buttons for")), "All games"), "scope");
	/* Reset to default */
	pico_input_row_change(row_of("Reset to default"), 0);
	s_lines = B(PB_A);
	CHECK(pico_input_gba_keys() == 0x001u, "reset: A %03x", pico_input_gba_keys());
	t = slurp("/gammaos/controls/gba.map");
	CHECK(t && strstr(t, "\na=a\n") && strstr(t, "\nsave=-\n"), "reset map: %s", t ? t : "(none)");

	/* the Mega Drive's names; the NES's special hotkey only for a disk */
	pico_input_game(&vol, GBA_CONSOLE_GENESIS, "Sonic.md", &s_igm, -1);
	s_lines = 0; pico_input_pad();
	s_lines = B(PB_Y) | B(PB_L);
	CHECK(pico_input_pad() == (J(PJ_Y) | J(PJ_L)), "MD pad %04x", pico_input_pad());
	CHECK(row_of("C") > 0 && row_of("MODE") > 0 && row_of("Turbo A") < 0, "MD rows");
	pico_input_game(&vol, GBA_CONSOLE_NES, "Mario.nes", &s_igm, -1);
	CHECK(row_of("Swap Disk Side") < 0, "a cartridge has the disk's hotkey");
	pico_input_game(&vol, GBA_CONSOLE_NES, "Zelda.fds", &s_igm, IT_DISK);
	r = row_of("Swap Disk Side");
	CHECK(r > 0 && row_of("Reset to default") == r + 1, "disk hotkey row %d", r);
	pico_input_row_change(r, 1);
	s_lines = 0; frames(0, 1);
	memset(s_ran, 0, sizeof s_ran);
	frames(B(PB_UP), 5); frames(0, 1);
	CHECK(s_ran[IT_DISK] == 1, "disk side not swapped");
	s_lines = B(PB_X);
	CHECK(pico_input_pad() == J(PJ_X), "NES X is armNES's turbo A: %04x", pico_input_pad());
	/* a broken file changes nothing */
	{
		FILE *f;
		char p[1024];
		snprintf(p, sizeof p, "%s/gammaos/controls/snes.map", s_root);
		f = fopen(p, "wb"); fputs("map\na=zz\nb=a+b\nx=\n=y\nl=r\n", f); fclose(f);
		pico_input_game(&vol, GBA_CONSOLE_SNES, "SMW.sfc", &s_igm, -1);
		s_lines = 0; pico_input_pad();
		s_lines = B(PB_A) | B(PB_B) | B(PB_R);
		CHECK(pico_input_pad() == (J(PJ_A) | J(PJ_B) | J(PJ_L) | J(PJ_R)), "SNES broken map %04x", pico_input_pad());
	}
	/* a 1.1.1 map: rewind still the toggle on L2 takes the hold; one the player moved keeps it */
	{
		FILE *f;
		char p[1024];
		snprintf(p, sizeof p, "%s/gammaos/controls/gbc.map", s_root);
		f = fopen(p, "wb"); fputs("map\na=a\nrewind=l2\nrewind_hold=-\nsave=select+ex2\n", f); fclose(f);
		pico_input_game(&vol, GBA_CONSOLE_GBC, "Zelda.gbc", &s_igm, -1);
		s_lines = B(PB_L2);
		CHECK(pico_input_hot_down(PH_RW_HOLD) && !pico_input_hot_down(PH_RW), "1.1.1 map: L2 not moved to the hold");
		f = fopen(p, "wb"); fputs("map\nrewind=l2+select\nrewind_hold=-\n", f); fclose(f);
		pico_input_game(&vol, GBA_CONSOLE_GBC, "Zelda.gbc", &s_igm, -1);
		s_lines = B(PB_L2) | B(PB_SELECT);
		CHECK(pico_input_hot_down(PH_RW) && !pico_input_hot_down(PH_RW_HOLD), "1.1.1 map: the player's rewind changed");
		f = fopen(p, "wb"); fputs("map 2\nrewind=l2\nrewind_hold=-\n", f); fclose(f);
		pico_input_game(&vol, GBA_CONSOLE_GBC, "Zelda.gbc", &s_igm, -1);
		s_lines = B(PB_L2);
		CHECK(pico_input_hot_down(PH_RW) && !pico_input_hot_down(PH_RW_HOLD), "map 2: the player's toggle changed");
	}
	pico_input_game_end();
	CHECK(!pico_input_page(), "page with no game");
	{
		char d[1400];
		pico_input_diag(d, sizeof d);
		CHECK(strstr(d, "input:") != 0, "diag");
	}
	printf(s_fail ? "%d failed\n" : "all passed\n", s_fail);
	return s_fail != 0;
}
