/*
 * gba_core_loader.c - LK side of the loadable core (see gba_core_abi.h).
 *
 * Compiled INTO lk_a (not the blob). gba_core_load() reads the core blob from boot_b into
 * its fixed DRAM slot, zeroes its BSS, does the same D-clean + I-invalidate the dynarec
 * uses for RWX code, then calls the blob's entry with the imports table and returns the
 * export table the driver drives the core through. Replaces linking libgpsp.a into lk_a,
 * freeing ~993 KiB of the 2 MB partition and making room for a second core (GB/GBC).
 */
#include "gba_core_abi.h"

typedef long ssize_t;
typedef long long off_t;	/* MUST match include/sys/types.h: 64-bit, or the ABI mismatch
				 * mis-passes the offset register pair and the read fails silently. */
extern ssize_t partition_read(const char *part, off_t off, unsigned char *buf, unsigned long len);
extern void *memset(void *d, int c, unsigned long n);

/* imports the blob calls back into LK for */
extern void ayaneo_gba_audio_submit(const short *interleaved, unsigned frames);
extern long gba_host_time(void);
extern void gba_yield_to_main(void);

/* Where the blob lives. VMA must match core_blob.ld; the boot_b offset is chosen by the
 * boot_b builder (boot_b is being repurposed to LK/menu assets + core blobs now that SD
 * is the source of truth for ROMs/saves/config). */
#define GBA_CORE_BLOB_PART "boot_b"
#define GBA_CORE_BLOB_OFF  0x01C00000u   /* 28 MB: the old save/state region, dead in SD
                                          * mode (saves go to the SD card); clear of the
                                          * SNES asset pack (17-~22 MB) and settings (30 MB) */
#define GBA_CORE_BLOB_VMA  0x4E400000u

/* clean D-line to PoU, DSB, invalidate I-line to PoU, BPIALL, DSB, ISB - over the loaded
 * code so the CPU fetches it correctly (same as gba_shim.c __clear_cache, but that lives
 * in the blob and is not available until after this runs). Single-core PL1: local ops. */
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

static const struct gba_core_imports s_imports = {
	ayaneo_gba_audio_submit,
	gba_host_time,
	gba_yield_to_main,
};

/* Load the GBA core blob and return its export table, or 0 on any failure (bad header,
 * short read, entry rejected). Called once during emu init before gba_core_init(). */
const struct gba_core_exports *gba_core_load(void)
{
	unsigned char *dst = (unsigned char *)GBA_CORE_BLOB_VMA;
	unsigned hdr[5];
	unsigned entry, load_size, total_span;
	gba_core_blob_init_fn init;
	const struct gba_core_exports *ex;

	if (partition_read(GBA_CORE_BLOB_PART, GBA_CORE_BLOB_OFF, (unsigned char *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
		return 0;
	if (hdr[0] != GBA_CORE_ABI_MAGIC || hdr[1] != GBA_CORE_ABI_VERSION)
		return 0;
	entry = hdr[2]; load_size = hdr[3]; total_span = hdr[4];
	if (load_size < sizeof hdr || total_span < load_size)
		return 0;

	/* pull the whole loadable image (header..data) into the fixed DRAM slot */
	if (partition_read(GBA_CORE_BLOB_PART, GBA_CORE_BLOB_OFF, dst, load_size) != (ssize_t)load_size)
		return 0;
	/* zero BSS [load_size, total_span) */
	memset(dst + load_size, 0, total_span - load_size);
	/* make the freshly written code fetchable */
	core_sync_code(GBA_CORE_BLOB_VMA, load_size);

	init = (gba_core_blob_init_fn)(void *)(dst + entry);
	ex = init(&s_imports);
	if (!ex || ex->magic != GBA_CORE_ABI_MAGIC || ex->version != GBA_CORE_ABI_VERSION)
		return 0;
	return ex;
}
