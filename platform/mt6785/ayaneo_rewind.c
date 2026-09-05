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
#define REWIND_CAP         0x3E000000u    /* ~992MB. The old 512MB cap tied every high mblock at 512MB,
					   * so region_find picked the FIRST (mblk4 @0x56000000); a higher cap
					   * lets it select the largest island (mblk10 ~990MB @0xC0000000 on a
					   * 3GB unit) uncapped, ~2x the rewind window. block-headroom stays the
					   * binding limit, so smaller SKUs still scale down safely. */
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

/* =========================================================================
 * Ring of periodic save-states for rewind.
 *
 * The mapped region is sliced into fixed-size slots; each slot = [u32 size][payload...]. Capture
 * pushes the current state (oldest overwritten when full). Rewind walks a cursor backward through
 * the stored slots; on release the cursor becomes the new head (the "future" states past the cursor
 * are discarded and future captures continue from there). The core packs whatever it needs into a
 * slot (e.g. GBA = machine state + sound ring). All indices are plain ints - no locking needed since
 * capture and rewind are mutually exclusive and both run on the emu thread.
 * ========================================================================= */
#define SLOT_HDR   16u                 /* 4-byte size + 12 pad -> 16B header, keeps payload 16-aligned */

static unsigned int s_slot_sz;         /* bytes per slot (hdr + payload), 0 = ring not ready */
static unsigned int s_nslots;
static unsigned int s_head;            /* next capture slot */
static unsigned int s_count;           /* valid stored slots (<= nslots) */
static unsigned int s_cursor;          /* rewind: current slot */
static unsigned int s_back;            /* rewind: steps back from newest */
static int          s_rewinding;

static unsigned char *slot_ptr(unsigned int i) { return s_base + (unsigned long long)i * s_slot_sz; }

/* (Re)initialise the ring for a per-core max payload. Maps the region on first use. Clears history.
 * Call at session start and whenever the timeline is invalidated (game switch / reset / load-state /
 * menu-exit is fine to leave). Returns the number of slots (0 = rewind unavailable on this SKU). */
unsigned int ayaneo_rewind_reset(unsigned int max_payload)
{
	unsigned int region = ayaneo_rewind_map();
	s_head = s_count = s_cursor = s_back = 0;
	s_rewinding = 0;
	if (!region || !max_payload) { s_slot_sz = 0; s_nslots = 0; return 0; }
	s_slot_sz = (SLOT_HDR + max_payload + 63u) & ~63u;
	s_nslots  = region / s_slot_sz;
	return s_nslots;
}

int ayaneo_rewind_ready(void)  { return s_slot_sz != 0 && s_nslots != 0; }
int ayaneo_rewind_active(void) { return s_rewinding; }
unsigned int ayaneo_rewind_slots(void) { return s_nslots; }
unsigned int ayaneo_rewind_count(void) { return s_count; }

/* Capture: get the payload pointer to write the current state into (up to max_payload), then commit
 * the actual byte count. Returns NULL if the ring is not ready. Not valid while rewinding. */
void *ayaneo_rewind_capture_begin(void)
{
	if (!ayaneo_rewind_ready() || s_rewinding) return 0;
	return slot_ptr(s_head) + SLOT_HDR;
}
void ayaneo_rewind_capture_commit(unsigned int size)
{
	if (!ayaneo_rewind_ready() || s_rewinding) return;
	if (size > s_slot_sz - SLOT_HDR) size = s_slot_sz - SLOT_HDR;   /* never record past the slot */
	*(volatile unsigned int *)slot_ptr(s_head) = size;
	s_head = (s_head + 1u) % s_nslots;
	if (s_count < s_nslots) s_count++;   /* else oldest is overwritten */
}

/* Rewind: enter (cursor = newest state), step back one stored state (clamped at oldest), read the
 * current cursor state, and exit (commit cursor as the new head, dropping the newer states). */
int ayaneo_rewind_begin(void)
{
	if (!ayaneo_rewind_ready() || s_count == 0) return -1;
	s_rewinding = 1;
	s_back = 0;
	s_cursor = (s_head + s_nslots - 1u) % s_nslots;   /* newest */
	return 0;
}
int ayaneo_rewind_step(void)
{
	if (!s_rewinding) return -1;
	if (s_back + 1u >= s_count) return -1;             /* already at oldest */
	s_back++;
	s_cursor = (s_head + s_nslots - 1u - s_back) % s_nslots;
	return 0;
}
const void *ayaneo_rewind_cur(unsigned int *size_out)
{
	if (!ayaneo_rewind_ready() || !s_rewinding) { if (size_out) *size_out = 0; return 0; }
	if (size_out) *size_out = *(volatile unsigned int *)slot_ptr(s_cursor);
	return slot_ptr(s_cursor) + SLOT_HDR;
}
void ayaneo_rewind_end(void)
{
	if (!s_rewinding) return;
	s_head  = (s_cursor + 1u) % s_nslots;   /* resume from the rewound point */
	s_count = s_count - s_back;             /* drop the discarded newer states */
	s_back = 0;
	s_rewinding = 0;
}

/* Ring-logic selftest (no core): push `n` slots each stamped with a marker, rewind all the way,
 * verify the markers come back newest->oldest, then exit and confirm the head resumes correctly. */
int ayaneo_rewind_ring_selftest(unsigned int *slots_out, unsigned int *pushed_out, unsigned int *bad_out)
{
	unsigned int i, bad = 0, n;
	if (!ayaneo_rewind_reset(4096u)) { *slots_out = 0; *pushed_out = 0; *bad_out = 0; return -1; }
	*slots_out = s_nslots;
	n = s_nslots + 5u;                       /* overflow the ring to exercise wraparound */
	for (i = 0; i < n; i++) {
		unsigned int *p = (unsigned int *)ayaneo_rewind_capture_begin();
		if (!p) { bad++; continue; }
		p[0] = 0xBEEF0000u | (i & 0xffff);   /* marker = push index */
		ayaneo_rewind_capture_commit(4096u);
	}
	*pushed_out = n;
	/* newest should be marker (n-1), then n-2, ... down to (n - count) */
	if (ayaneo_rewind_begin() == 0) {
		unsigned int expect = n - 1u, steps = 0;
		for (;;) {
			unsigned int sz; const unsigned int *p = (const unsigned int *)ayaneo_rewind_cur(&sz);
			if (!p || sz != 4096u || p[0] != (0xBEEF0000u | (expect & 0xffff))) bad++;
			if (ayaneo_rewind_step() != 0) break;
			expect--; steps++;
			if (steps > s_nslots) { bad++; break; }   /* runaway guard */
		}
		ayaneo_rewind_end();
	} else bad++;
	*bad_out = bad;
	ayaneo_rewind_reset(0);                  /* leave the ring clean/disabled */
	return bad ? -1 : 0;
}
