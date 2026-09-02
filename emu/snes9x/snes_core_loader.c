/*
 * snes_core_loader.c - LK side of the loadable snes9x core (see snes_core_abi.h).
 *
 * Compiled INTO lk_a. snes_core_load() reads the SNES core blob from boot_b into its fixed
 * DRAM slot (0x4F000000), zeroes BSS, does the D-clean + I-invalidate the other core
 * loaders use for freshly written code, then calls the blob entry with the imports table
 * and returns the export table. Mirrors emu/gbc/gbc_core_loader.c.
 */
#include "snes_core_abi.h"

typedef long ssize_t;
typedef long long off_t;   /* MUST match include/sys/types.h (64-bit) */
extern ssize_t partition_read(const char *part, off_t off, unsigned char *buf, unsigned long len);
extern void *memset(void *d, int c, unsigned long n);

/* imports the core calls back into LK for */
extern unsigned ayaneo_snes_pad_mask(void);   /* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits */
extern long gba_host_time(void);               /* wall-clock seconds for SRTC / BS-X carts */

#define SNES_CORE_BLOB_PART "boot_b"
#define SNES_CORE_BLOB_OFF  0x01E00000u   /* 31.5 MB: after the gpSP blob (~30.4 MB), inside
                                           * the 33 MB boot_b partition (ends 0x02100000) */
#define SNES_CORE_BLOB_VMA  0x4F000000u

#define CORE_CACHE_LINE 32u
static void core_sync_code(unsigned long start, unsigned long len)
{
	unsigned long s = start & ~(CORE_CACHE_LINE - 1);
	unsigned long e = start + len;
	unsigned long a;
	for (a = s; a < e; a += CORE_CACHE_LINE)
		__asm__ __volatile__("mcr p15, 0, %0, c7, c11, 1" :: "r"(a) : "memory"); /* DCCMVAU */
	__asm__ __volatile__("dsb" ::: "memory");
	for (a = s; a < e; a += CORE_CACHE_LINE)
		__asm__ __volatile__("mcr p15, 0, %0, c7, c5, 1" :: "r"(a) : "memory");  /* ICIMVAU */
	__asm__ __volatile__("mcr p15, 0, %0, c7, c5, 6" :: "r"(0) : "memory");          /* BPIALL */
	__asm__ __volatile__("dsb\n\tisb" ::: "memory");
}

static const struct snes_core_imports s_imports = {
	ayaneo_snes_pad_mask,
	gba_host_time,
};

/* diagnostics for `fastboot oem diag` (why did the blob load fail?) */
volatile unsigned g_snes_dbg_loaderr;   /* 1 hdr read, 2 magic, 3 hdr sizes, 4 blob read, 5 init */
volatile unsigned g_snes_dbg_hdr0;      /* magic word actually read from boot_b */
volatile int      g_snes_dbg_prc;       /* partition_read return of the header */

/* Load the SNES core blob and return its export table, or 0 on any failure. */
const struct snes_core_exports *snes_core_load(void)
{
	unsigned char *dst = (unsigned char *)SNES_CORE_BLOB_VMA;
	unsigned hdr[5];
	unsigned entry, load_size, total_span;
	snes_core_blob_init_fn init;
	const struct snes_core_exports *ex;
	ssize_t prc;

	prc = partition_read(SNES_CORE_BLOB_PART, SNES_CORE_BLOB_OFF, (unsigned char *)hdr, sizeof hdr);
	g_snes_dbg_prc = (int)prc;
	g_snes_dbg_hdr0 = hdr[0];
	if (prc != (ssize_t)sizeof hdr) { g_snes_dbg_loaderr = 1; return 0; }
	if (hdr[0] != SNES_CORE_ABI_MAGIC || hdr[1] != SNES_CORE_ABI_VERSION) { g_snes_dbg_loaderr = 2; return 0; }
	entry = hdr[2]; load_size = hdr[3]; total_span = hdr[4];
	if (load_size < sizeof hdr || total_span < load_size) { g_snes_dbg_loaderr = 3; return 0; }

	if (partition_read(SNES_CORE_BLOB_PART, SNES_CORE_BLOB_OFF, dst, load_size) != (ssize_t)load_size) { g_snes_dbg_loaderr = 4; return 0; }
	memset(dst + load_size, 0, total_span - load_size);   /* zero BSS */
	core_sync_code(SNES_CORE_BLOB_VMA, load_size);

	init = (snes_core_blob_init_fn)(void *)(dst + entry);
	ex = init(&s_imports);
	if (!ex || ex->magic != SNES_CORE_ABI_MAGIC || ex->version != SNES_CORE_ABI_VERSION) { g_snes_dbg_loaderr = 5; return 0; }
	g_snes_dbg_loaderr = 0;
	return ex;
}
