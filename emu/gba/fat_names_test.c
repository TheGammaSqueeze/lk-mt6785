/* Host test: names outside ASCII on FAT16, FAT32 and exFAT (fat_names_test.sh makes the images,
 * the files on them written by the host's own file system drivers):
 *  - the reader gives each name in UTF-8, and a path finds an entry by that name only (an
 *    underscore form never stands for another name);
 *  - the writers take UTF-8 names (long names, short names, the exFAT hash), replace and delete;
 *  - the listing keys each game by its UTF-8 name (.name and .uname), from files, zip members
 *    (UTF-8 or code page 437) and 7z members, sorts by the folded name, and lists two games
 *    whose names have the same shape in another script as two games with their own saves;
 *  - the files versions before 1.1 filed under an underscore form (saves, states, box art, play
 *    time, the resume record) are still found, and a write moves them to the name;
 *  - archives.idx in the old format is read again, not misread.
 * Run with: fat_names_test <image> [exfat]; prints PASS / FAIL lines and exits non-zero on FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_ro.h"
#include "fat_wr.h"
#include "sd_fat.h"
#include "pico_fold.h"
#include "gba_sd_save.h"
#include "pico_stats.h"

static FILE *g_img;
unsigned long mmc_wrap_bread(int d, unsigned long b, unsigned long c, void *p, unsigned int pt)
{ (void)d; (void)pt; if (fseek(g_img, (long)b * 512, SEEK_SET)) return 0; return fread(p, 512, c, g_img); }
unsigned long mmc_wrap_bwrite(int d, unsigned long b, unsigned long c, const void *p, unsigned int pt)
{ (void)d; (void)pt; if (fseek(g_img, (long)b * 512, SEEK_SET)) return 0; return fwrite(p, 512, c, g_img); }
int mmc_legacy_init(int x) { (void)x; return 0; }
void msdc_ext_sd_power_on(void) { }
int mt_set_gpio_mode(unsigned int p, unsigned int m) { (void)p; (void)m; return 0; }
int mt_set_gpio_dir(unsigned int p, unsigned int m) { (void)p; (void)m; return 0; }
int mt_set_gpio_pull_enable(unsigned int p, unsigned int m) { (void)p; (void)m; return 0; }
int mt_set_gpio_pull_select(unsigned int p, unsigned int m) { (void)p; (void)m; return 0; }
int mt_get_gpio_in(unsigned int p) { (void)p; return 0; }
void udelay(unsigned long u) { (void)u; }
const char *g_gba_state_dir = "/states/gba";
/* for pico_stats.c: the card it keeps playtime.txt on, written at once */
static fat_vol *g_vol;
fat_vol *gba_sd_menu_vol(void) { return g_vol; }
int card_writer_put(fat_vol *v, const char *dir, const char *name, const void *buf, uint32_t len,
		    char *status, int status_cap, const char *ok, const char *fail)
{ (void)status; (void)status_cap; (void)ok; (void)fail; fat_wr_mkpath(v, dir); return fat_wr_put(v, dir, name, buf, len); }
void *pico_lib_alloc(unsigned int size) { return calloc(1, size); }
long long pico_clock_utc(void) { return 1800000000LL; }
unsigned long current_time(void) { return 0; }
int ayaneo_settings_serialize(unsigned char *b, int cap) { (void)b; (void)cap; return 0; }
void ayaneo_settings_deserialize(const unsigned char *b, int len) { (void)b; (void)len; }

static int g_fail;
static void check(int ok, const char *what, const char *detail)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail ? ": " : "", detail ? detail : "");
	if (!ok) g_fail++;
}

/* the files fat_names_test.sh puts in /roms/gba, as written (UTF-8) */
static const char *const k_files[] = {
	"Pok\xC3\xA9mon \xC3\x89meraude.gba",
	"Poke\xCC\x81mon2.gba",                                          /* decomposed, as macOS writes */
	"\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3.gba",
	"Smile \xF0\x9F\x98\x80.gba",                                   /* outside the BMP */
	"Zelda.gba",
	"apple.gba",
};
#define NFILES (int)(sizeof k_files / sizeof k_files[0])

/* two names of the same shape in other scripts: both "___ ___" in the old underscore form */
#define GB_CN "\xE5\xAE\x9D\xE5\x8F\xAF\xE6\xA2\xA6 \xE6\xB5\x8B\xE8\xAF\x95\xE7\x89\x88"
#define GB_KO "\xED\x8F\xAC\xEC\xBC\x93\xEB\xAA\xAC \xED\x85\x8C\xEC\x8A\xA4\xED\x8A\xB8"
#define PK_E  "Pok\xC3\xA9mon \xC3\x89meraude"

/* the games the listing gives, in the order it must give them (each one's .name and .uname) */
static const char *const k_games[] = {
	"apple",
	"Caf\xC3\xA9 One",                                               /* zip, marked UTF-8 */
	"Caf\xC3\xA9 Two",                                               /* zip, code page 437 */
	"Huit",                                                           /* 7z */
	"Old",                                                            /* zip, code page 437 */
	PK_E,
	"Poke\xCC\x81mon2",
	"Sept \xC3\xA9",                                                  /* 7z */
	"Smile \xF0\x9F\x98\x80",
	"x",                                                              /* /Jeux é/gba, folders.txt */
	"Zelda",
	"Zeta",                                                           /* zip, marked UTF-8 */
};
#define NGAMES (int)(sizeof k_games / sizeof k_games[0])

static int utf8_whole(const char *s)
{
	const char *p = s;
	while (*p) {
		const char *q = p;
		uint32_t c = fat_u8_next(&q);
		if (c >= 0x80 && q - p == 1) return 0;   /* a stray byte */
		p = q;
	}
	return 1;
}

static void test_reader(fat_vol *v)
{
	fat_dir d; fat_dirent e;
	int seen[NFILES] = { 0 }, i, longs = 0;
	char buf[300];
	if (fat_opendir(v, "/roms/gba", &d) != 0) { check(0, "open /roms/gba", 0); return; }
	while (fat_readdir(&d, &e)) {
		for (i = 0; i < NFILES; i++) if (!strcmp(e.name, k_files[i])) seen[i] = 1;
		if (!strncmp(e.name, "\xE3\x81\x82\xE3\x81\x82", 6)) {   /* the long name of 100 x U+3042 */
			longs++;
			snprintf(buf, sizeof buf, "%d bytes", (int)strlen(e.name));
			check(strlen(e.name) <= 255 && strlen(e.name) >= 250 && utf8_whole(e.name) &&
			      !strcmp(e.name + strlen(e.name) - 4, ".gba"), "a long UTF-8 name is cut at a whole character, keeping its extension", buf);
		}
	}
	for (i = 0; i < NFILES; i++) check(seen[i], "readdir gives the UTF-8 name", k_files[i]);
	check(longs == 1, "readdir finds the long name", 0);
	for (i = 0; i < NFILES; i++) {
		fat_file f;
		snprintf(buf, sizeof buf, "/roms/gba/%s", k_files[i]);
		check(fat_open(v, buf, &f) == 0 && f.size > 0, "open by UTF-8 name", k_files[i]);
	}
	{	/* an underscore form opens no file of another name */
		fat_file f;
		check(fat_open(v, "/roms/gba/Pok_mon _meraude.gba", &f) != 0, "an underscore form is no wildcard", 0);
	}
	check(fat_opendir(v, "/Jeux \xC3\xA9/gba", &d) == 0, "folder by UTF-8 name", "/Jeux \xC3\xA9/gba");
	check(fat_opendir(v, "/Jeux _/gba", &d) != 0, "folder not by an underscore form", 0);
	check(fat_opendir(v, "/jeux \xC3\xA9/GBA", &d) == 0, "ASCII letters in either case", 0);
}

static int read_back(fat_vol *v, const char *path, const char *want, uint32_t len)
{
	fat_file f; char got[64];
	if (fat_open(v, path, &f) != 0 || f.size != len || len > sizeof got) return 0;
	return fat_read(&f, 0, got, len) == len && !memcmp(got, want, len);
}

static void test_writer(fat_vol *v, int exfat)
{
	static const char *names[] = { "\xC3\x89" "cran \xC3\xBC.sav", "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88.sav",
				       "x\xF0\x9F\x98\x80.sav", "plain.sav" };
	static const char *keys[] = { "_cran _.sav", "___.sav", "x__.sav", "plain.sav" };   /* no file of these */
	char path[300];
	int i, n;
	fat_dir d; fat_dirent e;
	check(fat_wr_mkpath(v, "/saves/gba") == 0, "mkpath /saves/gba", 0);
	for (i = 0; i < 4; i++) {
		char data[32];
		snprintf(data, sizeof data, "first %d", i);
		check(fat_wr_put(v, "/saves/gba", names[i], data, (uint32_t)strlen(data)) == 0, "write", names[i]);
	}
	/* replace one: the old entry goes, one entry stays */
	check(fat_wr_put(v, "/saves/gba", names[0], "second", 6) == 0, "replace", names[0]);
	for (i = 0; i < 4; i++) {
		char data[32];
		if (i == 0) strcpy(data, "second"); else snprintf(data, sizeof data, "first %d", i);
		snprintf(path, sizeof path, "/saves/gba/%s", names[i]);
		check(read_back(v, path, data, (uint32_t)strlen(data)), "read back by UTF-8 name", names[i]);
		if (i < 3) {
			fat_file f;
			snprintf(path, sizeof path, "/saves/gba/%s", keys[i]);
			check(fat_open(v, path, &f) != 0, "not under an underscore form", keys[i]);
		}
	}
	/* the listing: each once, with both forms right */
	n = 0;
	if (fat_opendir(v, "/saves/gba", &d) == 0) while (fat_readdir(&d, &e)) {
		for (i = 0; i < 4; i++) if (fat_name_eq(e.name, names[i])) n++;
	}
	check(n == 4, "the folder lists each once", 0);
	/* delete one by its UTF-8 name and the cluster the listing gave */
	if (fat_opendir(v, "/saves/gba", &d) == 0) while (fat_readdir(&d, &e)) {
		if (!strcmp(e.name, names[1])) { check(fat_wr_delete(v, "/saves/gba", names[1], e.first_clus) == 0, "delete", names[1]); break; }
	}
	snprintf(path, sizeof path, "/saves/gba/%s", names[1]);
	{ fat_file f; check(fat_open(v, path, &f) != 0, "deleted is gone", names[1]); }
	if (exfat) {	/* a folder of a UTF-8 name (the FAT writer makes 8.3 folders only) */
		check(fat_wr_mkpath(v, "/saves/Jeux \xC3\xA9/sub") == 0, "exFAT mkpath with a UTF-8 folder", 0);
		check(fat_opendir(v, "/saves/Jeux \xC3\xA9/sub", &d) == 0, "found by its name", 0);
		check(fat_wr_put(v, "/saves/Jeux \xC3\xA9/sub", "in.sav", "in", 2) == 0 &&
		      read_back(v, "/saves/Jeux \xC3\xA9/sub/in.sav", "in", 2), "a file in it", 0);
	}
}

static void test_fold(void)
{
	char out[64];
	pico_fold_ascii("Poke\xCC\x81mon \xC3\x89meraude \xC5\x81\xC3\xB3" "d\xC5\xBA \xE1\xBB\x87 \xEF\xBC\xA1", out, sizeof out);
	check(!strcmp(out, "Pokemon Emeraude Lodz e A"), "fold to ASCII (decomposed, Latin-1, Ext-A, Ext Additional, fullwidth)", out);
	pico_fold_ascii("\xE3\x83\x9D\xE3\x82\xB1 \xF0\x9F\x98\x80!", out, sizeof out);
	check(!strcmp(out, "__ _!"), "fold to ASCII (others as one '_' each)", out);
	check(pico_fold_cmp("Pok\xC3\xA9mon", "Poke\xCC\x81mon") == 0, "precomposed and decomposed compare equal", 0);
	check(pico_fold_cmp("pokemon", "POK\xC3\x89MON") == 0, "case and accents alike", 0);
	check(pico_fold_cmp("Zelda", "\xE3\x81\x82") < 0, "ASCII before other scripts", 0);
	check(pico_fold_cmp("a", "ab") < 0 && pico_fold_cmp("b", "ab") > 0, "prefix first", 0);
	pico_fold_ascii("\xC3\xA9t\xC3\xA9", out, 3);
	check(!strcmp(out, "et"), "fold to ASCII stops at the cap", out);
}

extern volatile unsigned int g_dbg_aix[3];
static void test_listing(fat_vol *v, const char *label)
{
	static gba_rom_entry r[64];
	int n, tot, i, ok = 1, k = 0, j, cn = 0, ko = 0;
	char buf[400];
	n = gba_sd_list_roms(v, r, 64, &tot);
	for (i = 0; i < n; i++) printf("  %s %2d %u %s\n", label, i, r[i].type, r[i].name);
	for (i = 0; i < n; i++) {
		if (!utf8_whole(r[i].name) || strlen(r[i].name) > 127 || strcmp(r[i].name, r[i].uname)) ok = 0;
		if (!strcmp(r[i].name, GB_CN)) cn++;
		if (!strcmp(r[i].name, GB_KO)) ko++;
	}
	check(ok, "every name whole UTF-8, at most 127 bytes, and the same as uname", 0);
	check(cn == 1 && ko == 1, "two names of the same underscore form are two games", label);
	for (i = 0, j = 0; i < n; i++) {	/* the GBA games, in order, the two in kana last */
		if (r[i].type != GBA_CONSOLE_GBA) continue;
		if (!strncmp(r[i].name, "\xE3\x81\x82", 3) || !strncmp(r[i].name, "\xE3\x83\x9D", 3)) { j++; continue; }
		if (k >= NGAMES || j) { check(0, "listed in order", r[i].name); continue; }
		snprintf(buf, sizeof buf, "#%d '%s', want '%s'", i, r[i].name, k_games[k]);
		check(!strcmp(r[i].name, k_games[k]), "listed in order", buf);
		k++;
	}
	check(k == NGAMES && j == 2, "every game of the folder listed", label);
}

/* the files fat_names_test.sh left under the underscore form (as before 1.1) */
static void test_legacy(fat_vol *v)
{
	unsigned char b[64];
	char key[128];
	uint32_t n;
	check(gba_sd_legacy_key(GB_CN, key, sizeof key) == 1 && !strcmp(key, "___ ___"), "legacy key of a CJK name", key);
	check(gba_sd_legacy_key("Smile \xF0\x9F\x98\x80", key, sizeof key) == 1 && !strcmp(key, "Smile __"), "legacy key beyond U+FFFF", key);
	check(gba_sd_legacy_key("Zelda", key, sizeof key) == 0 && !strcmp(key, "Zelda"), "an ASCII key is its own", key);
	n = gba_sd_load_sav(v, PK_E, b, sizeof b);
	check(n == 6 && !memcmp(b, "oldsav", 6), "a save under the old form is read", 0);
	n = gba_sd_load_state(v, PK_E, 3, b, sizeof b);
	check(n == 6 && !memcmp(b, "oldst3", 6), "a state under the old form is read", 0);
	n = gba_sd_read_named(v, "/roms/gba/boxart", PK_E, "ART", b, sizeof b);
	check(n == 6 && !memcmp(b, "oldart", 6), "box art under the old form is read", 0);
	/* the next write goes under the name, and is what is read from then on */
	check(gba_sd_write_sav(v, PK_E, "newsav", 6) == 0, "a save written under the name", 0);
	n = gba_sd_load_sav(v, PK_E, b, sizeof b);
	check(n == 6 && !memcmp(b, "newsav", 6), "then read from the name", 0);
	/* a CJK name no other game shares the underscore form of still reads its old files */
	check(!gba_sd_legacy_shared("____"), "ポケモン's form is its own", 0);
	n = gba_sd_load_sav(v, "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3", b, sizeof b);
	check(n == 6 && !memcmp(b, "oldpk4", 6), "a CJK name of its own form reads its old save", 0);
	{	/* the two games of one underscore form: neither reads the old files, whose they are cannot
		 * be told; each keeps what it writes */
		check(gba_sd_legacy_shared("___ ___"), "the form two games share is known", 0);
		check(gba_sd_load_sav(v, GB_CN, b, sizeof b) == 0, "game A does not read the shared old save", 0);
		check(gba_sd_load_sav(v, GB_KO, b, sizeof b) == 0, "game B does not read the shared old save", 0);
		check(gba_sd_read_named(v, "/roms/gb/boxart", GB_CN, "ART", b, sizeof b) == 0 &&
		      gba_sd_read_named(v, "/roms/gb/boxart", GB_KO, "ART", b, sizeof b) == 0, "neither reads the shared old .ART", 0);
		check(gba_sd_resume_is(GB_CN, "___ ___") == 0 && gba_sd_resume_is(GB_KO, "___ ___") == 0,
		      "neither takes the shared old resume record", 0);
		{
			unsigned int secs = 0; long long last = 0;
			check(!pico_stats_get(GBA_CONSOLE_GB, GB_CN, &secs, &last) && !pico_stats_get(GBA_CONSOLE_GB, GB_KO, &secs, &last),
			      "neither takes the shared old play time", 0);
		}
		check(gba_sd_write_sav(v, GB_CN, "A-save", 6) == 0, "game A writes its own", 0);
		n = gba_sd_load_sav(v, GB_KO, b, sizeof b);
		check(n == 0, "game B does not read game A's save", 0);
		check(gba_sd_write_sav(v, GB_KO, "B-save", 6) == 0, "game B writes its own", 0);
		n = gba_sd_load_sav(v, GB_CN, b, sizeof b);
		check(n == 6 && !memcmp(b, "A-save", 6), "game A keeps its save", 0);
		n = gba_sd_load_sav(v, GB_KO, b, sizeof b);
		check(n == 6 && !memcmp(b, "B-save", 6), "game B keeps its save", 0);
		{
			fat_file f;
			check(fat_open(v, "/saves/gba/___ ___.sav", &f) == 0 && f.size == 6, "the shared old save stays on the card", 0);
		}
	}
	/* the resume record */
	check(gba_sd_resume_is(PK_E, PK_E) == 2, "resume record of the name", 0);
	check(gba_sd_resume_is(PK_E, "Pok_mon _meraude") == 1, "resume record of the old form", 0);
	check(gba_sd_resume_is("Pokemon", "Pok_mon") == 0, "an ASCII name has no old form", 0);
	{
		char rec[100], longk[128];
		memset(longk, 'a', 120); longk[120] = 0;
		memcpy(rec, longk, 99); rec[99] = 0;
		check(gba_sd_resume_is(longk, rec) == 2, "a record cut at 99 bytes", 0);
	}
	/* play time: the old line is the game's, and its first start moves it to the name */
	{
		unsigned int secs = 0; long long last = 0;
		check(pico_stats_get(GBA_CONSOLE_GBA, PK_E, &secs, &last) && secs == 4242, "play time under the old form", 0);
		pico_stats_begin(GBA_CONSOLE_GBA, PK_E);
		pico_stats_end();
		{
			fat_file f; static char t[4096]; uint32_t m;
			int ok = fat_open(v, "/gammaos/playtime.txt", &f) == 0 && (m = fat_read(&f, 0, t, sizeof t - 1)) > 0;
			if (ok) t[m] = 0;
			check(ok && strstr(t, " " PK_E "\n") && !strstr(t, "Pok_mon _meraude"), "play time moved to the name, once", t);
		}
		check(pico_stats_get(GBA_CONSOLE_GBA, PK_E, &secs, &last) && secs == 4242, "and still read", 0);
	}
}

int main(int argc, char **argv)
{
	static fat_vol v;
	int exfat = argc > 2 && !strcmp(argv[2], "exfat");
	if (argc < 2 || !(g_img = fopen(argv[1], "r+b"))) { printf("usage: fat_names_test <image> [exfat]\n"); return 2; }
	if (gba_sd_mount(&v) != 0) { printf("FAIL mount\n"); return 1; }
	g_vol = &v;
	printf("fs %s\n", v.is_exfat ? "exFAT" : v.is_fat32 ? "FAT32" : "FAT16");
	test_fold();
	test_reader(&v);
	test_listing(&v, "first");
	check(g_dbg_aix[1] >= 3, "archives looked into the first time", 0);
	test_listing(&v, "index");
	check(g_dbg_aix[1] == 0 && g_dbg_aix[0] >= 3, "archives from the index the second time", 0);
	{	/* the index as an older version wrote it: version 1, so read again, not misread */
		fat_file f; static unsigned char ix[1 << 16];
		if (fat_open(&v, "/gammaos/archives.idx", &f) == 0 && f.size <= sizeof ix && fat_read(&f, 0, ix, f.size) == f.size) {
			check(ix[4] == 2, "the index is written in format 2", 0);
			ix[4] = 1;
			fat_wr_put(&v, "/gammaos", "archives.idx", ix, f.size);
			test_listing(&v, "old-index");
			check(g_dbg_aix[0] == 0 && g_dbg_aix[1] >= 3, "a format 1 index is read again", 0);
		} else check(0, "the index was written", 0);
	}
	test_legacy(&v);
	test_writer(&v, exfat || v.is_exfat);
	fclose(g_img);
	printf("%s\n", g_fail ? "== FAILURES ==" : "== all names tests PASS ==");
	return g_fail ? 1 : 0;
}
