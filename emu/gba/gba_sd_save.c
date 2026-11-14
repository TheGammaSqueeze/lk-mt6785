/* Save/state persistence to the microSD - see gba_sd_save.h. */
#include "gba_sd_save.h"
#include "fat_wr.h"
#include <string.h>
extern const char *g_gba_state_dir;   /* gba_driver.c: "/states/gba" for gpSP, the core's own folder for armGBA */
/* the card writer (card_writer.c), where the LK has one: states and battery saves go to the card
 * off the game's thread, and a read waits for what is still being written */
extern int  card_writer_put(fat_vol *v, const char *dir, const char *name, const void *buf, uint32_t len,
			    char *status, int status_cap, const char *ok, const char *fail) __attribute__((weak));
extern void card_writer_flush(void) __attribute__((weak));

/* "<rombase>.<ext>" from a ROM name into out[<=cap]. The roster already drops the ROM's
 * extension (sd_fat.c scan_rom_folder), so only a real ROM extension is stripped here: cutting at
 * the last dot whatever it was turned "Mario vs. Donkey Kong" into "Mario vs", so its box art was
 * never found and its saves and states were keyed (and could collide) under the cut name. */
static int is_rom_ext(const char *e)
{
	static const char *const known[] = { "gba", "gb", "gbc", "sfc", "smc", "md", "gen", "bin", "smd", "sms", "gg", "sg", 0 };
	int i, j;
	for (i = 0; known[i]; i++) {
		for (j = 0; known[i][j] && e[j]; j++) {
			char c = e[j]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
			if (c != known[i][j]) break;
		}
		if (!known[i][j] && !e[j]) return 1;
	}
	return 0;
}
static void base_ext_mode(const char *rom, const char *ext, char *out, int cap, int legacy)
{
	int L = 0, dot = -1, i, k = 0;
	while (rom[L]) L++;
	for (i = L - 1; i >= 0; i--) if (rom[i] == '.') { dot = i; break; }
	if (dot < 0 || (!legacy && !is_rom_ext(rom + dot + 1))) dot = L;
	for (i = 0; i < dot && k < cap - 6; i++) out[k++] = rom[i];
	out[k++] = '.';
	for (i = 0; ext[i] && k < cap - 1; i++) out[k++] = ext[i];
	out[k] = 0;
}
static void base_ext(const char *rom, const char *ext, char *out, int cap) { base_ext_mode(rom, ext, out, cap, 0); }
void gba_sd_file_name(const char *rom, const char *ext, char *out, int cap, int legacy)
{
	base_ext_mode(rom, ext, out, cap, legacy);
}

int gba_sd_legacy_key(const char *key, char *out, int cap)
{
	int o = 0, other = 0;
	if (cap > 128) cap = 128;              /* the 127 characters the older key was cut at */
	if (cap < 1) return 0;
	while (*key && o < cap - 1) {
		uint32_t c = fat_u8_next(&key);
		if (c < 0x80) { out[o++] = (char)c; continue; }
		other = 1;
		out[o++] = '_';
		if (c >= 0x10000 && o < cap - 1) out[o++] = '_';
	}
	out[o] = 0;
	return other;
}

/* the underscore forms of the games listed: a hash set of their two 32 bit hashes, with how many
 * games have each (up to 2: enough to say shared) */
#define LEG_SLOTS 65536u                  /* a power of two, twice the games listed at most */
struct leg_ent { uint32_t a, b; uint8_t n; };
static struct leg_ent *s_leg;
static int s_leg_ready;
static void *leg_mem(unsigned int n)
{
#ifdef __arm__
	extern void *pico_lib_alloc(unsigned int size);
	return pico_lib_alloc(n);
#else
	extern void *calloc(unsigned long, unsigned long);
	return calloc(1, n);
#endif
}
/* the slot of a form: where it is, or the empty one it would take (nothing written) */
static struct leg_ent *leg_slot(const char *s, uint32_t *pa, uint32_t *pb)
{
	uint32_t a = 2166136261u, b = 5381u, i;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);   /* the card's names match in either case */
		a = (a ^ c) * 16777619u;
		b = b * 33u + c;
	}
	a |= 1u;                                                      /* 0 marks an empty slot */
	for (i = (a ^ (b << 9)) & (LEG_SLOTS - 1u); s_leg[i].a && (s_leg[i].a != a || s_leg[i].b != b);
	     i = (i + 1u) & (LEG_SLOTS - 1u)) ;
	*pa = a; *pb = b;
	return &s_leg[i];
}
void gba_sd_legacy_begin(void)
{
	s_leg_ready = 0;
	if (!s_leg) s_leg = (struct leg_ent *)leg_mem(LEG_SLOTS * (unsigned)sizeof *s_leg);
	if (s_leg) { memset(s_leg, 0, LEG_SLOTS * sizeof *s_leg); s_leg_ready = 1; }
}
void gba_sd_legacy_add(const char *key)
{
	char legacy[128];
	struct leg_ent *e;
	uint32_t a, b;
	if (!s_leg_ready) return;
	gba_sd_legacy_key(key, legacy, (int)sizeof legacy);
	e = leg_slot(legacy, &a, &b);
	e->a = a; e->b = b;
	if (e->n < 2) e->n++;
}
int gba_sd_legacy_shared(const char *legacy)
{
	uint32_t a, b;
	if (!s_leg_ready) return 1;           /* nothing listed: whose a file is cannot be told */
	return leg_slot(legacy, &a, &b)->n >= 2;
}
int gba_sd_legacy_own(const char *key, char *out, int cap)
{
	return gba_sd_legacy_key(key, out, cap) && !gba_sd_legacy_shared(out);
}

#define RESUME_REC 99                  /* the bytes of a name the resume record holds */
/* rec names key: the same, or key's start when the record was cut */
static int rec_is(const char *key, const char *rec)
{
	size_t L = strlen(rec);
	return L == RESUME_REC ? !strncmp(key, rec, L) : !strcmp(key, rec);
}
int gba_sd_resume_is(const char *key, const char *rec)
{
	char legacy[128];
	if (!key || !rec || !rec[0]) return 0;
	if (rec_is(key, rec)) return 2;
	return gba_sd_legacy_own(key, legacy, (int)sizeof legacy) && rec_is(legacy, rec) ? 1 : 0;
}

static uint32_t read_one(fat_vol *v, const char *dir, const char *nm, unsigned char *dst, uint32_t cap)
{
	char path[300];
	int k = 0, i;
	fat_file f;
	if (card_writer_flush) card_writer_flush();   /* a file still being written is read once it is there */
	for (i = 0; dir[i] && k < (int)sizeof path - 2; i++) path[k++] = dir[i];
	if (k == 0 || path[k - 1] != '/') path[k++] = '/';
	for (i = 0; nm[i] && k < (int)sizeof path - 1; i++) path[k++] = nm[i];
	path[k] = 0;
	if (fat_open(v, path, &f) != 0) return 0;
	return fat_read(&f, 0, dst, cap < f.size ? cap : f.size);
}

/* the file a game's key and an older form of it would name, unless it is one already tried */
static uint32_t read_form(fat_vol *v, const char *dir, const char *rom, const char *ext, int cut,
			  char tried[4][288], int *nt, unsigned char *dst, uint32_t cap)
{
	int i;
	base_ext_mode(rom, ext, tried[*nt], (int)sizeof tried[0], cut);
	for (i = 0; i < *nt; i++) if (!strcmp(tried[i], tried[*nt])) return 0;
	return read_one(v, dir, tried[(*nt)++], dst, cap);
}

uint32_t gba_sd_read_named(fat_vol *v, const char *dir, const char *romname,
			   const char *ext, unsigned char *dst, uint32_t cap)
{
	char tried[4][288], legacy[128];
	int nt = 0;
	uint32_t n;
	/* the name; then where older versions put the file (the next write puts it under the name):
	 * cut at its last dot, whatever followed it; and with each character outside ASCII as '_' */
	if ((n = read_form(v, dir, romname, ext, 0, tried, &nt, dst, cap)) != 0) return n;
	if ((n = read_form(v, dir, romname, ext, 1, tried, &nt, dst, cap)) != 0) return n;
	/* the underscore form only when it is this game's alone: another game of that form may be
	 * the one the file is for */
	if (!gba_sd_legacy_own(romname, legacy, (int)sizeof legacy)) return 0;
	if ((n = read_form(v, dir, legacy, ext, 0, tried, &nt, dst, cap)) != 0) return n;
	return read_form(v, dir, legacy, ext, 1, tried, &nt, dst, cap);
}

int gba_sd_write_named(fat_vol *v, const char *dir, const char *romname,
		       const char *ext, const void *buf, uint32_t len)
{
	char nm[288];
	base_ext(romname, ext, nm, (int)sizeof nm);
	fat_wr_mkpath(v, dir);   /* ensure saves/gba or states/gba exists (idempotent) */
	return fat_wr_put(v, dir, nm, buf, len);
}

uint32_t gba_sd_load_sav(fat_vol *v, const char *rom, unsigned char *dst, uint32_t cap)
{ return gba_sd_read_named(v, "/saves/gba", rom, "sav", dst, cap); }

int gba_sd_write_sav(fat_vol *v, const char *rom, const void *buf, uint32_t len)
{ return gba_sd_write_named(v, "/saves/gba", rom, "sav", buf, len); }

uint32_t gba_sd_load_state(fat_vol *v, const char *rom, int slot, unsigned char *dst, uint32_t cap)
{
	char ext[4]; ext[0] = 's'; ext[1] = 't'; ext[2] = (char)('0' + (slot % 10)); ext[3] = 0;
	return gba_sd_read_named(v, g_gba_state_dir, rom, ext, dst, cap);
}

int gba_sd_write_state(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len)
{
	char ext[4]; ext[0] = 's'; ext[1] = 't'; ext[2] = (char)('0' + (slot % 10)); ext[3] = 0;
	return gba_sd_write_named(v, g_gba_state_dir, rom, ext, buf, len);
}

/* GammaOS menu settings, kept ON THE SD CARD only (the LK writes nothing on the eMMC): stored as
 * /gammaos/settings.bin, the serialized settings block (ayaneo_settings_serialize). A card from an
 * older build has them in /gba/settings.bin, read when the new file is not there yet; the next
 * save writes the new place. Read by the SD gate before the intro (skip intro, brightness...). */
extern int  ayaneo_settings_serialize(unsigned char *b, int cap);
extern void ayaneo_settings_deserialize(const unsigned char *b, int len);

int gba_sd_settings_load(fat_vol *v)
{
	unsigned char b[192];
	fat_file f;
	if (fat_open(v, "/gammaos/settings.bin", &f) != 0 && fat_open(v, "/gba/settings.bin", &f) != 0)
		return -1;
	if (fat_read(&f, 0, b, (uint32_t)sizeof b) != (uint32_t)sizeof b) return -1;
	ayaneo_settings_deserialize(b, (int)sizeof b);
	return 0;
}

int gba_sd_settings_write(fat_vol *v, const unsigned char *b, int n)
{
	if (n <= 0) return -1;
	fat_wr_mkpath(v, "/gammaos");
	return fat_wr_put(v, "/gammaos", "settings.bin", b, (uint32_t)n);
}

int gba_sd_settings_save(fat_vol *v)
{
	unsigned char b[192];
	return gba_sd_settings_write(v, b, ayaneo_settings_serialize(b, (int)sizeof b));
}

/* the same files, written off the caller's thread (status gets ok or fail once it is on the card) */
static int write_named_bg(fat_vol *v, const char *dir, const char *romname, const char *ext,
			  const void *buf, uint32_t len, char *status, int cap, const char *ok, const char *fail)
{
	char nm[288];
	int rc, i;
	base_ext(romname, ext, nm, (int)sizeof nm);
	if (card_writer_put) return card_writer_put(v, dir, nm, buf, len, status, cap, ok, fail);
	fat_wr_mkpath(v, dir);
	rc = fat_wr_put(v, dir, nm, buf, len);
	if (status && cap > 0) {
		const char *m = rc ? fail : ok;
		for (i = 0; i + 1 < cap && m && m[i]; i++) status[i] = m[i];
		status[i] = 0;
	}
	return rc ? -1 : 0;
}
int gba_sd_write_sav_bg(fat_vol *v, const char *rom, const void *buf, uint32_t len)
{ return write_named_bg(v, "/saves/gba", rom, "sav", buf, len, 0, 0, 0, 0); }
int gba_sd_write_named_bg(fat_vol *v, const char *dir, const char *romname, const char *ext,
			  const void *buf, uint32_t len)
{ return write_named_bg(v, dir, romname, ext, buf, len, 0, 0, 0, 0); }
int gba_sd_write_state_bg(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len,
			  char *status, int cap, const char *ok, const char *fail)
{
	char ext[4]; ext[0] = 's'; ext[1] = 't'; ext[2] = (char)('0' + (slot % 10)); ext[3] = 0;
	return write_named_bg(v, g_gba_state_dir, rom, ext, buf, len, status, cap, ok, fail);
}
/* "<rombase>.<ext>", as the card keeps a game's file under */
void gba_sd_state_name(const char *romname, const char *ext, char *out, int cap) { base_ext(romname, ext, out, cap); }
