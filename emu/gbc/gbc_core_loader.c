/*
 * gbc_core_loader.c - LK side of the loadable gambatte core (see gbc_core_abi.h).
 *
 * Compiled INTO lk_a. gbc_core_load() reads the GB/GBC core blob from boot_b into its
 * fixed DRAM slot (0x4E800000), zeroes its BSS, does the same D-clean + I-invalidate the
 * gpSP loader uses for freshly written code, then calls the blob's entry with the imports
 * table and returns the export table the driver drives the core through. Mirrors
 * emu/gba/gba_core_loader.c.
 */
#include "gbc_core_abi.h"

typedef long ssize_t;
typedef long long off_t;	/* MUST match include/sys/types.h (64-bit) or the offset
				 * register pair is mis-passed and the read fails silently. */
extern ssize_t partition_read(const char *part, off_t off, unsigned char *buf, unsigned long len);
extern void *memset(void *d, int c, unsigned long n);

/* imports the core calls back into LK for */
extern unsigned ayaneo_gbc_pad_mask(void);   /* physical pad -> gambatte button bits */
extern long gba_host_time(void);             /* wall-clock seconds for the cart RTC */

#define GBC_CORE_BLOB_PART "boot_b"
#define GBC_CORE_BLOB_OFF  0x01900000u   /* 25 MB: in the free gap between the SNES pack
                                          * (ends ~21 MB) and the gpSP blob (28 MB) */
#define GBC_CORE_BLOB_VMA  0x4E800000u

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

static const struct gbc_core_imports s_imports = {
	ayaneo_gbc_pad_mask,
	gba_host_time,
};

/* Load the GB/GBC core blob and return its export table, or 0 on any failure. */
const struct gbc_core_exports *gbc_core_load(void)
{
	unsigned char *dst = (unsigned char *)GBC_CORE_BLOB_VMA;
	unsigned hdr[5];
	unsigned entry, load_size, total_span;
	gbc_core_blob_init_fn init;
	const struct gbc_core_exports *ex;

	if (partition_read(GBC_CORE_BLOB_PART, GBC_CORE_BLOB_OFF, (unsigned char *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
		return 0;
	if (hdr[0] != GBC_CORE_ABI_MAGIC || hdr[1] != GBC_CORE_ABI_VERSION)
		return 0;
	entry = hdr[2]; load_size = hdr[3]; total_span = hdr[4];
	if (load_size < sizeof hdr || total_span < load_size)
		return 0;

	if (partition_read(GBC_CORE_BLOB_PART, GBC_CORE_BLOB_OFF, dst, load_size) != (ssize_t)load_size)
		return 0;
	memset(dst + load_size, 0, total_span - load_size);   /* zero BSS */
	core_sync_code(GBC_CORE_BLOB_VMA, load_size);

	init = (gbc_core_blob_init_fn)(void *)(dst + entry);
	ex = init(&s_imports);
	if (!ex || ex->magic != GBC_CORE_ABI_MAGIC || ex->version != GBC_CORE_ABI_VERSION)
		return 0;
	return ex;
}
