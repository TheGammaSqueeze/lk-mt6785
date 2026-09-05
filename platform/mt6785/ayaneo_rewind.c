/*
 * Emulator rewind ring buffer, backed by a high-DRAM region we map ourselves.
 *
 * The emulator's normal working memory is the reserved 128MB "scratch" [0x4E000000,0x56000000).
 * Immediately above it the preloader reports a large USABLE, UNRESERVED mblock starting at
 * 0x56000000 (637MB on a 3GB unit; smaller on 2GB SKUs) - free at runtime because this LK runs the
 * emulator instead of booting Android, so the kernel/ramdisk are never loaded. That region is NOT
 * MMU-mapped by default (0x56000000+ faults), so we arch_mmu_map() it Normal-WriteBack and use it as
 * a ring of periodic save-states for rewind. Size is chosen DYNAMICALLY from the mblock map, capped.
 *
 * See analog-input-lk memory + the rewind-research workflow. Ring/capture/rewind added incrementally;
 * this file first proves the dynamic-size + mapping in isolation (ayaneo_rewind_selftest / oem rewindtest).
 */
#include <platform/mt_typedefs.h>
#include <platform/boot_mode.h>
#include <arch/arm/mmu.h>
#include <string.h>
#include <printf.h>

extern BOOT_ARGUMENT *g_boot_arg;

#define REWIND_SCRATCH_END 0x56000000ULL /* end of the 128MB emulator scratch; look above this */
#define REWIND_VA_LIMIT    0x100000000ULL /* LK is 32-bit VA (identity) - can only map phys < 4GB */
#define REWIND_CAP         0x20000000u    /* hard cap 512MB (plenty; leaves huge margin) */
#define REWIND_HEADROOM    0x02000000u    /* leave 32MB below the chosen mblock's end */
#define REWIND_MIN         0x04000000u    /* need at least 64MB to bother */
#define SECT_ALIGN(x)      ((x) & ~(SECTION_SIZE - 1))   /* 2MB-align -> cheap L1 sections, no L2 heap */

static unsigned char *s_base;          /* mapped region base VA (== phys, identity) */
static unsigned int    s_base_phys;    /* chosen physical base */
static unsigned int    s_region;       /* mapped region size (bytes), 0 = not mapped */
static int             s_mapped;
static int             s_tried;

/* Pick the LARGEST usable+unreserved DRAM mblock that sits ABOVE the emulator scratch and inside the
 * 32-bit VA space, 2MB-aligned, size = min(block - headroom, CAP). Purely from the runtime mblock map,
 * so it is per-SKU dynamic AND self-protecting: a region reserved by ADSP/TEE/etc. is never a "usable"
 * mblock, so we can never pick it. Returns 0 (base) if none qualifies. */
static unsigned int rewind_region_find(unsigned int *base_out)
{
	mblock_info_t *mi;
	unsigned int i, best_base = 0, best_size = 0;
	*base_out = 0;
	if (!g_boot_arg)
		return 0;
	mi = &g_boot_arg->mblock_info;
	for (i = 0; i < mi->mblock_num && i < 128u; i++) {
		unsigned long long s = mi->mblock[i].start;
		unsigned long long sz = mi->mblock[i].size;
		unsigned long long abase, end, asz, avail;
		if (s < REWIND_SCRATCH_END || s >= REWIND_VA_LIMIT)
			continue;                                   /* must be above scratch + sub-4GB */
		abase = (s + (SECTION_SIZE - 1)) & ~((unsigned long long)SECTION_SIZE - 1);  /* 2MB up */
		end = s + sz;
		if (end > REWIND_VA_LIMIT)
			end = REWIND_VA_LIMIT;
		if (abase >= end)
			continue;
		asz = end - abase;
		avail = (asz > REWIND_HEADROOM) ? (asz - REWIND_HEADROOM) : 0;
		if (avail > REWIND_CAP)
			avail = REWIND_CAP;
		avail = SECT_ALIGN((unsigned int)avail);
		if (avail >= REWIND_MIN && (unsigned int)avail > best_size) {
			best_size = (unsigned int)avail;
			best_base = (unsigned int)abase;
		}
	}
	*base_out = best_base;
	return best_size;
}

/* Map the rewind region Normal-WriteBack (identity, once). Returns the mapped size, 0 on failure. */
unsigned int ayaneo_rewind_map(void)
{
	unsigned int sz, base;
	if (s_mapped)
		return s_region;
	if (s_tried)
		return 0;
	s_tried = 1;
	sz = rewind_region_find(&base);
	if (!sz || !base)
		return 0;
	if (arch_mmu_map((uint64_t)base, (vaddr_t)base,
			 MMU_MEMORY_TYPE_NORMAL_WRITE_BACK | MMU_MEMORY_AP_P_RW_U_NA, sz) != 0)
		return 0;
	s_base = (unsigned char *)(addr_t)base;
	s_base_phys = base;
	s_region = sz;
	s_mapped = 1;
	return sz;
}

unsigned int ayaneo_rewind_phys(void) { return s_base_phys; }

unsigned char *ayaneo_rewind_base(void) { return s_base; }
unsigned int   ayaneo_rewind_region(void) { return s_region; }

/* Isolation selftest: map the region, write a position-dependent pattern every 1MB across the WHOLE
 * region, read it back. If the map faulted or the region overlapped something live we would crash or
 * mismatch. region/tested/bad reported; returns 0 iff all read back correctly. */
int ayaneo_rewind_selftest(unsigned int *region_out, unsigned int *tested_out, unsigned int *bad_out)
{
	unsigned int sz = ayaneo_rewind_map();
	unsigned int off, tested = 0, bad = 0;
	*region_out = sz;
	*tested_out = 0;
	*bad_out = 0;
	if (!sz)
		return -1;
	for (off = 0; off + 4u <= sz; off += 0x100000u) {
		*(volatile unsigned int *)(s_base + off) = 0xA5A50000u ^ off;
		tested++;
	}
	for (off = 0; off + 4u <= sz; off += 0x100000u) {
		if (*(volatile unsigned int *)(s_base + off) != (0xA5A50000u ^ off))
			bad++;
	}
	*tested_out = tested;
	*bad_out = bad;
	return bad ? -1 : 0;
}
