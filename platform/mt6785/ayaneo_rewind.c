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
 * DELTA ring of periodic save-states for rewind.
 *
 * Consecutive save-states are ~identical, so instead of storing each full state we store the
 * XOR-delta from the previous state, run-length-encoded (XOR of two near-identical states is mostly
 * zero -> the RLE crushes it). Because rewind walks strictly BACKWARD one frame at a time and XOR is
 * its own inverse, reconstruction is a single delta apply per step (recon ^= delta_T = state_{T-1})
 * with NO keyframes needed - only a "base" flag on the first post-reset record marks the floor (its
 * delta is state XOR 0 = the whole state, which must never be applied during the walk).
 *
 * The public API is UNCHANGED, so the per-core loops don't change: capture_begin() returns a raw
 * staging buffer the core state_saves into; commit() deltas+RLEs it into the arena. begin()/step()
 * reconstruct into the same buffer; cur() hands the raw reconstructed state back to state_load.
 *
 * Storage: a byte arena (records may wrap the arena boundary; all record bytes are 4-aligned so a
 * u32 never straddles) plus a circular directory of {off,len,flags}. Records are evicted oldest-first
 * when the arena or directory fills. All state is plain scalars - capture and rewind are mutually
 * exclusive on the single emu thread, no locking.
 * ========================================================================= */
#define REC_DIR_CAP  262144u           /* max stored frames (directory entries); 3MB dir */
#define REC_BASE     1u                /* flags bit: first-after-reset record (delta vs zero) = floor */

struct rw_rec { unsigned int off; unsigned int len; unsigned int flags; };

static unsigned int   s_state_sz;      /* raw state bytes (word-rounded); 0 = ring not ready */
static unsigned int   s_state_words;
static unsigned int  *s_prev;          /* previous raw state (XOR base for the next capture) */
static unsigned int  *s_recon;         /* capture staging AND rewind reconstruction buffer */
static unsigned char *s_encbuf;        /* RLE encode scratch (one delta) */
static struct rw_rec *s_dir;           /* circular record directory */
static unsigned char *s_arena;         /* record byte arena */
static unsigned int   s_arena_sz;      /* arena bytes (multiple of 4) */
static unsigned int   s_wr;            /* arena write offset (4-aligned) */
static unsigned int   s_used;          /* live arena bytes */
static unsigned int   s_dir_head;      /* next directory slot (newest = head-1) */
static unsigned int   s_dir_count;     /* valid records */
static int            s_have_base;     /* the floor (base) record is still present */
static int            s_have_prev;     /* s_prev holds a valid previous state */
static unsigned int   s_back;          /* rewind: deltas applied from newest */
static int            s_rewinding;

/* RLE-encode (a XOR b) over nwords into out; b==0 => encode a itself. Token = [u32 gap][u32 lit]
 * [lit u32 xor-words]: skip `gap` unchanged words, then `lit` changed words. Returns byte length
 * (always a multiple of 4), 0 if it would exceed outcap (caller sizes outcap = state + slack). */
static unsigned int xor_rle_encode(unsigned char *out, unsigned int outcap,
				   const unsigned int *a, const unsigned int *b, unsigned int nwords)
{
	unsigned int i = 0, olen = 0;
	while (i < nwords) {
		unsigned int gap = 0, lit = 0, ls, k;
		while (i < nwords && (a[i] ^ (b ? b[i] : 0u)) == 0u) { gap++; i++; }
		ls = i;
		while (i < nwords && (a[i] ^ (b ? b[i] : 0u)) != 0u) { lit++; i++; }
		if (olen + 8u + lit * 4u > outcap) return 0u;
		*(unsigned int *)(out + olen) = gap; *(unsigned int *)(out + olen + 4u) = lit; olen += 8u;
		for (k = 0; k < lit; k++) { *(unsigned int *)(out + olen) = a[ls + k] ^ (b ? b[ls + k] : 0u); olen += 4u; }
	}
	return olen;
}

/* Apply an RLE'd delta stored in the arena at byte offset `off` (len bytes, may wrap) INTO recon:
 * recon ^= expand(delta). Every u32 is 4-aligned so it never straddles the wrap. Fast path for the
 * common non-wrapping record (plain pointer walk); modular access only for a record that wraps. */
static void xor_rle_apply(unsigned int *recon, unsigned int off, unsigned int len, unsigned int nwords)
{
	unsigned int w = 0;
	if (off + len <= s_arena_sz) {                     /* fast path: record is contiguous */
		const unsigned char *d = s_arena + off, *e = d + len;
		while (d + 8u <= e && w < nwords) {
			unsigned int gap = *(const unsigned int *)d; d += 4;
			unsigned int lit = *(const unsigned int *)d; d += 4;
			unsigned int k;
			w += gap;
			for (k = 0; k < lit && w < nwords; k++) { recon[w] ^= *(const unsigned int *)d; d += 4; w++; }
		}
		return;
	}
	{                                                  /* slow path: record wraps the arena end */
		unsigned int p = off, consumed = 0;
		while (consumed + 8u <= len && w < nwords) {
			unsigned int gap = *(unsigned int *)(s_arena + (p % s_arena_sz)); p += 4u;
			unsigned int lit = *(unsigned int *)(s_arena + (p % s_arena_sz)); p += 4u;
			unsigned int k;
			consumed += 8u;
			w += gap;
			for (k = 0; k < lit && w < nwords; k++) {
				recon[w] ^= *(unsigned int *)(s_arena + (p % s_arena_sz));
				p += 4u; consumed += 4u; w++;
			}
		}
	}
}

/* (Re)initialise the ring for a per-core max payload. Maps the region on first use. Clears history.
 * Call at session start and on every timeline break (reset / load-state / game switch). Returns a
 * capacity indicator (0 = rewind unavailable on this SKU). */
unsigned int ayaneo_rewind_reset(unsigned int max_payload)
{
	unsigned int region = ayaneo_rewind_map();
	unsigned int pstride, estride, dirbytes, hdr;
	unsigned char *b = s_base;
	s_wr = s_used = s_dir_head = s_dir_count = s_back = 0;
	s_rewinding = 0; s_have_base = 0; s_have_prev = 0;
	if (!region || !max_payload) { s_state_sz = 0; s_state_words = 0; return 0; }
	s_state_sz    = (max_payload + 3u) & ~3u;               /* word-round the raw state */
	s_state_words = s_state_sz / 4u;
	pstride  = (s_state_sz + 63u) & ~63u;                   /* prev, recon: one state each */
	estride  = (2u * s_state_sz + 4096u + 63u) & ~63u;      /* encbuf: worst-case delta + slack */
	dirbytes = REC_DIR_CAP * (unsigned int)sizeof(struct rw_rec);
	hdr = 2u * pstride + estride + dirbytes;                /* prev + recon + encbuf + directory */
	if (region <= hdr + REWIND_MIN) { s_state_sz = 0; s_state_words = 0; return 0; }
	s_prev   = (unsigned int *)(b + 0u * pstride);
	s_recon  = (unsigned int *)(b + 1u * pstride);
	s_encbuf = (unsigned char *)(b + 2u * pstride);
	s_dir    = (struct rw_rec *)(b + 2u * pstride + estride);
	s_arena  = b + hdr;
	s_arena_sz = (region - hdr) & ~3u;
	return REC_DIR_CAP;
}

int ayaneo_rewind_ready(void)  { return s_state_sz != 0; }
int ayaneo_rewind_active(void) { return s_rewinding; }
unsigned int ayaneo_rewind_slots(void) { return s_state_sz ? REC_DIR_CAP : 0u; }
unsigned int ayaneo_rewind_count(void) { return s_dir_count; }

/* Live delta-ring stats for the current session (records stored, arena bytes used, arena size, raw
 * per-state bytes). records/60 = seconds of rewind; used/records = average delta size = how well the
 * game's states delta-compress (small = long window). */
void ayaneo_rewind_stat(unsigned int *records, unsigned int *used, unsigned int *arena, unsigned int *state)
{
	if (records) *records = s_dir_count;
	if (used)    *used    = s_used;
	if (arena)   *arena   = s_arena_sz;
	if (state)   *state   = s_state_sz;
}

/* Evict the oldest record (free its arena bytes + directory slot). */
static void rw_evict_oldest(void)
{
	unsigned int tail = (s_dir_head - s_dir_count) % REC_DIR_CAP;
	if (s_dir[tail].flags & REC_BASE) s_have_base = 0;   /* floor gone: all survivors are deltas */
	s_used -= s_dir[tail].len;
	s_dir_count--;
}

/* Capture: hand back the raw staging buffer, then commit the actual state size. The commit computes
 * the XOR-delta vs the previous state, RLE-encodes it, and appends it (evicting oldest as needed). */
void *ayaneo_rewind_capture_begin(void)
{
	if (!ayaneo_rewind_ready() || s_rewinding) return 0;
	return s_recon;
}
void ayaneo_rewind_capture_commit(unsigned int size)
{
	unsigned int len, i, flags = 0;
	if (!ayaneo_rewind_ready() || s_rewinding) return;
	(void)size;                                          /* state size is fixed at reset (s_state_sz) */
	if (!s_have_prev) { flags = REC_BASE; }              /* first after reset: delta vs zero = floor */
	len = xor_rle_encode(s_encbuf, 2u * s_state_sz + 4096u,
			     s_recon, s_have_prev ? s_prev : 0, s_state_words);
	if (len == 0u || len > s_arena_sz) {                 /* incompressible past the arena: skip frame */
		for (i = 0; i < s_state_words; i++) s_prev[i] = s_recon[i];
		s_have_prev = 1;
		return;
	}
	while ((s_used + len > s_arena_sz || s_dir_count >= REC_DIR_CAP) && s_dir_count > 0)
		rw_evict_oldest();
	/* write the record at s_wr, wrapping the arena boundary (bytes stay 4-aligned) */
	{
		unsigned int first = s_arena_sz - s_wr;
		if (first >= len) {
			for (i = 0; i < len; i++) s_arena[s_wr + i] = s_encbuf[i];
		} else {
			for (i = 0; i < first; i++) s_arena[s_wr + i] = s_encbuf[i];
			for (i = 0; i < len - first; i++) s_arena[i] = s_encbuf[first + i];
		}
		s_dir[s_dir_head].off = s_wr; s_dir[s_dir_head].len = len; s_dir[s_dir_head].flags = flags;
		s_dir_head = (s_dir_head + 1u) % REC_DIR_CAP;
		s_dir_count++;
		s_wr = (s_wr + len) % s_arena_sz;
		s_used += len;
		if (flags & REC_BASE) s_have_base = 1;
	}
	for (i = 0; i < s_state_words; i++) s_prev[i] = s_recon[i];
	s_have_prev = 1;
}

/* Rewind: enter (recon = newest state), step back one state (apply one reverse delta), read the
 * reconstructed state, and exit (drop the discarded newer records, resume forward from here). */
int ayaneo_rewind_begin(void)
{
	unsigned int i;
	if (!ayaneo_rewind_ready() || s_dir_count == 0 || !s_have_prev) return -1;
	for (i = 0; i < s_state_words; i++) s_recon[i] = s_prev[i];   /* newest state */
	s_rewinding = 1; s_back = 0;
	return 0;
}
int ayaneo_rewind_step(void)
{
	unsigned int max, idx;
	if (!s_rewinding) return -1;
	max = s_dir_count - (s_have_base ? 1u : 0u);          /* deltas applicable (base is the floor) */
	if (s_back >= max) return -1;
	idx = (s_dir_head - 1u - s_back) % REC_DIR_CAP;       /* record to undo (newest-first) */
	xor_rle_apply(s_recon, s_dir[idx].off, s_dir[idx].len, s_state_words);
	s_back++;
	return 0;
}
const void *ayaneo_rewind_cur(unsigned int *size_out)
{
	if (!ayaneo_rewind_ready() || !s_rewinding) { if (size_out) *size_out = 0; return 0; }
	if (size_out) *size_out = s_state_sz;
	return s_recon;
}
void ayaneo_rewind_end(void)
{
	unsigned int i;
	if (!s_rewinding) return;
	for (i = 0; i < s_back; i++) {                        /* discard the s_back newest records */
		s_dir_head = (s_dir_head - 1u) % REC_DIR_CAP;
		s_used -= s_dir[s_dir_head].len;
		s_dir_count--;
	}
	if (s_dir_count) {
		unsigned int nh = (s_dir_head - 1u) % REC_DIR_CAP;
		s_wr = (s_dir[nh].off + s_dir[nh].len) % s_arena_sz;
	} else { s_wr = 0; s_used = 0; s_have_base = 0; }
	for (i = 0; i < s_state_words; i++) s_prev[i] = s_recon[i];   /* resume forward from the rewound state */
	s_have_prev = 1;                                     /* recon is a valid state to delta against */
	s_back = 0; s_rewinding = 0;
}

/* --- deterministic pseudo-state generators for the selftest ---
 * P1_W: a small state that changes a FEW words per frame (compressible delta, like real gameplay).
 * P2_W: a larger state fully regenerated each frame (INCOMPRESSIBLE ~4.8KB delta) so the 990MB arena
 *       fills and WRAPS before the 256K directory cap - exercising the split-write + modular-read. */
#define P1_W  256u
#define P2_W  1200u
static void rw_p1_mut(unsigned int *s, unsigned int i)     /* state_i = mutate(state_{i-1}) */
{
	unsigned int j, h = 2654435761u * (i + 1u);
	for (j = 0; j < 8u; j++) { h = h * 1664525u + 1013904223u; s[h % P1_W] = h; }
}
static void rw_p1_build(unsigned int *s, unsigned int k)   /* recompute state_k from state_0 */
{
	unsigned int w, i;
	for (w = 0; w < P1_W; w++) s[w] = 0x11110000u ^ w;
	for (i = 1u; i <= k; i++) rw_p1_mut(s, i);
}
static void rw_p2_full(unsigned int *s, unsigned int k)    /* fully-determined state_k (O(W)) */
{
	unsigned int w, h = 0x9E3779B1u * (k + 1u);
	for (w = 0; w < P2_W; w++) { h = h * 1664525u + 1013904223u + w; s[w] = h; }
}

/* Delta round-trip selftest (no core). Phase 1: a batch that FITS (no eviction), verify EVERY
 * reconstructed state is BYTE-EXACT vs a from-scratch recompute - exactly validates the XOR
 * encode/apply + begin/step/cur/end. Phase 2: incompressible states that FORCE arena wraparound +
 * eviction, verify the newest AND the oldest-retained states are byte-exact (a bad delta or a bad
 * wrapped read would corrupt the oldest). pushed_out = phase-2 states, bad_out = mismatches (0=pass). */
int ayaneo_rewind_ring_selftest(unsigned int *slots_out, unsigned int *pushed_out, unsigned int *bad_out)
{
	static unsigned int st[P2_W], ref[P2_W];
	unsigned int i, w, bad = 0, sz, back, n1 = 400u, n2;
	const unsigned int *r;
	*slots_out = 0; *pushed_out = 0; *bad_out = 0;

	/* ---- Phase 1: exactness (no eviction) ---- */
	if (!ayaneo_rewind_reset(P1_W * 4u)) return -1;
	*slots_out = REC_DIR_CAP;
	rw_p1_build(st, 0);
	for (i = 0; i < n1; i++) {
		unsigned int *p;
		if (i) rw_p1_mut(st, i);
		p = (unsigned int *)ayaneo_rewind_capture_begin();
		if (!p) { bad++; continue; }
		for (w = 0; w < P1_W; w++) p[w] = st[w];
		ayaneo_rewind_capture_commit(P1_W * 4u);
	}
	if (ayaneo_rewind_begin() == 0) {
		back = 0;
		for (;;) {
			r = (const unsigned int *)ayaneo_rewind_cur(&sz);
			rw_p1_build(ref, n1 - 1u - back);
			if (!r || sz != P1_W * 4u) bad++;
			else for (w = 0; w < P1_W; w++) if (r[w] != ref[w]) { bad++; break; }
			if (ayaneo_rewind_step() != 0) break;
			back++;
		}
		ayaneo_rewind_end();
		if (back != n1 - 1u) bad++;                  /* n1 states => n1-1 backward steps */
	} else bad++;

	/* ---- Phase 2: arena wraparound + eviction (incompressible deltas, ~4.8KB each) ---- */
	ayaneo_rewind_reset(P2_W * 4u);
	/* arena ~ (region - hdr); fill past it: ~region/4.8KB records. Push enough to wrap once. */
	n2 = (s_arena_sz / (P2_W * 4u)) + 8000u;
	for (i = 0; i < n2; i++) {
		unsigned int *p = (unsigned int *)ayaneo_rewind_capture_begin();
		rw_p2_full(st, i);
		if (!p) { bad++; continue; }
		for (w = 0; w < P2_W; w++) p[w] = st[w];
		ayaneo_rewind_capture_commit(P2_W * 4u);
	}
	*pushed_out = n2;
	if (ayaneo_rewind_begin() == 0) {
		r = (const unsigned int *)ayaneo_rewind_cur(&sz);
		rw_p2_full(ref, n2 - 1u);                    /* newest must be exact */
		if (!r || sz != P2_W * 4u) bad++;
		else for (w = 0; w < P2_W; w++) if (r[w] != ref[w]) { bad++; break; }
		back = 0;
		while (ayaneo_rewind_step() == 0) back++;    /* walk the whole retained (post-eviction) chain */
		r = (const unsigned int *)ayaneo_rewind_cur(&sz);
		rw_p2_full(ref, n2 - 1u - back);             /* oldest retained must be exact too */
		if (r) for (w = 0; w < P2_W; w++) if (r[w] != ref[w]) { bad++; break; }
		if (back < 1000u) bad++;
		ayaneo_rewind_end();
	} else bad++;

	*bad_out = bad;
	ayaneo_rewind_reset(0);
	return bad ? -1 : 0;
}
