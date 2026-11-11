/*
 * core_file.c - loading an emulator core from the SD card into its fixed DRAM slot (see
 * core_file.h). Replaces the boot_b copies: the LK owns only lk_a on the eMMC now.
 */
#include "core_file.h"

extern void *memset(void *d, int c, unsigned long n);

/* clean the D-cache and invalidate the I-cache over the freshly written code, so the CPU fetches
 * what was just copied (single core, PL1: local maintenance to the point of unification) */
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

void *core_file_load(fat_vol *v, const char *path, unsigned vma, unsigned slot_size,
		     unsigned magic, unsigned ver_lo, unsigned ver_hi, int *err)
{
	unsigned char *dst = (unsigned char *)vma;
	unsigned hdr[5];
	fat_file f;
	uint32_t len = 0;
	int e = CORE_FILE_OK;

	if (!v)
		e = CORE_FILE_NO_CARD;
	else if (fat_open(v, path, &f) != 0)
		e = CORE_FILE_MISSING;
	else if (f.size < sizeof hdr || fat_read(&f, 0, hdr, sizeof hdr) != sizeof hdr ||
		 hdr[0] != magic || hdr[1] < ver_lo || hdr[1] > ver_hi)
		e = CORE_FILE_HEADER;
	/* entry inside the image, the BSS after it, all inside the slot. A file may stop up to 16 bytes
	 * short of the image: the linker pads the image's end to 16, and objcopy drops a pad that is
	 * outside every section; those bytes are zero, as the BSS is. */
	else if (hdr[3] < sizeof hdr || hdr[2] < sizeof hdr || hdr[2] >= hdr[3] || hdr[4] < hdr[3] ||
		 hdr[4] > slot_size || f.size + 16u < hdr[3] || f.size <= hdr[2])
		e = CORE_FILE_SIZE;
	else {
		len = (f.size < hdr[3]) ? f.size : hdr[3];
		if (fat_read(&f, 0, dst, len) != len)
			e = CORE_FILE_READ;
	}
	if (err)
		*err = e;
	if (e != CORE_FILE_OK)
		return 0;
	memset(dst + len, 0, hdr[4] - len);
	core_sync_code(vma, hdr[3]);
	return dst + hdr[2];
}

const char *core_file_error(int err)
{
	switch (err) {
	case CORE_FILE_OK:       return "ok";
	case CORE_FILE_NO_CARD:  return "no card";
	case CORE_FILE_MISSING:  return "core file missing";
	case CORE_FILE_HEADER:   return "not this core, or wrong version";
	case CORE_FILE_SIZE:     return "core file damaged";
	case CORE_FILE_READ:     return "card read failed";
	case CORE_FILE_REJECTED: return "core rejected";
	}
	return "?";
}
