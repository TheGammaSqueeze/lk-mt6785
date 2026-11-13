/* On-device SD -> FAT glue (LK only). See sd_fat.h. */
#include "sd_fat.h"
#include "fat_wr.h"   /* fat_wr_mkpath for gba_sd_make_rom_dirs */
#include "pico_libmem.h"   /* PICO_GAMES_MAX, the listing's tables */
#include "rom_archive.h"   /* games in .zip and .7z */
#include <string.h>   /* memcpy: the sector cache */

/* LK MMC block-read: reads blkcnt 512B sectors at LBA blknr from mmc device
 * dev_num, partition part_id, into dst. Returns blkcnt on success. Declared here
 * to avoid pulling the whole MTK storage header set into the emu module. */
extern unsigned long mmc_wrap_bread(int dev_num, unsigned long blknr,
				    unsigned long blkcnt, void *dst, unsigned int part_id);
extern unsigned long mmc_wrap_bwrite(int dev_num, unsigned long blknr,
				     unsigned long blkcnt, const void *src, unsigned int part_id);
/* Bring up an MMC host: verbose-1 = host id. platform.c only inits the boot
 * storage (msdc0 = internal eMMC via mmc_legacy_init(1)); the external microSD is
 * a separate host (msdc1) that nothing else initializes, so we must do it here.
 * verbose=2 -> id=1 = msdc1 (the removable SD slot). Returns 0 (MMC_ERR_NONE) ok. */
extern int mmc_legacy_init(int verbose);
/* Enable the external microSD LDOs (VMCH card power + VMC IO) - the LK build does
 * not power them otherwise (only the eMMC boot rail is left on by the preloader),
 * so mmc_init(1) would find no card. Defined in platform/mt6785/msdc_io.c. */
extern void msdc_ext_sd_power_on(void);
void ayaneo_storage_lock(void);
void ayaneo_storage_unlock(void);

/* mmc_legacy_init(1) in platform.c -> id = verbose-1 = 0, so the external microSD
 * (MMC_SLOT=1 hardware; UFS is the boot device, so this MMC slot is the card) is
 * device 0. part_id 0 = the user data area (SD cards have no boot partitions).
 * These are compile-time so they are trivial to retarget once verified on HW. */
#ifndef SD_DEV_NUM
#define SD_DEV_NUM 1          /* msdc1 = external microSD (msdc0 = internal eMMC) */
#endif
#ifndef SD_PART_USER
#define SD_PART_USER 0
#endif
/* verbose arg to mmc_legacy_init that maps to SD_DEV_NUM (id = verbose - 1). */
#define SD_MMC_VERBOSE (SD_DEV_NUM + 1)

/* Command trace ring: msdc.c (host 1) records every command here so the fastboot
 * sd-probe can show the exact identification sequence and where it fails. */
#define GBA_SD_TRACE_N 24
struct gba_sd_trace_ent { unsigned op, app, err, r0; };
static struct gba_sd_trace_ent s_trace[GBA_SD_TRACE_N];
static unsigned s_trace_cnt;
void gba_sd_cmdtrace(unsigned op, unsigned app, unsigned err, unsigned r0)
{
	unsigned i = s_trace_cnt % GBA_SD_TRACE_N;
	s_trace[i].op = op; s_trace[i].app = app;
	s_trace[i].err = err; s_trace[i].r0 = r0;
	s_trace_cnt++;
}
/* Read back trace entry #idx (0 = oldest kept). Returns 0 if out of range. */
int gba_sd_trace_get(unsigned idx, unsigned *op, unsigned *app, unsigned *err, unsigned *r0)
{
	unsigned n = s_trace_cnt < GBA_SD_TRACE_N ? s_trace_cnt : GBA_SD_TRACE_N;
	unsigned base = s_trace_cnt < GBA_SD_TRACE_N ? 0 : s_trace_cnt % GBA_SD_TRACE_N;
	if (idx >= n) return 0;
	{ unsigned i = (base + idx) % GBA_SD_TRACE_N;
	  *op = s_trace[i].op; *app = s_trace[i].app;
	  *err = s_trace[i].err; *r0 = s_trace[i].r0; }
	return 1;
}

/* The last sectors read one at a time, kept: the FAT layer reads directories, the FAT and the
 * exFAT bitmap a sector at a time (a directory's sector once per 32 byte entry in it), and every
 * open walks its path from the root again, each read a whole card command. With no cache, opening
 * one small file took most of a second and the menu's box art (a lookup per game through four
 * directories) two and a half. Writes go through the cache too (sd_write), so it never holds a
 * sector older than the card's; a mount starts it empty. */
#define SEC_CACHE_N 64
static struct { uint32_t lba, used; int ok; } s_sc[SEC_CACHE_N];
static uint8_t s_sc_buf[SEC_CACHE_N][512];
static uint32_t s_sc_clock;
volatile unsigned g_dbg_sc_hit, g_dbg_sc_miss;   /* oem diag: the cache's hits and misses */
static int sc_find(uint32_t lba)
{
	int i;
	for (i = 0; i < SEC_CACHE_N; i++)
		if (s_sc[i].ok && s_sc[i].lba == lba) return i;
	return -1;
}
static void sc_keep(uint32_t lba, const void *buf)
{
	int i = sc_find(lba), k;
	if (i < 0) {	/* the least recently used place */
		for (i = 0, k = 1; k < SEC_CACHE_N; k++)
			if (!s_sc[k].ok || (s_sc[i].ok && s_sc[k].used < s_sc[i].used)) i = k;
	}
	memcpy(s_sc_buf[i], buf, 512);
	s_sc[i].lba = lba; s_sc[i].ok = 1; s_sc[i].used = ++s_sc_clock;
}
static void sc_clear(void)
{
	int i;
	for (i = 0; i < SEC_CACHE_N; i++) s_sc[i].ok = 0;
}

static unsigned sd_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	(void)ctx;
	if (count == 1) {	/* a sector read lately: from the cache, no card command */
		int i;
		ayaneo_storage_lock();
		i = sc_find(lba);
		if (i >= 0) {
			memcpy(buf, s_sc_buf[i], 512);
			s_sc[i].used = ++s_sc_clock;
			g_dbg_sc_hit++;
			ayaneo_storage_unlock();
			return 1;
		}
		ayaneo_storage_unlock();
	}
#ifdef AYANEO_GBA_SD
	/* Kick the 10s LK watchdog on every SD sector read so a long transfer (e.g.
	 * loading a 16MB ROM, which is one big fat_read that blocks well past 10s)
	 * does not let the watchdog reset the device mid-load. Covers ROM/BIOS/state
	 * loads and the mount reads uniformly. */
	{ extern void mtk_wdt_restart(void); mtk_wdt_restart(); }
#endif
	{
		unsigned r;
		ayaneo_storage_lock();
		r = (unsigned)mmc_wrap_bread(SD_DEV_NUM, (unsigned long)lba,
					     (unsigned long)count, buf, SD_PART_USER);
		if (count == 1 && r == 1) { sc_keep(lba, buf); g_dbg_sc_miss++; }
		ayaneo_storage_unlock();
		return r;
	}
}

/* One mutex serialises every block transfer (SD and the eMMC settings block): the settings
 * writer runs on its own low priority thread while a game plays, and the game thread may
 * read or write the card at the same moment (saves, states, box art). Lazily initialised
 * (the first users run single threaded at boot). */
#ifdef __arm__
#include <kernel/mutex.h>
static mutex_t s_storage_mtx;
static int s_storage_mtx_init;
void ayaneo_storage_lock(void)
{
	if (!s_storage_mtx_init) { mutex_init(&s_storage_mtx); s_storage_mtx_init = 1; }
	mutex_acquire(&s_storage_mtx);
}
void ayaneo_storage_unlock(void)
{
	if (s_storage_mtx_init) mutex_release(&s_storage_mtx);
}
#else
void ayaneo_storage_lock(void) {}
void ayaneo_storage_unlock(void) {}
#endif

static unsigned sd_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
	unsigned r;
	(void)ctx;
#ifdef AYANEO_GBA_SD
	/* Kick the 10s LK watchdog on every SD sector write so a long save (state or
	 * .sav, which fat_wr writes sector-by-sector) does not reset the device mid
	 * save when the power key is pressed in game. */
	{ extern void mtk_wdt_restart(void); mtk_wdt_restart(); }
#endif
	ayaneo_storage_lock();
	r = (unsigned)mmc_wrap_bwrite(SD_DEV_NUM, (unsigned long)lba,
				      (unsigned long)count, buf, SD_PART_USER);
	{	/* the cached copies of these sectors take what was written (dropped if it failed) */
		uint32_t k;
		for (k = 0; k < count; k++) {
			int i = sc_find(lba + k);
			if (i < 0) continue;
			if (r == count) memcpy(s_sc_buf[i], (const uint8_t *)buf + k * 512u, 512);
			else s_sc[i].ok = 0;
		}
	}
	ayaneo_storage_unlock();
	return r;
}

/* Bring up the microSD host (msdc1) once. Returns 0 on success (card present +
 * identified), negative if the slot is empty / init fails. */
static int s_sd_hw_inited;
static int s_sd_hw_rc = 999;      /* last mmc_legacy_init return code (for diagnostics) */
int gba_sd_hw_init(void)
{
	if (s_sd_hw_inited) return 0;
	msdc_ext_sd_power_on();                  /* power the SD LDOs first (LK leaves them off) */
	s_sd_hw_rc = mmc_legacy_init(SD_MMC_VERBOSE);
	if (s_sd_hw_rc != 0) return -1;   /* no card / init failed */
	s_sd_hw_inited = 1;
	return 0;
}

/* Card detect: msdc1 cd-gpios = GPIO8, cd_level 0 (the pin reads 0 with a card inserted,
 * measured on the Konkr). Internal pull up so an empty slot reads 1. Instant, no host init. */
int gba_sd_card_present(void)
{
	extern int mt_get_gpio_in(unsigned int pin);
	extern int mt_set_gpio_mode(unsigned int pin, unsigned int mode);
	extern int mt_set_gpio_dir(unsigned int pin, unsigned int dir);
	extern int mt_set_gpio_pull_enable(unsigned int pin, unsigned int en);
	extern int mt_set_gpio_pull_select(unsigned int pin, unsigned int sel);
	extern void udelay(unsigned long usec);
	const unsigned int cd = 8u | 0x80000000u;
	mt_set_gpio_mode(cd, 0); mt_set_gpio_dir(cd, 0);
	mt_set_gpio_pull_enable(cd, 1); mt_set_gpio_pull_select(cd, 1);
	udelay(200);
	return mt_get_gpio_in(cd) == 0;
}

/* Raw return code of the last microSD host init attempt (mmc error code). */
int gba_sd_hw_rc(void) { return s_sd_hw_rc; }

/* Raw sector read from the microSD (post hw-init), for the fastboot debug probe. */
unsigned gba_sd_bread(uint32_t lba, uint32_t count, void *buf)
{
	unsigned r;
	ayaneo_storage_lock();
	r = (unsigned)mmc_wrap_bread(SD_DEV_NUM, (unsigned long)lba,
				     (unsigned long)count, buf, SD_PART_USER);
	ayaneo_storage_unlock();
	return r;
}

int gba_sd_mount(fat_vol *v)
{
	int rc;
	if (gba_sd_hw_init() != 0) return -5;   /* microSD slot could not be brought up */
	ayaneo_storage_lock();
	sc_clear();   /* a card put in since holds other sectors */
	ayaneo_storage_unlock();
	rc = fat_mount(v, sd_read, 0);
	/* Attach the writer so save/state persistence (fat_wr_put) works on device.
	 * Without this v->wr stays 0 and every write silently returns -1. */
	if (rc == 0) fat_set_writer(v, sd_write);
	return rc;
}

/* Load /gba_bios.bin (must be exactly 16384 bytes) into dst[16384]. Returns 0 on
 * success, negative if missing / wrong size / short read. */
int gba_sd_load_bios(fat_vol *v, unsigned char *dst)
{
	fat_file f;
	/* /bios/gba_bios.bin, or /gba_bios.bin where older cards have it */
	if (fat_open(v, "/bios/gba_bios.bin", &f) != 0 && fat_open(v, "/gba_bios.bin", &f) != 0) return -1;
	if (f.size != 16384u) return -2;
	if (fat_read(&f, 0, dst, 16384u) != 16384u) return -3;
	return 0;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int name_ci_cmp(const char *a, const char *b)
{ while (*a && *b) { char x = lc(*a), y = lc(*b); if (x != y) return x - y; a++; b++; } return lc(*a) - lc(*b); }

/* The systems and the folder names a card may give them. /roms/<first name> is GammaOS's own
 * layout (gba_sd_make_rom_dirs makes it); the others are the names ES-DE, RetroArch playlists
 * and the like use, so a card set up for another front end lists as it is. */
static const struct rom_system {
	unsigned char type;
	const char *dirs;     /* folder names, space separated, matched without case */
	const char *exts;     /* the ROM extensions, space separated */
} k_systems[] = {
	{ GBA_CONSOLE_GB,      "gb gameboy",                         ".gb" },
	{ GBA_CONSOLE_GBC,     "gbc gameboycolor",                   ".gbc" },
	{ GBA_CONSOLE_GBA,     "gba gameboyadvance",                 ".gba" },
	{ GBA_CONSOLE_SNES,    "snes sfc superfamicom snesna",       ".sfc .smc" },
	{ GBA_CONSOLE_GENESIS, "genesis megadrive md megadrivejp",   ".md .gen .bin .smd" },
	{ GBA_CONSOLE_SMS,     "sms mastersystem",                   ".sms" },
	{ GBA_CONSOLE_GG,      "gg gamegear",                        ".gg" },
	{ GBA_CONSOLE_SG,      "sg sg1000 sg-1000",                  ".sg" },
};
#define N_SYSTEMS ((int)(sizeof k_systems / sizeof k_systems[0]))

/* is `name` one of the words of the space separated `list` (without case)? With `suffix`, does
 * it end with one, and how long is that word */
static int word_in(const char *name, const char *list, int suffix)
{
	int L = 0;
	while (name[L]) L++;
	while (*list) {
		int n = 0, i;
		while (list[n] && list[n] != ' ') n++;
		if (suffix ? L > n : L == n) {
			const char *t = name + (suffix ? L - n : 0);
			for (i = 0; i < n && lc(t[i]) == lc(list[i]); i++) ;
			if (i == n) return n;
		}
		list += n;
		while (*list == ' ') list++;
	}
	return 0;
}

/* The folders the games were found in: a game keeps the index of its own (gba_rom_entry.dir),
 * for the box art beside it. Filled again by every listing. */
#define ROM_DIRS 64
static char s_rom_dir[ROM_DIRS][96];
static uint32_t s_rom_dir_id[ROM_DIRS];
static int s_nrom_dir;

const char *gba_sd_rom_dir(const gba_rom_entry *r)
{
	return r && r->dir < s_nrom_dir ? s_rom_dir[r->dir] : "/roms";
}

static void path_join(char *out, int cap, const char *a, const char *b)
{
	int k = 0, i;
	for (i = 0; a[i] && k < cap - 1; i++) out[k++] = a[i];
	if (k < cap - 1) out[k++] = '/';
	for (i = 0; b[i] && k < cap - 1; i++) out[k++] = b[i];
	out[k] = 0;
}

/* The listing's own tables, sized for PICO_GAMES_MAX: a hash of the games listed so far (system
 * and name, for the duplicate check) and the sort's index arrays. Taken once. */
#define DUP_SLOTS 65536u               /* a power of two, twice PICO_GAMES_MAX */
static unsigned short *s_dup;          /* game index + 1, 0 empty */
static unsigned short *s_ord, *s_tmp;
static void *list_mem(unsigned int n)
{
#ifdef __arm__
	return pico_lib_alloc(n);
#else
	extern void *calloc(unsigned long, unsigned long);
	return calloc(1, n);
#endif
}
static int list_tables(void)
{
	if (!s_dup) s_dup = (unsigned short *)list_mem(DUP_SLOTS * 2u);
	if (!s_ord) s_ord = (unsigned short *)list_mem(PICO_GAMES_MAX * 2u);
	if (!s_tmp) s_tmp = (unsigned short *)list_mem(PICO_GAMES_MAX * 2u);
	return s_dup && s_ord && s_tmp;
}
static unsigned int name_hash(unsigned char type, const char *nm)
{
	unsigned int h = 2166136261u ^ type;
	for (; *nm; nm++) h = (h ^ (unsigned char)lc(*nm)) * 16777619u;
	return h;
}
/* the slot of (type, nm) in the duplicate hash: where it is, or the empty one it would take */
static unsigned int dup_slot(const gba_rom_entry *out, unsigned char type, const char *nm)
{
	unsigned int i = name_hash(type, nm) & (DUP_SLOTS - 1u);
	while (s_dup[i]) {
		const gba_rom_entry *r = &out[s_dup[i] - 1u];
		if (r->type == type && name_ci_cmp(r->name, nm) == 0) break;
		i = (i + 1u) & (DUP_SLOTS - 1u);
	}
	return i;
}

/* ---- what the archives hold: an index kept on the card ---------------------------------------
 * To list each ROM of an archive that holds several, the archive is looked into, which with
 * thousands of them would slow every power on. What each one holds is kept in
 * /gammaos/archives.idx (its folder and name, size and first cluster, then the names of its ROMs),
 * so only an archive that is new or changed is read again; the file is written again when
 * anything changed. Records: u32 key, u32 size, u32 first cluster, u16 count (AIX_BAD: it could
 * not be read), then each name as a length byte and its bytes. */
#define AIX_FILE   "archives.idx"
#define AIX_MAX    (2u << 20)
#define AIX_SLOTS  65536u
#define AIX_BAD    0xFFFFu
#define AIX_ONE    1024               /* ROMs taken from one archive */
static uint8_t *s_aix_old, *s_aix_new, *s_aix_names;
static uint32_t *s_aix_slot;          /* record offsets in s_aix_old + 1, 0 empty */
static uint32_t s_aix_old_n, s_aix_new_n;
static int s_aix_changed, s_aix_ok;
volatile unsigned int g_dbg_aix[3];   /* oem diag: archives from the index, looked into, records */

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t aix_key(const char *dir, const char *name)
{
	uint32_t h = 2166136261u;
	for (; *dir; dir++) h = (h ^ (unsigned char)lc(*dir)) * 16777619u;
	h = (h ^ '/') * 16777619u;
	for (; *name; name++) h = (h ^ (unsigned char)lc(*name)) * 16777619u;
	return h;
}
static uint32_t aix_rec_len(const uint8_t *r, const uint8_t *end)
{
	uint32_t n = r[12] | (r[13] << 8), len = 14, i;
	if (n == AIX_BAD) return len;
	for (i = 0; i < n; i++) {
		if (r + len >= end) return 0;
		len += 1u + r[len];
	}
	return r + len <= end ? len : 0;
}
static void aix_begin(fat_vol *v)
{
	fat_file f;
	uint32_t at;
	s_aix_ok = 0;
	if (!s_aix_old) s_aix_old = (uint8_t *)list_mem(AIX_MAX);
	if (!s_aix_new) s_aix_new = (uint8_t *)list_mem(AIX_MAX);
	if (!s_aix_names) s_aix_names = (uint8_t *)list_mem(AIX_ONE * 129u);
	if (!s_aix_slot) s_aix_slot = (uint32_t *)list_mem(AIX_SLOTS * 4u);
	if (!s_aix_old || !s_aix_new || !s_aix_names || !s_aix_slot) return;
	s_aix_ok = 1;
	s_aix_old_n = s_aix_new_n = 8;
	s_aix_changed = 0;
	memset(s_aix_slot, 0, AIX_SLOTS * 4u);
	g_dbg_aix[0] = g_dbg_aix[1] = 0;
	if (fat_open(v, "/gammaos/" AIX_FILE, &f) != 0 || f.size < 8 || f.size > AIX_MAX ||
	    fat_read(&f, 0, s_aix_old, f.size) != f.size || memcmp(s_aix_old, "GAIX", 4) != 0 || rd32le(s_aix_old + 4) != 1) {
		s_aix_changed = 1;   /* none yet, or another form: written afresh */
		return;
	}
	s_aix_old_n = f.size;
	for (at = 8; at + 14 <= s_aix_old_n; ) {	/* each record into the hash, by its key */
		uint32_t len = aix_rec_len(s_aix_old + at, s_aix_old + s_aix_old_n), k;
		if (!len) break;                      /* a cut record: the rest is ignored */
		for (k = rd32le(s_aix_old + at) & (AIX_SLOTS - 1u); s_aix_slot[k]; k = (k + 1u) & (AIX_SLOTS - 1u)) ;
		s_aix_slot[k] = at + 1u;
		at += len;
	}
}
/* the archive's record from the index, when it has not changed since */
static const uint8_t *aix_find(uint32_t key, uint32_t size, uint32_t clus)
{
	uint32_t k;
	if (!s_aix_ok) return 0;
	for (k = key & (AIX_SLOTS - 1u); s_aix_slot[k]; k = (k + 1u) & (AIX_SLOTS - 1u)) {
		const uint8_t *r = s_aix_old + s_aix_slot[k] - 1u;
		if (rd32le(r) == key && rd32le(r + 4) == size && rd32le(r + 8) == clus) return r;
	}
	return 0;
}
/* a record into the new index */
static void aix_put(const uint8_t *rec, uint32_t len)
{
	if (!s_aix_ok || s_aix_new_n + len > AIX_MAX) return;
	memcpy(s_aix_new + s_aix_new_n, rec, len);
	s_aix_new_n += len;
}
static void aix_end(fat_vol *v)
{
	g_dbg_aix[2] = s_aix_ok ? s_aix_new_n : 0;
	if (!s_aix_ok || (!s_aix_changed && s_aix_new_n == s_aix_old_n)) return;
	memcpy(s_aix_new, "GAIX", 4);
	wr32le(s_aix_new + 4, 1);
	if (s_aix_new_n == 8 && s_aix_old_n == 8) return;   /* no archives, and none before */
	fat_wr_mkpath(v, "/gammaos");
	fat_wr_put(v, "/gammaos", AIX_FILE, s_aix_new, s_aix_new_n);
}
/* collecting one archive's ROM names: a record being built in s_aix_names */
struct aix_build { uint8_t *rec; uint32_t len; uint32_t n; };
static void aix_each(void *ctx, const char *name)
{
	struct aix_build *b = (struct aix_build *)ctx;
	uint32_t L = (uint32_t)strlen(name);
	if (b->n >= AIX_ONE) return;
	if (L > 127) L = 127;
	b->rec[b->len] = (uint8_t)L;
	memcpy(b->rec + b->len + 1, name, L);
	b->len += 1 + L;
	b->n++;
}
/* What archive e (in folder dir) holds: its record, from the index or made by looking into it */
static const uint8_t *aix_archive(fat_vol *v, const char *dir, const fat_dirent *e, int arc, const char *exts)
{
	uint32_t key = aix_key(dir, e->name);
	const uint8_t *r = aix_find(key, e->size, e->first_clus);
	struct aix_build b;
	fat_file f;
	int n;
	if (r) { aix_put(r, aix_rec_len(r, s_aix_old + s_aix_old_n)); g_dbg_aix[0]++; return r; }
	if (!s_aix_ok) return 0;
	b.rec = s_aix_names; b.len = 14; b.n = 0;
	f.v = v; f.first_clus = e->first_clus; f.size = e->size; f.ex_nofat = e->ex_nofat;
	n = rom_archive_list(arc, &f, exts, aix_each, &b);
	wr32le(b.rec, key); wr32le(b.rec + 4, e->size); wr32le(b.rec + 8, e->first_clus);
	if (n < 0) { b.len = 14; b.n = AIX_BAD; }
	b.rec[12] = (uint8_t)b.n; b.rec[13] = (uint8_t)(b.n >> 8);
	s_aix_changed = 1;
	g_dbg_aix[1]++;
	aix_put(b.rec, b.len);
	return b.rec;
}

/* one game into out[] (its file e, the name it lists under, the archive it is in and which ROM
 * of it); a game already listed for the system keeps its first place, unless that was an archive
 * and this is the ROM itself (no unpacking). Returns the new count. */
static int add_game(gba_rom_entry *out, int max, int n, int *tot, unsigned char type, int di,
		    const fat_dirent *e, const char *nm, int arc, unsigned member)
{
	unsigned int slot;
	int k;
	if (!nm[0]) return n;
	if (n >= max) { (*tot)++; return n; }   /* past the cap: counted, not checked */
	slot = dup_slot(out, type, nm);
	if (s_dup[slot]) {
		gba_rom_entry *was = &out[s_dup[slot] - 1u];
		if (was->arc && !arc) {
			was->first_clus = e->first_clus; was->size = e->size; was->ex_nofat = e->ex_nofat;
			was->arc = RA_NONE; was->member = 0; was->dir = (unsigned char)di;
		}
		return n;
	}
	for (k = 0; nm[k] && k < 127; k++) out[n].name[k] = nm[k];
	out[n].name[k] = 0;
	out[n].type = type;
	out[n].first_clus = e->first_clus;
	out[n].size = e->size;
	out[n].ex_nofat = e->ex_nofat;
	out[n].dir = (unsigned char)di;
	out[n].arc = (unsigned char)arc;
	out[n].member = (unsigned short)member;
	s_dup[slot] = (unsigned short)(n + 1);
	(*tot)++;
	return n + 1;
}

/* Append the games of system `sys` in folder `path` (cluster `id`) to out[]. A game already
 * listed for the same system (the same name in another folder) is left out, so a card holding
 * both layouts lists each game once, from the folder found first. *tot counts every game, also
 * past the cap, so the caller can tell the list was cut. Returns the new count. */
static int scan_rom_folder(fat_vol *v, const char *path, uint32_t id, const struct rom_system *sys,
			   gba_rom_entry *out, int max, int n, int *tot)
{
	fat_dir d; fat_dirent e;
	int di;
	for (di = 0; di < s_nrom_dir && s_rom_dir_id[di] != id; di++) ;
	if (di < s_nrom_dir) return n;                 /* this folder was listed already */
	if (s_nrom_dir == ROM_DIRS || fat_opendir(v, path, &d) != 0) return n;
	di = s_nrom_dir++;
	s_rom_dir_id[di] = id;
	{ int k; for (k = 0; path[k] && k < 95; k++) s_rom_dir[di][k] = path[k]; s_rom_dir[di][k] = 0; }
	while (fat_readdir(&d, &e)) {
		int el, k, arc = RA_NONE, L = 0;
		char nm[128];
		const uint8_t *rec;
		uint32_t cnt;
		if (e.is_dir || e.size == 0) continue;  /* folders, empty placeholder files */
		if (e.name[0] == '.') continue;         /* macOS "._" resource files and the like */
		el = word_in(e.name, sys->exts, 1);
		if (!el) {	/* games in an archive: a .zip or .7z in the system's folder */
			if ((el = word_in(e.name, ".zip", 1)) != 0) arc = RA_ZIP;
			else if ((el = word_in(e.name, ".7z", 1)) != 0) arc = RA_7Z;
			else continue;
		}
		/* the name without its extension (and an archive's without the ROM's inside it too,
		 * "Game.gba.zip"): the menu's label and the key of its saves, states and box art, the
		 * same as the game unpacked would have (the ROM itself is opened by its cluster) */
		while (e.name[L]) L++;
		L -= el;
		if (L > 127) L = 127;
		for (k = 0; k < L; k++) nm[k] = e.name[k];
		nm[L] = 0;
		if (arc && (el = word_in(nm, sys->exts, 1)) != 0) nm[L -= el] = 0;
		if (!arc) { n = add_game(out, max, n, tot, sys->type, di, &e, nm, RA_NONE, 0); continue; }
		/* an archive: one game under its own name when it holds one ROM (or cannot be read,
		 * then its launch says why), a game for each ROM when it holds several */
		rec = aix_archive(v, path, &e, arc, sys->exts);
		cnt = rec ? (uint32_t)(rec[12] | (rec[13] << 8)) : AIX_BAD;
		if (cnt == AIX_BAD || cnt <= 1) { n = add_game(out, max, n, tot, sys->type, di, &e, nm, arc, 0); continue; }
		{
			const uint8_t *q = rec + 14;
			uint32_t m;
			for (m = 0; m < cnt; m++, q += 1 + q[0]) {
				char rn[128];
				int rl = q[0], st = 0, j;
				for (j = 0; j < rl; j++) if (q[1 + j] == '/' || q[1 + j] == '\\') st = j + 1;   /* its folders inside go */
				for (j = st; j < rl && j - st < 127; j++) rn[j - st] = (char)q[1 + j];
				rn[j - st] = 0;
				if ((el = word_in(rn, sys->exts, 1)) != 0) rn[(j - st) - el] = 0;
				n = add_game(out, max, n, tot, sys->type, di, &e, rn, arc, m);
			}
		}
	}
	return n;
}

/* the system folders straight inside `root` ("" the card's root) */
static int scan_root(fat_vol *v, const char *root, gba_rom_entry *out, int max, int n, int *tot)
{
	fat_dir d; fat_dirent e;
	char path[96];
	int s;
	if (fat_opendir(v, root[0] ? root : "/", &d) != 0) return n;
	while (fat_readdir(&d, &e)) {
		if (!e.is_dir) continue;
		for (s = 0; s < N_SYSTEMS && !word_in(e.name, k_systems[s].dirs, 0); s++) ;
		if (s == N_SYSTEMS) continue;
		path_join(path, sizeof path, root, e.name);
		n = scan_rom_folder(v, path, e.first_clus, &k_systems[s], out, max, n, tot);
	}
	return n;
}

/* folders at the card's root that never hold games: skipped when looking for a ROMs folder */
static int not_a_rom_root(const char *nm)
{
	return nm[0] == '.' || word_in(nm, "gammaos saves states cores bios boxart gba_states", 0) ||
	       name_ci_cmp(nm, "System Volume Information") == 0 || name_ci_cmp(nm, "LOST.DIR") == 0;
}

/* case-insensitive by name, stable: a merge sort of indexes, then the entries moved into that
 * order in place, cycle by cycle (tens of thousands of games, no second copy of the list) */
static void sort_by_name(gba_rom_entry *out, int n)
{
	int w, i;
	for (i = 0; i < n; i++) s_ord[i] = (unsigned short)i;
	for (w = 1; w < n; w *= 2) {
		unsigned short *a = s_ord, *b = s_tmp, *t;
		for (i = 0; i < n; i += 2 * w) {
			int l = i, m = i + w < n ? i + w : n, r = i + 2 * w < n ? i + 2 * w : n, x = l, y = m, k = l;
			while (x < m && y < r) b[k++] = name_ci_cmp(out[a[y]].name, out[a[x]].name) < 0 ? a[y++] : a[x++];
			while (x < m) b[k++] = a[x++];
			while (y < r) b[k++] = a[y++];
		}
		t = s_ord; s_ord = s_tmp; s_tmp = t;
	}
	/* position i takes entry s_ord[i]; s_tmp marks the positions done */
	for (i = 0; i < n; i++) s_tmp[i] = 0;
	for (i = 0; i < n; i++) {
		gba_rom_entry hold;
		int j = i;
		if (s_tmp[i] || s_ord[i] == i) { s_tmp[i] = 1; continue; }
		hold = out[i];
		for (;;) {
			int k = s_ord[j];
			s_tmp[j] = 1;
			if (k == i) { out[j] = hold; break; }
			out[j] = out[k];
			j = k;
		}
	}
}

int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max, int *total)
{
	int n = 0, tot = 0;
	if (total) *total = 0;
	s_nrom_dir = 0;
	if (max <= 0 || !list_tables()) return 0;
	if (max > PICO_GAMES_MAX) max = PICO_GAMES_MAX;
	memset(s_dup, 0, DUP_SLOTS * 2u);
	aix_begin(v);
	/* Where the games may be, in this order (a game in two places is listed from the first):
	 * /roms/<system> (GammaOS), /<system> at the root, and for every other folder at the
	 * root, its system folders and those of its roms folder: /ROMs/<system> (ES-DE),
	 * /Emulation/roms/<system> (EmuDeck), /<anything>/roms/<system>. Folder names are
	 * matched without case. */
	n = scan_root(v, "/roms", out, max, n, &tot);
	n = scan_root(v, "", out, max, n, &tot);
	{
		fat_dir d; fat_dirent e;
		if (fat_opendir(v, "/", &d) == 0) {
			while (fat_readdir(&d, &e)) {
				char top[96], sub[96];
				int s, is_sys = 0;
				if (!e.is_dir || not_a_rom_root(e.name) || name_ci_cmp(e.name, "roms") == 0) continue;
				for (s = 0; s < N_SYSTEMS && !is_sys; s++) is_sys = word_in(e.name, k_systems[s].dirs, 0);
				if (is_sys) continue;           /* /gba and the like: listed above */
				path_join(top, sizeof top, "", e.name);
				n = scan_root(v, top, out, max, n, &tot);
				path_join(sub, sizeof sub, top, "roms");
				n = scan_root(v, sub, out, max, n, &tot);
			}
		}
	}
	aix_end(v);
	if (total) *total = tot;
	sort_by_name(out, n);
	return n;
}

int gba_sd_make_rom_dirs(fat_vol *v)
{
	int rc = 0;
	if (fat_wr_mkpath(v, "/roms/gb")   != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/gbc")  != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/gba")  != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/snes") != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/genesis") != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/sms")  != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/gg")   != 0) rc = -1;
	if (fat_wr_mkpath(v, "/roms/sg")   != 0) rc = -1;
	return rc;
}

/* why the last ROM did not load or start (RA_ERR_*), for the menus' note */
static int s_rom_err;
void gba_rom_set_error(int err) { s_rom_err = err; }
int gba_rom_take_error(void) { int e = s_rom_err; s_rom_err = 0; return e; }
int gba_rom_peek_error(void) { return s_rom_err; }

const char *gba_sd_rom_exts(int type)
{
	int s;
	for (s = 0; s < N_SYSTEMS; s++) if (k_systems[s].type == type) return k_systems[s].exts;
	return "";
}
/* the most ROM each system's core holds (its loader's buffer) */
uint32_t gba_sd_rom_cap(int type)
{
	return type == GBA_CONSOLE_GBA ? 32u << 20 : 8u << 20;   /* GBA 32 MB; the others 8 MB */
}

static void rom_file(fat_vol *v, const gba_rom_entry *r, fat_file *f)
{
	/* reuse the cached chain head. The exFAT contiguous flag must come with it: left unset the
	 * reader follows the FAT, which holds nothing for a contiguous file, and stops after the
	 * first cluster (a GB/GBC game then ran 32 KB of ROM and the rest of the buffer's old data) */
	f->v = v; f->first_clus = r->first_clus; f->size = r->size; f->ex_nofat = r->ex_nofat;
}

/* can the game start: a ROM that fits its system, an archive that holds one we can unpack */
int gba_rom_check(fat_vol *v, const gba_rom_entry *r)
{
	fat_file f;
	uint32_t n = r->size;
	int rc;
	if (!v || !r) return RA_ERR_READ;
	if (r->arc) {
		rom_file(v, r, &f);
		rc = rom_archive_probe(r->arc, &f, gba_sd_rom_exts(r->type), r->member, &n);
		if (rc != RA_OK) return rc;
	}
	if (n == 0) return RA_ERR_DAMAGED;
	return n > gba_sd_rom_cap(r->type) ? RA_ERR_TOOBIG : RA_OK;
}

volatile unsigned int g_dbg_romload[4];   /* oem diag: the last ROM load: ms, bytes, archive kind, result */
static uint32_t load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap);
uint32_t gba_sd_load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap)
{
	extern unsigned gpt4_get_current_tick(void) __attribute__((weak));
	unsigned t0 = gpt4_get_current_tick ? gpt4_get_current_tick() : 0;
	uint32_t n = load_rom(v, r, dst, cap);
	g_dbg_romload[0] = gpt4_get_current_tick ? (gpt4_get_current_tick() - t0) / 13000u : 0;
	g_dbg_romload[1] = n;
	g_dbg_romload[2] = r->arc;
	g_dbg_romload[3] = (unsigned)s_rom_err;
	return n;
}
static uint32_t load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap)
{
	fat_file f;
	uint32_t n = r->size;
	int rc;
	s_rom_err = 0;
	rom_file(v, r, &f);
	if (r->arc) {	/* unpacked straight into the core's ROM memory, its CRC checked */
		/* at the menu's clock: the cores run slower, and the unpacking is the whole wait */
		extern unsigned int ayaneo_get_cpu_mhz(void) __attribute__((weak));
		extern void ayaneo_set_cpu_mhz(unsigned int mhz) __attribute__((weak));
		unsigned int was = ayaneo_get_cpu_mhz ? ayaneo_get_cpu_mhz() : 0;
		if (was && was < 1200 && ayaneo_set_cpu_mhz) ayaneo_set_cpu_mhz(1200);
		rc = rom_archive_load(r->arc, &f, gba_sd_rom_exts(r->type), r->member, dst, cap, &n);
		if (was && was < 1200 && ayaneo_set_cpu_mhz) ayaneo_set_cpu_mhz(was);
		if (rc != RA_OK) { s_rom_err = rc; return 0; }
		return n;
	}
	/* Refuse a ROM that does not fit the arena: a truncated read is a CORRUPT ROM (gpSP would
	 * execute garbage). Return 0 so the caller rejects it and falls back to the selector instead
	 * of booting a broken game. Also reject an empty file. */
	if (n == 0) { s_rom_err = RA_ERR_DAMAGED; return 0; }
	if (n > cap) { s_rom_err = RA_ERR_TOOBIG; return 0; }
	if (fat_read(&f, 0, dst, n) != n) { s_rom_err = RA_ERR_READ; return 0; }   /* a short read is a corrupt ROM too */
	return n;
}

/* the first n bytes of a ROM (its header), unpacked from an archive when it is in one */
uint32_t gba_sd_rom_head(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t n)
{
	fat_file f;
	uint32_t got = 0;
	rom_file(v, r, &f);
	if (r->arc) return rom_archive_head(r->arc, &f, gba_sd_rom_exts(r->type), r->member, dst, n, &got) == RA_OK ? got : 0;
	return fat_read(&f, 0, dst, n < r->size ? n : r->size);
}
