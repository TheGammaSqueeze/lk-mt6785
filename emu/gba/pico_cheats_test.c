/*
 * pico_cheats_test.c - pico_cheats.c on the PC, against the card's real cheat packs.
 *
 *   pico_cheats_test.sh <card dir>
 *
 * The card's files come from a folder (fat_open / fat_read / the named reads and writes stubbed
 * over it), the window is a buffer and the core a stand-in that keeps the codes it is given and
 * refuses one with "BAD" in it. Checked: a game found by its dump's CRC-32 and by its name, the
 * headings, the parts of a long list, the player's own .cht and its enable flags, a code the core
 * refuses, and the .on file written and read back at the next start.
 */
#include "pico_cheats.h"
#include "gba_sd_save.h"
#include "sd_fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const char *s_root;
static FILE *s_open;
static unsigned char s_arena[2u << 20];
void *ayaneo_cheats_arena(unsigned int *size) { *size = sizeof s_arena; return s_arena; }

int fat_open(fat_vol *v, const char *path, fat_file *f)
{
	char p[1024];
	(void)v; (void)f;
	snprintf(p, sizeof p, "%s%s", s_root, path);
	if (s_open) fclose(s_open);
	s_open = fopen(p, "rb");
	return s_open ? 0 : -1;
}
uint32_t fat_read(fat_file *f, uint32_t off, void *buf, uint32_t len)
{
	(void)f;
	if (!s_open || fseek(s_open, (long)off, SEEK_SET)) return 0;
	return (uint32_t)fread(buf, 1, len, s_open);
}
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
int gba_sd_write_named_bg(fat_vol *v, const char *dir, const char *romname, const char *ext, const void *buf, uint32_t len)
{
	char p[1024], d[1024];
	FILE *f;
	(void)v;
	snprintf(d, sizeof d, "mkdir -p '%s%s'", s_root, dir);
	if (system(d)) return -1;
	named(p, sizeof p, dir, romname, ext);
	if (!(f = fopen(p, "wb"))) return -1;
	fwrite(buf, 1, len, f);
	fclose(f);
	return 0;
}

unsigned int gpt4_get_current_tick(void) { return 0; }
#include <dirent.h>
static DIR *s_dir;
int fat_opendir(fat_vol *v, const char *path, fat_dir *d)
{
	char p[1024];
	(void)v; (void)d;
	snprintf(p, sizeof p, "%s%s", s_root, path);
	if (s_dir) closedir(s_dir);
	s_dir = opendir(p);
	return s_dir ? 0 : -1;
}
int fat_readdir(fat_dir *d, fat_dirent *e)
{
	struct dirent *x;
	(void)d;
	if (!s_dir || !(x = readdir(s_dir))) return -1;
	memset(e, 0, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", x->d_name);
	e->is_dir = x->d_type == DT_DIR;
	return 0;
}

/* the core */
static char s_codes[64][256];
static int s_ncodes, s_resets;
static void core_reset(void) { s_ncodes = 0; s_resets++; }
static int core_add(const char *code)
{
	if (strstr(code, "BAD")) return -1;
	if (s_ncodes < 64) snprintf(s_codes[s_ncodes++], 256, "%s", code);
	return 0;
}
static const struct pico_cheat_core s_core = { core_reset, core_add };

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); s_fail++; } } while (0)

static void dump(const char *what)
{
	char l[256];
	pico_cheats_diag(l, sizeof l);
	printf("%s: %s\n", what, l);
}
static int find_row(const char *title)
{
	int i, n = pico_cheats_rows();
	for (i = 0; i < n; i++) if (!strcmp(pico_cheats_row_title(i), title)) return i;
	return -1;
}

/* the chooser's first part (where None, the found list and the player's files are) */
static void first_part(void)
{
	while (pico_cheats_step_part(-1)) ;
}

/* a 4 byte tail that brings data's CRC-32 to want (CRC-32 is linear: the tail is solved for) */
static void forge_crc(unsigned char *data, unsigned len, uint32_t want)
{
	/* the register after the data, run backwards from want through 4 bytes */
	uint32_t table[256], rev[256], crc, reg;
	int i, k;
	for (i = 0; i < 256; i++) {
		uint32_t c = (uint32_t)i;
		for (k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		table[i] = c;
		rev[c >> 24] = (uint32_t)i;
	}
	crc = (uint32_t)crc32(0L, data, len - 4) ^ 0xFFFFFFFFu;
	reg = want ^ 0xFFFFFFFFu;
	for (i = 3; i >= 0; i--) {
		uint32_t idx = rev[reg >> 24];
		reg = ((reg ^ table[idx]) << 8) | idx;
	}
	for (i = 0; i < 4; i++) {
		data[len - 4 + i] = (unsigned char)((reg ^ crc) & 0xFF);
		reg >>= 8; crc >>= 8;
	}
}

int main(int argc, char **argv)
{
	fat_vol vol;
	static unsigned char rom[1 << 16];
	int r, i;
	if (argc < 2) { printf("pico_cheats_test <card dir>\n"); return 2; }
	s_root = argv[1];
	memset(&vol, 0, sizeof vol);
	for (i = 0; i < (int)sizeof rom; i++) rom[i] = (unsigned char)(i * 7);

	/* 1: by the dump's CRC: Pokemon FireRed (USA, Europe) (Rev 1), 84EE4776 in No-Intro's list */
	forge_crc(rom, sizeof rom, 0x84EE4776u);
	CHECK((uint32_t)crc32(0L, rom, sizeof rom) == 0x84EE4776u, "forged crc");
	pico_cheats_game(&vol, GBA_CONSOLE_GBA, "My FireRed.gba", rom, sizeof rom, &s_core);
	dump("firered by crc");
	CHECK(pico_cheats_page(), "no page for FireRed");
	CHECK(pico_cheats_rows() > 3, "rows %d", pico_cheats_rows());
	CHECK(s_ncodes == 0 && !pico_cheats_on(), "nothing on at first");
	CHECK(!strcmp(pico_cheats_row_title(0), "Cheat list") && !strcmp(pico_cheats_row_value(0), "Found for this game"), "the list row first");
	for (i = 0; i < pico_cheats_rows() && i < 6; i++)
		printf("  %d %s [%s] %s\n", i, pico_cheats_row_head(i) ? "head" : "    ", pico_cheats_row_value(i) ? pico_cheats_row_value(i) : "-", pico_cheats_row_title(i));
	r = pico_cheats_row_head(1) ? 2 : 1;
	CHECK(pico_cheats_row_change(r, 0) == 1, "flip");
	CHECK(pico_cheats_on() && s_ncodes == 1, "one code in force (%d)", s_ncodes);
	CHECK(!strcmp(pico_cheats_row_value(r), "On"), "row says On");
	CHECK(!strcmp(s_codes[0], pico_cheats_row_text(r)), "the code given is the row's");
	pico_cheats_game_end();
	CHECK(!pico_cheats_on() && !pico_cheats_page(), "gone at the end");

	/* 2: the next start finds it on (the .on file) */
	s_ncodes = 0;
	pico_cheats_game(&vol, GBA_CONSOLE_GBA, "My FireRed.gba", rom, sizeof rom, &s_core);
	CHECK(pico_cheats_on() && s_ncodes == 1, "back on at the next start (%d)", s_ncodes);
	CHECK(pico_cheats_row_change(r, 0) == 1 && !pico_cheats_on() && s_ncodes == 0, "off again");
	pico_cheats_game_end();

	/* 3: by its name, a dump no list has: the cheat file's own name */
	memset(rom, 0x5A, sizeof rom);
	pico_cheats_game(&vol, GBA_CONSOLE_GBA, "Sonic Advance (World).gba", rom, sizeof rom, &s_core);
	dump("sonic by name");
	CHECK(pico_cheats_page(), "Sonic Advance by name");
	pico_cheats_game_end();
	/* a name nobody has: the page all the same, its list None, and the chooser to pick one */
	pico_cheats_game(&vol, GBA_CONSOLE_GBA, "Pokemon LeafGreen.gba", rom, sizeof rom, &s_core);
	CHECK(pico_cheats_page() && pico_cheats_rows() == 1, "only the list row (%d)", pico_cheats_rows());
	CHECK(!strcmp(pico_cheats_row_value(0), "None"), "list None: %s", pico_cheats_row_value(0));
	CHECK(pico_cheats_row_change(0, 0) && pico_cheats_choosing(), "the chooser opens");
	{
		int n = pico_cheats_rows(), k, leaf = -1;
		for (k = 0; k < n; k++) if (strstr(pico_cheats_row_title(k), "Leaf Green")) leaf = k;
		printf("chooser: %d rows, row 0 %s, LeafGreen at %d\n", n, pico_cheats_row_title(0), leaf);
		CHECK(leaf >= 0, "opened at the part with the game's name");
		CHECK(leaf >= 0 && pico_cheats_row_change(leaf, 0) && !pico_cheats_choosing(), "picked");
		CHECK(pico_cheats_rows() > 3 && strstr(pico_cheats_row_value(0), "Leaf Green"), "its cheats: %s", pico_cheats_row_value(0));
	}
	pico_cheats_game_end();
	pico_cheats_game(&vol, GBA_CONSOLE_GBA, "Pokemon LeafGreen.gba", rom, sizeof rom, &s_core);
	CHECK(strstr(pico_cheats_row_value(0), "Leaf Green") != 0, "the pick kept for the next start");
	/* None picked */
	{
		int k, n;
		pico_cheats_row_change(0, 0);
		first_part();
		n = pico_cheats_rows();
		for (k = 0; k < n && strcmp(pico_cheats_row_title(k), "None"); k++) ;
		CHECK(k < n && pico_cheats_row_change(k, 0) && pico_cheats_rows() == 1 && !strcmp(pico_cheats_row_value(0), "None"), "None picked");
	}
	/* the search: L, E, A typed on the Search row, and LeafGreen among the few left */
	{
		int k, n, leaf = -1;
		const char *w = "LEAF";
		pico_cheats_row_change(0, 0);
		CHECK(!strcmp(pico_cheats_row_title(0), "Search"), "the chooser starts with Search");
		for (k = 0; w[k]; k++) {
			while (pico_cheats_row_value(0)[strlen(pico_cheats_row_value(0)) - 2] != w[k]) pico_cheats_row_change(0, 1);
			pico_cheats_row_change(0, 0);
		}
		n = pico_cheats_rows();
		for (k = 0; k < n; k++) if (strstr(pico_cheats_row_title(k), "Leaf Green")) leaf = k;
		printf("search LEAF: %d rows, %s, %s; LeafGreen at %d\n", n, pico_cheats_row_value(0), pico_cheats_row_title(1), leaf);
		CHECK(!strcmp(pico_cheats_row_title(1), "Clear search") && leaf > 1 && n < 10, "the search narrowed it");
		CHECK(pico_cheats_row_change(1, 0) && pico_cheats_rows() > 20, "Clear search");
		pico_cheats_page_open();
	}
	pico_cheats_game_end();
	{
		char p2[1024];
		snprintf(p2, sizeof p2, "%s/gammaos/cheats/gba/Pokemon LeafGreen.use", s_root);
		remove(p2);
	}

	/* 4: a long list in parts: the SNES game with the most cheats */
	{
		FILE *f;
		char p[1024];
		snprintf(p, sizeof p, "%s/gammaos/cheats/snes/Long List.cht", s_root);
		{
			char d[1100];
			snprintf(d, sizeof d, "mkdir -p '%s/gammaos/cheats/snes'", s_root);
			if (system(d)) return 1;
		}
		f = fopen(p, "w");
		fprintf(f, "cheats = 230\n\n");
		for (i = 0; i < 230; i++)
			fprintf(f, "cheat%d_desc = \"Cheat %d\"\ncheat%d_code = \"%s%04X\"\ncheat%d_enable = %s\n\n",
				i, i, i, i == 7 ? "BAD" : "7E", i, i, i < 2 || i == 7 ? "true" : "false");
		fclose(f);
		pico_cheats_game(&vol, GBA_CONSOLE_SNES, "Long List.sfc", rom, sizeof rom, &s_core);
		dump("long list (player's .cht)");
		CHECK(pico_cheats_page(), "page for the player's own cheats");
		CHECK(pico_cheats_rows() == 92, "a part is the list row, the parts row and 90 (%d)", pico_cheats_rows());
		CHECK(!strcmp(pico_cheats_row_title(1), "Page") && !strcmp(pico_cheats_row_value(1), "1 / 3"), "parts row: %s %s", pico_cheats_row_title(1), pico_cheats_row_value(1));
		CHECK(pico_cheats_row_head(2) && !strcmp(pico_cheats_row_title(2), "Your cheats"), "heading first");
		CHECK(s_ncodes == 2 && pico_cheats_on(), "the .cht's two enabled codes in force (%d)", s_ncodes);
		r = find_row("Cheat 7");
		CHECK(r > 0 && !strcmp(pico_cheats_row_value(r), "Not read"), "the refused one says so");
		CHECK(pico_cheats_row_change(1, 1) && !strcmp(pico_cheats_row_value(1), "2 / 3"), "the next part");
		CHECK(pico_cheats_step_part(1) && !strcmp(pico_cheats_row_value(1), "3 / 3") && !pico_cheats_step_part(1), "R to the last, no further");
		CHECK(pico_cheats_step_part(-1), "L back");
		CHECK(!strcmp(pico_cheats_row_title(2), "Cheat 89"), "part two starts at 89: %s", pico_cheats_row_title(2));
		CHECK(pico_cheats_row_change(1, -1) && pico_cheats_row_change(1, -1) && !strcmp(pico_cheats_row_value(1), "3 / 3"), "back round to the last");
		CHECK(pico_cheats_rows() == 2 + 231 - 180, "the last part's rows (%d)", pico_cheats_rows());
		/* the file is in the chooser too, under Your cheat files */
		{
			int k, n, head = -1, file = -1;
			pico_cheats_row_change(0, 0);
			first_part();
			n = pico_cheats_rows();
			for (k = 0; k < n; k++) {
				if (!strcmp(pico_cheats_row_title(k), "Your cheat files")) head = k;
				if (!strcmp(pico_cheats_row_title(k), "Long List")) file = k;
			}
			CHECK(head >= 0 && pico_cheats_row_head(head) && file > head, "the player's file listed (%d %d)", head, file);
			pico_cheats_page_open();
			CHECK(!pico_cheats_choosing(), "the page opens on the list again");
		}
		pico_cheats_game_end();
		remove(p);
		snprintf(p, sizeof p, "%s/gammaos/cheats/snes/Long List.on", s_root);
		remove(p);
	}
	/* 5: descriptions that begin like the pack's own lines ("Keep Gaining ...", "Kicked-shell ...")
	 * are cheats all the same: Super Mario World (USA), B19ED489, has 473 */
	{
		char l[128];
		forge_crc(rom, sizeof rom, 0xB19ED489u);
		pico_cheats_game(&vol, GBA_CONSOLE_SNES, "SMW.smc", rom, sizeof rom, &s_core);
		pico_cheats_diag(l, sizeof l);
		printf("smw: %s\n", l);
		CHECK(strstr(l, " c473 ") != 0, "every Super Mario World cheat: %s", l);
		pico_cheats_game_end();
	}
	/* 6: a core with no cheat calls: nothing */
	{
		struct pico_cheat_core none = { 0, 0 };
		pico_cheats_game(&vol, GBA_CONSOLE_GBA, "My FireRed.gba", rom, sizeof rom, &none);
		CHECK(!pico_cheats_page(), "no page without the core's calls");
	}
	/* 7: by CRC, LeafGreen (USA, Europe), D69C96CC, now matched (its cheat file says "(V1.1)(USA)") */
	forge_crc(rom, sizeof rom, 0xD69C96CCu);
	pico_cheats_game(&vol, GBA_CONSOLE_GBA, "LG.gba", rom, sizeof rom, &s_core);
	CHECK(pico_cheats_rows() > 3 && !strcmp(pico_cheats_row_value(0), "Found for this game"), "LeafGreen by CRC (%d)", pico_cheats_rows());
	pico_cheats_game_end();
	{
		char p[1024];
		snprintf(p, sizeof p, "%s/gammaos/cheats/gba/My FireRed.on", s_root);
		remove(p);
	}
	printf(s_fail ? "%d FAILED\n" : "all PASS\n", s_fail);
	return s_fail ? 1 : 0;
}
