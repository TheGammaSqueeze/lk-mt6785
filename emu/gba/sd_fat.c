/* On-device SD -> FAT glue (LK only). See sd_fat.h. */
#include "sd_fat.h"
#include "fat_wr.h"   /* fat_wr_mkpath for gba_sd_make_rom_dirs */
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
/* case-insensitive suffix match: does name end with the given dotted extension
 * (e.g. ".gb", ".gbc", ".gba")? Both are ASCII; ext must include the leading dot. */
static int ends_ext(const char *n, const char *ext)
{
	int L = 0, E = 0, i;
	while (n[L]) L++;
	while (ext[E]) E++;
	if (L < E + 1) return 0;                    /* need at least one char before the ext */
	{ const char *e = n + L - E;
	  for (i = 0; i < E; i++) if (lc(e[i]) != lc(ext[i])) return 0; }
	return 1;
}

/* Append every <ext> file in <path> to out[], tagged with <type>. n is the count
 * already stored; returns the new count. *tot is bumped for every match (even past
 * the cap) so the caller can detect truncation. A missing folder is not an error. */
static int scan_rom_folder(fat_vol *v, const char *path, const char *ext,
			   unsigned char type, gba_rom_entry *out, int max, int n, int *tot)
{
	fat_dir d; fat_dirent e;
	if (fat_opendir(v, path, &d) != 0) return n;   /* folder absent -> nothing to add */
	while (fat_readdir(&d, &e)) {
		if (e.is_dir || !ends_ext(e.name, ext)) continue;
		if (e.size == 0) continue;              /* skip empty/placeholder files */
		(*tot)++;
		if (n < max) {
			int k = 0, el = 0;
			while (e.name[k] && k < 127) { out[n].name[k] = e.name[k]; k++; }
			out[n].name[k] = 0;
			/* Strip the trailing ".gb/.gbc/.gba" for DISPLAY: the roster name is
			 * only used for the menu label and for sav/state keying (base_ext there
			 * re-strips any extension anyway), and the ROM itself opens by cluster,
			 * so dropping the ext here is safe and gives a clean title. ends_ext
			 * already matched, so the name is guaranteed to end with <ext>. */
			while (ext[el]) el++;
			if (k >= el) out[n].name[k - el] = 0;
			out[n].first_clus = e.first_clus;
			out[n].size = e.size;
			out[n].ex_nofat = e.ex_nofat;
			out[n].type = type;
			n++;
		}
	}
	return n;
}

int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max, int *total)
{
	int n = 0, i, j, tot = 0;
	if (total) *total = 0;
	if (max <= 0) return 0;
	/* Merge all three consoles into one roster; each entry keeps its type so the
	 * menu can badge it and the launcher can pick the right core. */
	n = scan_rom_folder(v, "/roms/gb",   ".gb",   GBA_CONSOLE_GB,   out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/gbc",  ".gbc",  GBA_CONSOLE_GBC,  out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/gba",  ".gba",  GBA_CONSOLE_GBA,  out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/snes", ".sfc",  GBA_CONSOLE_SNES, out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/snes", ".smc",  GBA_CONSOLE_SNES, out, max, n, &tot);
	/* Sega, all via the Genesis-Plus-GX core (one blob) - the type routes the system hint. */
	n = scan_rom_folder(v, "/roms/genesis", ".md",  GBA_CONSOLE_GENESIS, out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/genesis", ".gen", GBA_CONSOLE_GENESIS, out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/genesis", ".bin", GBA_CONSOLE_GENESIS, out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/genesis", ".smd", GBA_CONSOLE_GENESIS, out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/sms",     ".sms", GBA_CONSOLE_SMS, out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/gg",      ".gg",  GBA_CONSOLE_GG,  out, max, n, &tot);
	n = scan_rom_folder(v, "/roms/sg",      ".sg",  GBA_CONSOLE_SG,  out, max, n, &tot);
	if (total) *total = tot;
	for (i = 1; i < n; i++) {           /* insertion sort, case-insensitive by name */
		gba_rom_entry t = out[i];
		for (j = i - 1; j >= 0 && name_ci_cmp(out[j].name, t.name) > 0; j--)
			out[j + 1] = out[j];
		out[j + 1] = t;
	}
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

uint32_t gba_sd_load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap)
{
	fat_file f;
	uint32_t n = r->size;
	/* Refuse a ROM that does not fit the arena: a truncated read is a CORRUPT
	 * ROM (gpSP would execute garbage). Return 0 so the caller rejects it and
	 * falls back to the selector instead of booting a broken game. Also reject
	 * an empty file. */
	if (n == 0 || n > cap) return 0;
	/* reuse the cached chain head. The exFAT contiguous flag must come with it: left unset the
	 * reader follows the FAT, which holds nothing for a contiguous file, and stops after the
	 * first cluster (a GB/GBC game then ran 32 KB of ROM and the rest of the buffer's old data) */
	f.v = v; f.first_clus = r->first_clus; f.size = r->size; f.ex_nofat = r->ex_nofat;
	return fat_read(&f, 0, dst, n) == n ? n : 0u;   /* a short read is a corrupt ROM too */
}
