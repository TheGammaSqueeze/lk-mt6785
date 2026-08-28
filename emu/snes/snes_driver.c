/*
 * LK-side driver for the SNES Classic home menu. Thin: loads the asset pack from
 * boot_b into DRAM, hands the portable menu module (snes_menu.c) its work memory,
 * then each frame reads input, updates + renders the menu to the panel, and plays
 * queued sounds. All menu logic/drawing lives in the portable module so it can be
 * validated on the host against the web app.
 *
 * Reuses the GBC harness (display canvas/present, boot_b partition_read, boot
 * hook, watchdog) and provides the ayaneo_gbc_* entry names the hooks call.
 */
#include <debug.h>
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <part_interface.h>

#include "snes_menu.h"
#include "snes_audio.h"

/* ---- LK primitives ---- */
extern time_t current_time(void);
/* free-running 13 MHz counter (13 ticks/us); current_time() is only 10 ms
 * resolution, far too coarse for a per-frame dt (it beats 10/20ms at 60Hz). */
extern unsigned gpt4_get_current_tick(void);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern void mtk_wdt_disable(void);
extern void mtk_wdt_restart(void);
extern void ayaneo_display_prepare(void);
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
/* OVL hardware-layered present for the steady home carousel (OVL_LAYERS.md). A 0
 * addr disables that upper layer; l2_clean flushes the L2 band (only when rebuilt). */
extern void ayaneo_canvas_present_layers(unsigned int l2_pa, int l2_pan, int l2_clean,
					 unsigned int l3_pa, int l3_y0, int l3_y1);
#define ASP_CONTENT_S_DRV 1.18519f   /* must match snes_menu.c ASP_CONTENT_S (4:3 x zoom) */
/* tell the single-buffer present whether to skip its explicit post-swap vsync wait
 * (skip when this frame fit in a vsync; the wait is only needed for overrunning frames). */
extern void ayaneo_present_skip_vsync(int skip);
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int scale, unsigned int argb, const char *s);
extern void ayaneo_apply_persisted_brightness(void);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern void bigcore_start(void);
extern unsigned bigcore_counter(void);
extern unsigned bigcore_raw_magic(void);
extern unsigned bigcore_raw_counter(void);
extern unsigned bigcore_cached_ok(void);
extern int g_bc_target, g_bc_psci_ret;
extern unsigned g_bc_mpidr, g_bc_pwrstat;

#ifdef AYANEO_BIGCORE_EXPT
/* ---- per-frame fork/join across cpu0 + the cached worker (cpu1) ----
 * The render split is proven output-correct on the host (host_render "split").
 * snes_menu_render is deterministic/non-mutating on the menu, uses a per-core
 * z-sort context (selected by MPIDR), and writes only its band's rows, so the
 * two cores can render disjoint scanline bands of the same frame concurrently.
 * Sync is WFE/SEV with a bounded-spin fallback so a wedged worker never hangs. */
#include "bigcore_comms.h"
#include <arch/ops.h>   /* arch_clean_cache_range / arch_invalidate_cache_range (addr_t,size_t) */
extern int _dprintf(const char *fmt, ...);
static volatile struct bc_comms *const g_bc =
	(volatile struct bc_comms *)(unsigned long)BC_COMMS_PA;
#define BC_CLEAN(p, n)  arch_clean_cache_range((unsigned long)(p), (unsigned long)(n))
#define BC_INVAL(p, n)  arch_invalidate_cache_range((unsigned long)(p), (unsigned long)(n))

/* One-directional cache maintenance across the two cores (HW snoop coherency is
 * empirically not delivering cross-core writes at this pre-kernel stage, so we
 * hand data over explicitly through DRAM = the Point of Coherency).
 *
 * THE RULE that the earlier code violated: for any 64B comms line, exactly ONE
 * core ever CLEANS it (its owner) and the other only INVALIDATES before reading.
 * The old worker did BC_CLEAN(g_bc, sizeof) which wrote the worker's OWN stale
 * copy of cpu0-owned lines (go @64, the job payload incl menu_ptr @192) back over
 * DRAM, stamping garbage (0xab102d01) over cpu0's freshly written pointer. Each
 * comms line has a single owner; only the owner cleans it.
 *   line0  @0   : stage/magic          - worker owns (cpu0 wrote the MMU snapshot
 *                                         here ONCE pre-power-on, never again)
 *   go     @64  : frame seq            - cpu0 owns
 *   done   @128 : frame done           - worker owns
 *   job    @192 : fb/menu_ptr/band...  - cpu0 owns
 *   cnt    @256 : counter/cached_ok    - worker owns
 *   fault  @320 : fault capture        - worker owns (cleaned in the asm handler)
 *   diag   @384 : w_mpidr..w_menuw0    - worker owns
 * A reader INVALIDATES the peer-owned line right before reading it; an owner
 * CLEANS its line right after writing it. Never the reverse. */
#define BC_L(off)       ((void *)(unsigned long)(BC_COMMS_PA + (unsigned)(off)))
#define BC_LINE         64u

static inline void bc_sev(void) { __asm__ volatile("sev" ::: "memory"); }
static inline void bc_wfe(void) { __asm__ volatile("wfe" ::: "memory"); }
static inline void bc_dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }
static inline void bc_dsb(void) { __asm__ volatile("dsb ish" ::: "memory"); }

/* Worker-private WB-cacheable scratch line for the Lever-1 self-readback probe. No
 * other core ever touches it, so a stale readback isolates a broken worker cache/MMU
 * path (LK-local bug) from a genuine cross-core snoop-admission wall. In LK BSS so it
 * is guaranteed mapped Normal-WB by the page tables the worker adopts. */
static unsigned bc_self_wb[16] __attribute__((aligned(64)));

/* worker (cpu1): wait for a posted frame, render its band, signal done. */
void bc_worker_entry(void)
{
	unsigned last = 0;
	g_bc->stage = 0x11; BC_CLEAN(BC_L(0), BC_LINE); bc_dsb();  /* reached; publish stage (line0, worker-owned) */
	for (;;) {
		for (;;) {                             /* wait for a posted frame */
			BC_INVAL(BC_L(64), BC_LINE);   /* invalidate the GO line (cpu0-owned), read fresh from DRAM */
			if (g_bc->go != last) break;
			bc_wfe();
		}
		last = g_bc->go;
		/* DIAGNOSTIC: read cpu0's non-comms canary (0x51000000, pure cacheable) and
		 * publish what we saw (w_can1, worker-owned line6). Also drop a return canary
		 * at +0x40 for cpu0 to read - this fully characterises cpu0<->worker cacheable
		 * coherency for a region OUTSIDE the comms block, both directions. Done first
		 * so it is recorded even though the render below still faults on menu_ptr. */
		BC_INVAL(0x51000000u, 64u);
		g_bc->w_can1 = *(volatile unsigned *)(unsigned long)0x51000000u;
		/* PRODUCER-OFFLOAD VIABILITY: read the PRE-bringup static value at 0x51000024
		 * MMU-on, WITHOUT invalidate (how a worker reads static, pre-cleaned assets). If
		 * this is 0x57A70DED, the worker can read pre-bringup static data -> offload viable. */
		g_bc->w_static_can = *(volatile unsigned *)(unsigned long)0x51000024u;
#ifdef AYANEO_BC_SGI
		{	/* GIC CHANNEL TEST: cpu0 sets SGI#1 pending in cpu1's GICR via MMIO each frame.
			 * The worker POLLS GICR_ISPENDR0 (cpu1 SGI frame 0x0c070000+0x200) via MMIO. MMIO
			 * goes to the GIC peripheral, NOT the frozen DRAM/DSU-snoop path - so if the worker
			 * sees cpu0's pending bit, the GIC/MMIO is a working cpu0->worker channel and the
			 * clean producer-offload (worker tracks focus via SGI) is unlocked. */
			static unsigned sgi_seen = 0;
			if (*(volatile unsigned *)(unsigned long)0x0c070200u & 0x2u) {
				sgi_seen++;
				*(volatile unsigned *)(unsigned long)0x0c070280u = 0x2u;   /* GICR_ICPENDR0 clear */
			}
			g_bc->w_sgi_count = sgi_seen;
			g_bc->w_spm_scratch = *(volatile unsigned *)(unsigned long)0x10006250u;  /* SPM scratch MMIO */
			BC_CLEAN((unsigned long)&g_bc->w_sgi_count, 8u); bc_dsb();
		}
#endif
		{	/* AT-translate the canary VA on the WORKER: PAR-lo ATTR[63:56 of the
			 * 64-bit PAR is in the hi word; here we grab lo which has F/SH/PA] so we
			 * can see if the worker's mapping of 0x51000000 is really Device. */
			unsigned pl, ph;
			__asm__ volatile("mcr p15,0,%0,c7,c8,0" :: "r"(0x51000000u));
			__asm__ volatile("isb");
			__asm__ volatile("mrrc p15,0,%0,%1,c7" : "=r"(pl), "=r"(ph));
			g_bc->w_canpar = ph;   /* hi word carries ATTR[63:56] */
		}
		{	/* ============ DECISIVE PROBE (Lever 1) ============
			 * Isolate the worker's OWN MMU-on load path from cross-core coherency.
			 * These touch only worker-private memory, so a stale readback here means
			 * an LK-local MMU/TLB/ordering bug, NOT a snoop-admission wall. Also grab
			 * the FULL PAR-lo (bit0=F, +resolved PA) of the failing canary VA. */
			unsigned pl, ph;
			/* (A) WB-cacheable self: write -> clean-to-PoC -> invalidate -> read back.
			 * Purely local (no peer reads bc_self_wb). Stale == broken worker cache. */
			bc_self_wb[0] = 0x5E1F0000u | (last & 0xffffu);
			BC_CLEAN((unsigned long)&bc_self_wb[0], 64u); bc_dsb();
			BC_INVAL((unsigned long)&bc_self_wb[0], 64u);
			g_bc->w_self_wb = bc_self_wb[0];
			/* (B) Device self at 0x51000080 (Device-nGnRnE 2MB): write then read back.
			 * Device bypasses cache; stale == the worker's Device access is not even
			 * reaching DRAM (routing/translation), the strongest "not coherency" tell. */
			*(volatile unsigned *)(unsigned long)0x51000080u = 0x0DE00000u | (last & 0xffffu);
			bc_dsb();
			g_bc->w_self_dev = *(volatile unsigned *)(unsigned long)0x51000080u;
			/* (C) full PAR-lo of the FAILING canary VA (0x51000000): F bit + PA. */
			__asm__ volatile("mcr p15,0,%0,c7,c8,0" :: "r"(0x51000000u));
			__asm__ volatile("isb");
			__asm__ volatile("mrrc p15,0,%0,%1,c7" : "=r"(pl), "=r"(ph));
			g_bc->w_canpar_lo = pl;
			/* (D) full PAR-lo of the WB self VA: confirms it resolves to the right PA
			 * with F=0 (else the self-readback in (A) would be meaningless). */
			__asm__ volatile("mcr p15,0,%0,c7,c8,0" :: "r"((unsigned)(unsigned long)&bc_self_wb[0]));
			__asm__ volatile("isb");
			__asm__ volatile("mrrc p15,0,%0,%1,c7" : "=r"(pl), "=r"(ph));
			g_bc->w_selfpar_lo = pl;
		}
		{	/* CRITICAL PROBE: does an MMU-OFF read (AFTER the MMU was on) see cpu0's
			 * fresh write? The MMU-off snapshot read worked at bringup; if MMU-off reads
			 * still see cpu0 here, a per-frame MMU-off window can carry cpu0->worker data
			 * and Lever 4 lives. If this too is 0xa86dbdec (frozen garbage), cpu0->worker
			 * is dead by every means. LK is identity-mapped (VA==PA) so PC/SP stay valid
			 * across the toggle; invalidate the line first so no worker-cached copy shadows
			 * the physical read. */
			unsigned v, sc;
			BC_INVAL(0x51000000u, 64u);
			bc_dsb();
			__asm__ volatile("mrc p15,0,%0,c1,c0,0" : "=r"(sc));
			__asm__ volatile("mcr p15,0,%0,c1,c0,0" :: "r"(sc & ~1u));   /* SCTLR.M=0: MMU off */
			__asm__ volatile("isb");
			v = *(volatile unsigned *)(unsigned long)0x51000000u;       /* physical read, MMU off */
			__asm__ volatile("mcr p15,0,%0,c1,c0,0" :: "r"(sc));        /* MMU back on */
			__asm__ volatile("isb");
			g_bc->w_can_mmuoff = v;
		}
		*(volatile unsigned *)(unsigned long)0x51000040u = 0x77770000u | (last & 0xffffu);
		BC_CLEAN(0x51000040u, 64u);
		BC_CLEAN(BC_L(384), BC_LINE);          /* publish w_can1 (worker->cpu0, known-good) */
		BC_INVAL(BC_L(192), BC_LINE);          /* invalidate the JOB line (cpu0-owned), read fresh */
		g_bc->stage = 0x22; BC_CLEAN(BC_L(0), BC_LINE);   /* publish stage; do NOT touch go/job lines */
		{
			snes_menu *menu = (snes_menu *)(unsigned long)g_bc->menu_ptr;
			snes_target t = {0};
			unsigned bandlo, bandhi;
			/* Coherency probe: record the pointer the worker actually read from
			 * comms and flush ONLY the diag line (worker-owned) - so even if
			 * dereferencing a garbage pointer faults below, cpu0 still sees what the
			 * worker read (== cpu0's menu_ptr means the invalidate delivered it). */
			g_bc->w_menu = (unsigned)(unsigned long)menu;
			BC_CLEAN(BC_L(384), BC_LINE);
			t.fb = (unsigned int *)(unsigned long)g_bc->fb;
			t.pitch = g_bc->pitch; t.W = (int)g_bc->W; t.H = (int)g_bc->H;
			t.offx = (int)g_bc->offx; t.offy = (int)g_bc->offy;
			snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
			snes_target_band(&t, (int)g_bc->band_y0, (int)g_bc->band_y1);
			bandlo = g_bc->band_y0; bandhi = g_bc->band_y1;
			{ unsigned v;   /* worker CPU-state snapshot (w_fpexc captured in asm) */
			  __asm__ volatile("mrc p15,0,%0,c0,c0,5":"=r"(v)); g_bc->w_mpidr = v;
			  __asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(v)); g_bc->w_sctlr = v;
			  __asm__ volatile("mrc p15,0,%0,c1,c0,2":"=r"(v)); g_bc->w_cpacr = v; }
			/* read the menu state + scene pool cpu0 just updated, fresh from DRAM
			 * (bulk data: the worker is the READER, so it invalidates before reading) */
			BC_INVAL(menu, 4096);
			BC_INVAL(0x50C00000u, 0x00400000u);
			/* prove the worker can now read the menu's real data through the pointer
			 * (non-garbage first word) - flush this datum too before the render. */
			g_bc->w_menuw0 = *(volatile unsigned *)menu;
			BC_CLEAN(BC_L(384), BC_LINE);
			g_bc->stage = 0x33; BC_CLEAN(BC_L(0), BC_LINE);
			snes_menu_render(menu, &t);
			g_bc->stage = 0x44;
			/* clean the band we rendered out to DRAM so cpu0/display see it
			 * (bulk data: the worker OWNS its band, so it cleans it) */
			BC_CLEAN((unsigned long)t.fb + (unsigned long)bandlo * t.pitch * 4u,
				 (unsigned long)(bandhi - bandlo) * t.pitch * 4u);
		}
		g_bc->counter = last;                  /* heartbeat = frames rendered */
		g_bc->done = last;
		BC_CLEAN(BC_L(256), BC_LINE);          /* publish counter/cached_ok (worker-owned) */
		BC_CLEAN(BC_L(128), BC_LINE);          /* publish done (worker-owned) */
		BC_CLEAN(BC_L(0), BC_LINE);            /* publish final stage=0x44 (worker-owned) */
		bc_dsb();
		bc_sev();                              /* wake cpu0 */
	}
}

static int bc_worker_ready(void) { return g_bc->cached_ok == 0xB16C0DE5u; }

/* Target little-cluster clock once the two cores are rendering in parallel. The
 * whole cluster (cpu0..5) shares one PLL/buck, so this clocks both workers. With
 * a 2-core split each core does ~half the frame, so we can drop from 2000 MHz and
 * still hold 60 fps at much lower dynamic power. Tune on HW: raise if the FPS HUD
 * dips below 60, lower toward ~1075 for minimum power. Single-core fallback keeps
 * the boot 2000 MHz. */
/* Deferred: keep 2000 until the parallel render is confirmed working on HW, so a
 * freeze/slowdown can't be confused with the clock. Once the split holds, set this
 * to ~1200 (then tune toward ~1075) for the low-power win. */
#define BC_MHZ 2000
static int s_bc_clk_set;

/* cpu0: fork the bottom band to the worker, render the top band, join. */
static unsigned s_bc_seq;
unsigned g_bc_wfin, g_bc_fb;   /* diagnostics: worker-finished vs fallback frames */
static unsigned g_bc_canrb;    /* cpu0's read-back of its own canary write to 0x51000000 */
static unsigned g_bc_cpupar;   /* cpu0's PAR-hi of the canary VA (0x04=Device 0xff=WB) */
extern unsigned int gpt4_get_current_tick(void);
static void bc_dispatch(unsigned int *fb, unsigned pitch, int W, int H,
			snes_menu *menu, snes_target *tfull)
{
	int sy = (H / 2) & ~15;                    /* split on a 16px (>=64B) line */
	unsigned seq = ++s_bc_seq, tw0, waited;
	g_bc->fb = (unsigned)(unsigned long)fb; g_bc->pitch = pitch;
	g_bc->W = (unsigned)W; g_bc->H = (unsigned)H;
	g_bc->offx = (unsigned)tfull->offx; g_bc->offy = (unsigned)tfull->offy;
	g_bc->band_y0 = (unsigned)sy; g_bc->band_y1 = (unsigned)H;   /* worker: bottom */
	g_bc->menu_ptr = (unsigned)(unsigned long)menu;
	/* HW cross-core coherency is not delivering cross-core writes here, so hand the
	 * data over through DRAM. cpu0 OWNS the job line (@192) and the bulk menu + scene
	 * pool the render walks: clean ONLY those to the Point of Coherency. Do NOT clean
	 * the whole comms block - that would write cpu0's stale copies of worker-owned
	 * lines (done/counter/diag) back over the worker's fresh values. */
	BC_CLEAN(BC_L(192), BC_LINE);               /* job payload (fb/menu_ptr/band...) */
	BC_CLEAN(menu, 4096);                       /* snes_menu struct */
	BC_CLEAN(0x50C00000u, 0x00400000u);         /* SNES_HOME_PA scene pool (first 4MB) */
	/* DIAGNOSTIC canary in a NON-comms, pure-cacheable region (0x51000000 comp
	 * staging, free during render, never touched uncached). Tests whether the
	 * worker can read cpu0's cleaned cacheable writes AT ALL, vs the comms region
	 * specifically. cpu0 writes+cleans here; the worker invalidates+reads it into
	 * w_can1; the worker also writes a return canary at +0x40 for cpu0 to read. */
	*(volatile unsigned *)(unsigned long)0x51000000u = 0xCA5A0000u | (seq & 0xffffu);
	BC_CLEAN(0x51000000u, 64u);
#ifdef AYANEO_BC_SGI
	/* GIC CHANNEL TEST (cpu0 side): set SGI#1 pending in cpu1's GICR via MMIO. If the worker's
	 * MMIO poll of GICR_ISPENDR0 sees it (w_sgi_count increments), MMIO is a live cpu0->worker
	 * channel bypassing the dead DRAM snoop. GICR SGI frame for cpu1 = 0x0c070000; ISPENDR0 +0x200. */
	*(volatile unsigned *)(unsigned long)0x0c070200u = 0x2u;   /* GICR_ISPENDR0: SGI#1 pending */
	/* GENERIC MMIO channel (disambiguates a GIC-specific block from a general MMIO freeze):
	 * cpu0 writes a rolling value to SPM CPU_SPARE_CON (0x10006250, unused scratch); the worker
	 * reads it MMU-on via MMIO. If w_spm_scratch tracks 0x5A5Axxxx, MMIO is a live cpu0->worker
	 * channel regardless of the GIC's NS-write permission. */
	*(volatile unsigned *)(unsigned long)0x10006250u = 0x5A5A0000u | (seq & 0xffffu);
#endif
	/* EXPERIMENT (shared-L3 evict): if the worker shares the DSU L3 but gets no snoop
	 * invalidations, it holds a stale (boot_b-era) L3 line for the canary that cpu0's
	 * clean-only never evicts. A clean+INVALIDATE to PoC from cpu0 evicts that shared L3
	 * line so the worker re-fills from DRAM. Low probability (the Device canary bypasses
	 * caches, and the worker's PRIVATE L1/L2 copy is unreachable without snoop broadcast),
	 * but cheap and harmless - fold it in for an extra chance. */
	BC_INVAL(0x51000000u, 64u);
	bc_dsb();
	{	/* LEVER (DVM): force inner-shareable Distributed Virtual Memory traffic after
		 * publishing the canary. TLBI ...IS broadcasts a DVM Sync to every PE in the
		 * inner-shareable domain; a late-woken worker that is powered + PWR_ON_ACK'd but
		 * not yet actively RECEIVING snoops may get its snoop-input wired by a DVM
		 * round-trip. If the worker's canary read flips 0xa86dbdec -> 0xCA5Axxxx, this is
		 * the missing step. Harmless otherwise (a TLB maintenance op). */
		__asm__ volatile("mcr p15,0,%0,c8,c3,0" :: "r"(0u));   /* TLBIALLIS (Inner-Shareable) */
		__asm__ volatile("dsb ish");
		__asm__ volatile("isb");
	}
	/* cpu0 reads back its OWN write to 0x51000000: if this is 0xCA5Axxxx the write
	 * landed (Device -> DRAM) from cpu0's view; if it is the old value the write is
	 * being dropped/not landing even for cpu0. Pins "does cpu0's write reach DRAM". */
	g_bc_canrb = *(volatile unsigned *)(unsigned long)0x51000000u;
	{	/* cpu0's OWN PAR of the canary VA: confirms cpu0 really sees 0x51000000 as
		 * Device (0x04) too, so its read-back is from DRAM not a stale WB cache line. */
		unsigned pl, ph;
		__asm__ volatile("mcr p15,0,%0,c7,c8,0" :: "r"(0x51000000u));
		__asm__ volatile("isb");
		__asm__ volatile("mrrc p15,0,%0,%1,c7" : "=r"(pl), "=r"(ph));
		g_bc_cpupar = ph;
	}
	bc_dsb();                                  /* job + menu update must land before go */
	g_bc->go = seq;                            /* publish the frame (cpu0 owns the go line) */
	BC_CLEAN(BC_L(64), BC_LINE);               /* clean go to DRAM - the worker reads it from there */
	bc_dsb();
	bc_sev();                                  /* wake the worker */
	{	/* cpu0 renders the top band [0, sy) meanwhile */
		snes_target t0 = *tfull;
		snes_target_band(&t0, 0, sy);
		snes_menu_render(menu, &t0);
	}
	tw0 = gpt4_get_current_tick();
	/* Join by BUSY-SPIN (not WFE): a WFE here would park cpu0 and the time-bounded
	 * fallback would never fire, so a worker that never signals done (crashed / missed
	 * the wakeup) would hang the menu forever. Busy-spin lets the fallback fire and
	 * cpu0 render the worker's band itself. Bound by WALL CLOCK (13 MHz gpt4 tick), not
	 * an iteration count: ~1.5 ms leaves ~31 ms of a 33 ms (30 fps) frame for cpu0 to
	 * render the second band, so a failing worker degrades to ~30 fps, not 4 fps. */
	for (;;) {
		BC_INVAL(BC_L(128), BC_LINE);      /* invalidate DONE (worker-owned), read fresh */
		if (g_bc->done == seq) { g_bc_wfin++; break; }
		/* Fallback deadline: 2 ms. The worker is still not a coherent snoop
		 * participant on HW (run-15: removing the DCISW did not help, the DSU snoop
		 * admit is an ATF/hardware power-sequence gap, not our worker code), so it
		 * faults every frame and cpu0 always renders both bands. Keep the wasted wait
		 * small so the menu holds ~30 fps single-core until the coherency path is
		 * resolved. Raise this again when the worker actually completes. */
		if ((gpt4_get_current_tick() - tw0) > 13u * 2000u) {   /* 2 ms */
			snes_target tb = *tfull;
			snes_target_band(&tb, sy, H);
			snes_menu_render(menu, &tb);
			g_bc_fb++;
			break;
		}
	}
	/* the worker cleaned its band to DRAM; invalidate cpu0's stale copy of it so
	 * the present/display picks up the worker's rows (not cpu0's blank band). */
	BC_INVAL((unsigned long)fb + (unsigned long)sy * pitch * 4u,
		 (unsigned long)(H - sy) * pitch * 4u);
	bc_dmb();
	waited = (gpt4_get_current_tick() - tw0) / 13u;   /* us cpu0 waited for worker */
	{	/* One-time COMPREHENSIVE dump the instant a worker fault is captured
		 * (everything needed to diagnose: fault regs, worker CPU state, the job
		 * it was handed), then only a light periodic line so the log is readable. */
		static unsigned fc, dumped, parlogged;
		BC_INVAL(BC_L(0), BC_LINE);        /* fresh stage (line0, worker-owned) */
		BC_INVAL(BC_L(256), BC_LINE);      /* fresh counter/cached_ok (line4, worker-owned) */
		BC_INVAL(BC_L(320), BC_LINE);      /* fresh fault capture (line5, worker-owned) */
		BC_INVAL(BC_L(384), BC_LINE);      /* fresh diag: par + coherency probe (line6) */
		if (!parlogged) {   /* once: the worker's mapping of the comms VA + coherency probe */
			parlogged = 1;
			_dprintf("BC PAR (comms VA attrs): par=0x%x:0x%x (bit0=fault; SH bits8:7; ATTR 63:56)\n",
				 g_bc->par_hi, g_bc->par_lo);
			_dprintf("BC COHERENCY PROBE: cpu0 menu=0x%x  worker-read menu=0x%x  worker-read *menu=0x%x\n",
				 g_bc->menu_ptr, g_bc->w_menu, g_bc->w_menuw0);
			{	/* non-comms canary both directions: 0xCA5A.... => cpu0->worker
				 * cacheable read OK; 0x7777.... => worker->cpu0 OK. Garbage =>
				 * that direction's cacheable coherency is broken (not comms-specific). */
				unsigned can2;
				BC_INVAL(0x51000040u, 64u);
				can2 = *(volatile unsigned *)(unsigned long)0x51000040u;
				_dprintf("BC CANARY (non-comms 0x51000000): cpu0->worker worker-read=0x%x (expect 0xCA5Axxxx) ; worker->cpu0 cpu0-read=0x%x (expect 0x7777xxxx)\n",
					 g_bc->w_can1, can2);
				_dprintf("BC CANARY PROBE: cpu0 self-readback=0x%x ; cpu0 PAR-hi=0x%x ; worker PAR-hi of 0x51000000=0x%x (ATTR byte at bits31:24, 0x04=Device 0xff=WB)\n",
					 g_bc_canrb, g_bc_cpupar, g_bc->w_canpar);
				_dprintf("BC STATICPROBE: worker read of PRE-bringup static 0x51000024 = 0x%x (0x57A70DED => worker CAN read static pre-cleaned data => producer-offload VIABLE)\n",
					 g_bc->w_static_can);
#ifdef AYANEO_BC_SGI
				_dprintf("BC SGIPROBE: worker saw %u GIC SGI-pending signals via MMIO (increments => GIC/MMIO is a live cpu0->worker channel; clean offload unlocked)\n",
					 g_bc->w_sgi_count);
				_dprintf("BC MMIOPROBE: worker read SPM scratch 0x10006250 = 0x%x (0x5A5Axxxx tracking seq => MMIO is a live cpu0->worker channel regardless of GIC)\n",
					 g_bc->w_spm_scratch);
#endif
				/* Lever-1 decisive probe (worker-private, isolates worker MMU/cache from
				 * cross-core coherency). Read the decision tree in MULTICORE_RESEARCH.md:
				 *  self_wb==0x5E1Fxxxx & self_dev==0x0DE0xxxx & canpar_lo bit0==0 & PA==0x51000
				 *    -> worker path SANE, failure is purely cpu0->worker visibility -> WALL.
				 *  self_wb STALE -> worker cacheable load path BROKEN -> LK-local bug (fixable).
				 *  self_dev STALE -> worker Device access not reaching DRAM -> translation bug. */
				_dprintf("BC LEVER1 PROBE: w_self_wb=0x%x (exp 0x5E1Fxxxx) w_self_dev=0x%x (exp 0x0DE0xxxx) canpar_lo=0x%x selfpar_lo=0x%x (bit0=F, PA in 31:12)\n",
					 g_bc->w_self_wb, g_bc->w_self_dev, g_bc->w_canpar_lo, g_bc->w_selfpar_lo);
				_dprintf("BC MMUOFF PROBE: worker MMU-off read of 0x51000000 = 0x%x (0xCA5Axxxx => MMU-off carries cpu0->worker; 0xa86dbdec => frozen, dead)\n",
					 g_bc->w_can_mmuoff);
			}
		}
		if (g_bc->fault_type && !dumped) {
			dumped = 1;
			_dprintf("BC WORKER FAULT: type=%u(1undef2pabt3dabt) far=0x%x fsr=0x%x pc=0x%x spsr=0x%x\n",
				 g_bc->fault_type, g_bc->fault_far, g_bc->fault_fsr,
				 g_bc->fault_pc, g_bc->fault_spsr);
			_dprintf("BC WORKER STATE: mpidr=0x%x sctlr=0x%x cpacr=0x%x fpexc=0x%x stage=0x%x\n",
				 g_bc->w_mpidr, g_bc->w_sctlr, g_bc->w_cpacr, g_bc->w_fpexc, g_bc->stage);
			_dprintf("BC JOB: fb=0x%x pitch=%u W=%u H=%u band=%u..%u menu=0x%x wstacktop=0x%x\n",
				 g_bc->fb, g_bc->pitch, g_bc->W, g_bc->H,
				 g_bc->band_y0, g_bc->band_y1, g_bc->menu_ptr, g_bc->stack_top);
		}
		if ((++fc % 240u) == 0u) {  /* ~every 8s at 30fps: minimal spam */
			unsigned can2;
			BC_INVAL((unsigned long)BC_COMMS_PA + 384u, 64u);   /* fresh probe line */
			BC_INVAL(0x51000040u, 64u);
			can2 = *(volatile unsigned *)(unsigned long)0x51000040u;
			_dprintf("BC split: wfin=%u fb=%u wait=%uus stage=0x%x fault=%u/pc0x%x wcnt=%u wmenu=0x%x w*menu=0x%x can1=0x%x can2=0x%x\n",
				 g_bc_wfin, g_bc_fb, waited, g_bc->stage,
				 g_bc->fault_type, g_bc->fault_pc, g_bc->counter,
				 g_bc->w_menu, g_bc->w_menuw0, g_bc->w_can1, can2);
		}
	}
}
#endif
extern int  mt_power_off(void);
extern int  pmic_detect_powerkey(void);

/* ---- audio (ayaneo_audio.c): codec bring-up + direct 48 kHz ring submit ---- */
extern void ayaneo_gbc_audio_init(void);
extern void ayaneo_snes_audio_submit(const short *stereo, unsigned frames);
extern int  ayaneo_snes_audio_room(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_gbc_audio_get_volume(void);

/* ---- volume/brightness (MTK keypad volume keys; Select = brightness modifier) ---- */
extern int  mtk_detect_key(unsigned short key);
extern int  ayaneo_brightness_step(int dir);   /* dir +1/-1; returns new 0-100% */
extern int  ayaneo_brightness_pct(void);
extern void ayaneo_settings_load(void);
extern void ayaneo_settings_save(void);

/* ---- input (gpio-keys, active-low) ---- */
extern int mt_set_gpio_mode(unsigned pin, unsigned mode);
extern int mt_set_gpio_dir(unsigned pin, unsigned dir);
extern int mt_set_gpio_pull_enable(unsigned pin, unsigned en);
extern int mt_set_gpio_pull_select(unsigned pin, unsigned sel);
extern int mt_get_gpio_in(unsigned pin);
#define GP(n)      ((n) | 0x80000000u)
#define PRESSED(g) (mt_get_gpio_in(GP(g)) == 0)
#define K_LEFT 78
#define K_RIGHT 80
#define K_UP 89
#define K_DOWN 79
#define K_A 83
#define K_B 82
#define K_START 91
#define K_SELECT 90

/* ---- config / DRAM layout ---- */
#define SNES_PART     "boot_b"
#define SNES_OFF      0x00400000u
#define SNES_MAGIC    0x5A534E53u
#define SNES_BLOB_PA  0x50000000u
#define SNES_HOME_PA  0x50C00000u   /* home rnode pool */
#define SNES_BG_PA    0x50E00000u   /* bg rnode pool */
#define SNES_COMP_PA  0x51000000u   /* compressed staging */
#define SNES_FCC_PA   0x51000000u   /* focused-card body cache (panel-sized; REUSES the COMP
                                     * staging, which is only touched during the boot-time
                                     * pack load - free for the whole render loop after that) */
#define SNES_WP_PA    0x52000000u   /* wallpaper cache (1536*720*4) */
#define SNES_CHROME_PA 0x53000000u  /* static chrome cache (up to 1280*960*4 in 4:3) */
/* OVL layer buffers (OVL_LAYERS.md). The ONLY WB DRAM mapped in LK is the SCRATCH/
 * download window [0x4E000000, 0x56000000) (k85v1_64 SCRATCH_ADDR + SCRATCH_SIZE);
 * 0x56000000+ is UNMAPPED (an earlier attempt at 0x57/0x58 data-aborted). Reuse the
 * two parked-bigcore slots, which sit inside that window and are unused whenever
 * AYANEO_BIGCORE_EXPT is off (the default). Each is a panel-sized (1280*960*4 =
 * 0x4B0000) double buffer (0x960000 total) inside its 16MB slot. */
#define SNES_OVL_L2_PA 0x54000000u  /* mapped WB (was BC_COMMS); free when EXPT off */
#define SNES_OVL_L3_PA 0x55000000u  /* mapped WB (was SPMFW staging); free when EXPT off */
#define SNES_RAW_MAX  (32u * 1024 * 1024)
#define SNES_COMP_MAX (16u * 1024 * 1024)
#define HOME_CAP (16u * 1024 * 1024 / (unsigned)sizeof(snes_rnode))
#define BG_CAP   (2u  * 1024 * 1024 / (unsigned)sizeof(snes_rnode))

static snes_pack s_pk;
static snes_menu s_menu;
static snes_mixer s_mix;
static short s_mixbuf[16384 * 2];  /* holds a full ring-half refill (~341 ms) */

/* Resolve a sound res-hash to its PCM + loop info and start a mixer voice. */
static void play_sound(uint32_t hash, int loop, int is_bgm)
{
	const snes_snd_entry *sn = snes_res_snd(&s_pk, hash);
	const int16_t *pcm;
	if (!sn || !sn->frames) return;
	pcm = (const int16_t *)(s_pk.base + sn->pcm);
	snes_audio_play(&s_mix, pcm, sn->frames, sn->rate,
			sn->loop_start, sn->loop_end, loop, 256, is_bgm);
}

/* on-screen volume/brightness slider (drawn for a short time after a change) */
static int s_osd_kind;    /* 0 none, 1 volume, 2 brightness */
static int s_osd_pct;
static int s_osd_ticks;

/* Poll the hardware volume keys. Plain Volume adjusts audio; Select + Volume
 * adjusts screen brightness. Persists the new value to boot_b. */
static void poll_volume(void)
{
	static int vu_prev, vd_prev;
	int vu = mtk_detect_key(0x11);      /* VolumeUp   */
	int vd = mtk_detect_key(0x00);      /* VolumeDown */
	int sel = PRESSED(K_SELECT);        /* brightness modifier */
	int dir = 0;
	if (vu && !vu_prev) dir = +1;
	else if (vd && !vd_prev) dir = -1;
	if (dir) {
		if (sel) {
			s_osd_pct = ayaneo_brightness_step(dir);
			s_osd_kind = 2;
		} else {
			ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5);
			s_osd_pct = ayaneo_gbc_audio_get_volume();
			s_osd_kind = 1;
		}
		s_osd_ticks = 90;   /* ~1.4 s at 8 ms/frame */
		ayaneo_settings_save();
	}
	vu_prev = vu; vd_prev = vd;
}

/* draw the volume/brightness slider onto the canvas (letterbox top bar) */
static void draw_osd(unsigned int *fb, unsigned int pitch, int W)
{
	int bw = 360, bh = 34, bx = (W - bw) / 2, by = 18, fillw;
	if (s_osd_ticks <= 0) return;
	ayaneo_fill(fb, pitch, bx - 8, by - 8, bw + 16, bh + 16, 0xE0101418u);
	ayaneo_fill(fb, pitch, bx, by, bw, bh, 0xFF303840u);
	fillw = bw * (s_osd_pct < 0 ? 0 : s_osd_pct > 100 ? 100 : s_osd_pct) / 100;
	ayaneo_fill(fb, pitch, bx, by, fillw, bh, 0xFF37B0FFu);
	ayaneo_text(fb, pitch, bx, by - 20, 2, 0xFFFFFFFFu,
		    s_osd_kind == 2 ? "BRIGHTNESS" : "VOLUME");
	s_osd_ticks--;
}

/* ---- OVL hardware-layering state (OVL_LAYERS.md) ---- */
static uint32_t s_cc_sig;    /* card-cache signature the L2 buffer was built for */
static int s_cc_valid;       /* L2 currently holds a valid build */
static int s_l2_flip;        /* L2 live-buffer index (flips only on rebuild) */
static int s_l3_flip;        /* L3 live-buffer index (flips every frame) */
static int s_was_layered;    /* previous frame presented via the OVL layers */

/* ---- frame-time telemetry (top-left readout) ---- */
extern unsigned (*g_perf_tick)(void);  /* snes_menu.c per-phase profiler hook */
extern unsigned g_perf[8];             /* 0 wp,1 chrome,2 carousel,3 filmstrip,4 rest */
static unsigned perf_tick(void) { return gpt4_get_current_tick(); }
static unsigned s_perf_render_us;    /* last frame: update+render (CPU cost) */
static unsigned s_perf_present_us;   /* last frame: present incl. vsync wait */
static char s_perf_str[40] = "";
static char *u2s(char *p, unsigned v)
{
	char tmp[12]; int n = 0;
	if (v == 0) { *p++ = '0'; return p; }
	while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
	while (n) *p++ = tmp[--n];
	return p;
}
static char s_perf_str2[48] = "";
#ifdef AYANEO_BIGCORE_EXPT
static char s_perf_str3[96] = "";   /* experimental bigcore proof-of-life line (EXPT only) */
#endif
#ifdef AYANEO_BIGCORE_EXPT   /* i2s/u2h feed the EXPT-only bigcore line below */
static char *i2s(char *p, int v) { if (v < 0) { *p++ = '-'; v = -v; } return u2s(p, (unsigned)v); }
static char *u2h(char *p, unsigned v) {   /* compact hex, no leading zeros */
	static const char hx[] = "0123456789abcdef"; char t[8]; int n = 0;
	*p++ = '0'; *p++ = 'x';
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = hx[v & 0xf]; v >>= 4; }
	while (n) *p++ = t[--n];
	return p;
}
#endif
static void draw_perf(unsigned int *fb, unsigned int pitch)
{
	static unsigned acc_r, acc_p, n, ap0, ap1, ap2, ap3, ap4;
	acc_r += s_perf_render_us; acc_p += s_perf_present_us; n++;
	ap0 += g_perf[0]/13; ap1 += g_perf[1]/13; ap2 += g_perf[2]/13;
	ap3 += g_perf[3]/13; ap4 += g_perf[4]/13;
#ifdef AYANEO_BIGCORE_EXPT
	{	/* EXPERIMENTAL multicore proof of life: boot MPIDR, PSCI target, ret, counter */
		char *p = s_perf_str3;
		*p++='B'; *p++='C'; *p++=' ';
		*p++='m'; p=u2h(p, g_bc_mpidr); *p++=' ';
		*p++='t'; p=i2s(p, g_bc_target); *p++=' ';
		*p++='r'; p=i2s(p, g_bc_psci_ret); *p++=' ';
		*p++='p'; p=u2h(p, g_bc_pwrstat); *p++=' ';
		/* raw (ungated) magic + counter: g=1 if handshake magic present, then the
		 * live counter regardless of magic, so one flash fully classifies (see
		 * bigcore.c bigcore_raw_*). */
		/* g = reached-the-stub (magic), k = came-up-cached (MMU+caches on),
		 * c = worker heartbeat. g1 k0 => reached but MMU-enable wedged. */
		*p++='g'; *p++=(bigcore_raw_magic()==0xB16C0DE5u)?'1':'0';
		*p++='k'; *p++=(bigcore_cached_ok()==0xB16C0DE5u)?'1':'0'; *p++=' ';
		*p++='c'; p=u2s(p, bigcore_raw_counter()); *p++=' ';
		/* d = frames the worker finished its band in time; b = fallback frames
		 * (worker missed the deadline). d climbing = split working; b climbing
		 * with d flat = worker not completing (cpu0 doing both bands). */
#ifdef AYANEO_BIGCORE_EXPT
		{ extern unsigned g_bc_wfin, g_bc_fb;
		  *p++='d'; p=u2s(p, g_bc_wfin); *p++='b'; p=u2s(p, g_bc_fb); }
#endif
		*p=0;
	}
#endif
	if (n >= 20) {
		unsigned ar = acc_r / n, ap = acc_p / n, tot = ar + ap;
		unsigned fps = tot ? (1000000u + tot / 2) / tot : 0;
		char *p = s_perf_str;
		p = u2s(p, fps); *p++ = 'f'; *p++ = ' ';
		*p++ = 'r'; p = u2s(p, ar); *p++ = ' ';
		*p++ = 'p'; p = u2s(p, ap); *p = 0;
		p = s_perf_str2;
		*p++='w'; p=u2s(p,ap0/n); *p++=' ';
		*p++='c'; p=u2s(p,ap1/n); *p++=' ';
		*p++='r'; p=u2s(p,ap2/n); *p++=' ';
		*p++='f'; p=u2s(p,ap3/n); *p++=' ';
		*p++='o'; p=u2s(p,ap4/n); *p=0;
		acc_r = acc_p = n = ap0 = ap1 = ap2 = ap3 = ap4 = 0;
	}
	/* dark semi-transparent backing so the overlay is legible over the light
	 * wallpaper/chrome (the plain green/blue text washes out on gray). */
	ayaneo_fill(fb, pitch, 4, 2, 740, 62, 0xC8000000u);
	if (s_perf_str[0]) {
		ayaneo_text(fb, pitch, 10, 6, 2, 0xFF00FF66u, s_perf_str);
		ayaneo_text(fb, pitch, 10, 26, 2, 0xFF00FF66u, s_perf_str2);
	}
#ifdef AYANEO_BIGCORE_EXPT
	ayaneo_text(fb, pitch, 10, 46, 2, 0xFF3060FFu, s_perf_str3);   /* blue, EXPT only */
#endif
}

static void dbg(const char *msg)
{
	unsigned int pitch, W, H, i;
	for (i = 0; i < 2; i++) {
		unsigned int *b = ayaneo_canvas_back(&pitch, &W, &H);
		ayaneo_fill(b, pitch, 0, 0, (int)W, 40, 0xFF000000u);
		ayaneo_text(b, pitch, 20, 8, 3, 0xFF40FF60u, msg);
		ayaneo_canvas_present();
	}
}

static int load_pack(void)
{
	unsigned char hdr[12];
	unsigned magic, rawlen, complen;
	unsigned char *comp = (unsigned char *)SNES_COMP_PA;
	unsigned char *blob = (unsigned char *)SNES_BLOB_PA;
	unsigned long zlen;
	if (partition_read(SNES_PART, SNES_OFF, hdr, 12) != 12) return -1;
	magic  = (unsigned)hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | ((unsigned)hdr[3]<<24);
	rawlen = (unsigned)hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | ((unsigned)hdr[7]<<24);
	complen= (unsigned)hdr[8] | (hdr[9]<<8) | (hdr[10]<<16) | ((unsigned)hdr[11]<<24);
	if (magic != SNES_MAGIC || rawlen == 0 || rawlen > SNES_RAW_MAX ||
	    complen == 0 || complen > SNES_COMP_MAX) return -2;
	if (partition_read(SNES_PART, SNES_OFF + 12, comp, complen) != (ssize_t)complen) return -3;
	zlen = complen;
	if (zunzip(comp, &zlen, blob, (int)rawlen, 0) != 0) return -4;
	if (snes_pack_open(&s_pk, blob, rawlen) != 0) return -5;
	return 0;
}

static void input_init(void)
{
	unsigned pins[8] = { K_LEFT, K_RIGHT, K_UP, K_DOWN, K_A, K_B, K_START, K_SELECT };
	int i;
	for (i = 0; i < 8; i++) {
		mt_set_gpio_mode(GP(pins[i]), 0); mt_set_gpio_dir(GP(pins[i]), 0);
		mt_set_gpio_pull_enable(GP(pins[i]), 1); mt_set_gpio_pull_select(GP(pins[i]), 1);
	}
}

#ifdef AYANEO_DEBUG_LOGGING
/* Per-second UART breakdown of the layered present's cost, so the slow-present
 * regression can be diagnosed from the log (not just the coarse OSD render/present).
 * Tracks the WORST (max total) present frame in each 60-frame window and prints its
 * full breakdown once, so a brief scroll's hitch frame is captured without flooding
 * the slow UART every frame. g_snes_disp_us[] is filled inside present_layers. */
extern unsigned g_snes_disp_us[6];   /* [L0clean L2clean L3clean cfg trig vsync] us */
extern unsigned g_cc_us[4];          /* build_cardcache: clear, draw, band-scan, unpremult */
extern unsigned g_cur_us[4];         /* render_cursor_layer: clear, card, cursor, unpremult */
static void snes_present_log(int rebuilt, unsigned build_us, unsigned cursor_us,
			     unsigned pl_us)
{
	static unsigned n, worst, wb, wc, wp, wr, wreb, wd[6], wcc[4], wcu[4];
	unsigned total = build_us + cursor_us + pl_us;
	if (total >= worst) {
		unsigned i;
		worst = total; wb = build_us; wc = cursor_us; wp = pl_us;
		wr = s_perf_render_us; wreb = (unsigned)rebuilt;
		for (i = 0; i < 6; i++) wd[i] = g_snes_disp_us[i];
		for (i = 0; i < 4; i++) { wcc[i] = g_cc_us[i]; wcu[i] = g_cur_us[i]; }
	}
	if (++n >= 90u) {
		_dprintf("SNESP st=%d reb=%u rend=%uus PRESENT=%uus [build=%u curs=%u pl=%u] "
			 "cc{clr=%u drw=%u scan=%u unp=%u} cur{clr=%u card=%u cur=%u unp=%u} "
			 "clean{L0=%u L2=%u L3=%u} cfg=%u trig=%u vsync=%u\n",
			 s_menu.state, wreb, wr, worst, wb, wc, wp,
			 wcc[0], wcc[1], wcc[2], wcc[3], wcu[0], wcu[1], wcu[2], wcu[3],
			 wd[0], wd[1], wd[2], wd[3], wd[4], wd[5]);
		n = 0; worst = 0;
	}
}
#endif

static int snes_emu_thread(void *arg)
{
	int r;
	unsigned last;
	(void)arg;

	ayaneo_display_prepare();
	mtk_wdt_disable();
	dbg("SNES: loading pack");

	r = load_pack();
	if (r != 0) {
		char m[16]; m[0]='S';m[1]='N';m[2]='E';m[3]='S';m[4]=' ';m[5]='E';m[6]='R';
		m[7]='R';m[8]=' ';m[9]=(char)('0'-r);m[10]=0;
		dbg(m);
		for (;;) { mtk_wdt_restart(); thread_sleep(200); }
	}
	if (snes_menu_init(&s_menu, &s_pk, (snes_rnode *)SNES_HOME_PA, HOME_CAP,
			   (snes_rnode *)SNES_BG_PA, BG_CAP, (uint32_t *)SNES_WP_PA,
			   (uint32_t *)SNES_CHROME_PA) != 0) {
		dbg("SNES ERR: menu init");
		for (;;) { mtk_wdt_restart(); thread_sleep(200); }
	}
	/* focused-card body cache: render the settled card once, blit it (shifted) each frame
	 * instead of re-compositing (~4ms -> ~1ms on the A55). Reuses the load-only COMP slot. */
	s_menu.fcc = (uint32_t *)(unsigned long)SNES_FCC_PA;
	s_menu.fcc_ready = 0;

	input_init();
	ayaneo_set_cpu_mhz(2000);
	bigcore_start();   /* EXPERIMENTAL: try to bring up a big A76 core (proof of life) */
	ayaneo_apply_persisted_brightness();

	/* bring up the codec/AFE ring and start the looping home BGM */
	ayaneo_settings_load();          /* persisted volume + brightness */
	snes_audio_init(&s_mix);
	ayaneo_gbc_audio_init();
	if (s_menu.bgm) play_sound(s_menu.bgm, 1, 1);

	g_perf_tick = perf_tick;      /* enable the render per-phase profiler */
	last = gpt4_get_current_tick();

	for (;;) {
		unsigned int pitch, W, H;
		unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
		snes_target t = {0};   /* zero-init incl. the band clip (band_y0/y1) */
		int layered;           /* this frame uses the OVL hardware-layered present */
		snes_input in;
		unsigned t_frame0 = gpt4_get_current_tick();
		/* 13 MHz counter: dt in seconds = ticks / 13e6 (unsigned wrap-safe) */
		float dt = (float)(t_frame0 - last) / 13000000.0f;
		if (dt <= 0) dt = 0.016f; if (dt > 0.1f) dt = 0.1f;
		last = t_frame0;

		in.left = PRESSED(K_LEFT); in.right = PRESSED(K_RIGHT);
		in.up = PRESSED(K_UP); in.down = PRESSED(K_DOWN);
		in.a = PRESSED(K_A); in.b = PRESSED(K_B);
		in.start = PRESSED(K_START); in.select = PRESSED(K_SELECT);

		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2; t.offy = ((int)H - SNES_VH) / 2;
		snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
		/* the panel is physically 1280x960 (4:3); fill it natively instead of
		 * letterboxing the 720 design. Enable the 4:3 adaptation whenever the panel
		 * is at least 960 tall and rebuild the chrome cache on the transition. */
		{
			int want = ((int)H >= 960) ? 1 : 0;
			if (want != s_menu.aspect) { s_menu.aspect = want; s_menu.chrome_ready = 0; }
		}
		/* clear the letterbox bars (the wallpaper covers the 720 region) */
		ayaneo_fill(fb, pitch, 0, 0, (int)W, t.offy, 0xFF000000u);
		ayaneo_fill(fb, pitch, 0, t.offy + SNES_VH, (int)W, t.offy, 0xFF000000u);

		poll_volume();
		snes_menu_update(&s_menu, &in, dt);
		/* OVL hardware layering: only the STATIC home carousel is composited from OVL
		 * layers - the card bodies (L2) and cursor (L3) are skipped in the framebuffer
		 * (L0) pass. While the carousel is MOVING (its state signature changed since
		 * last frame), rebuilding the L2 cache every frame is slow (the premult build
		 * path is not NEON) and tears, so those frames render single-buffer via the fast
		 * direct path - exactly the tear-free pre-layering carousel. Likewise every
		 * non-home state. Layer only once the strip has settled. See OVL_LAYERS.md. */
		/* The home carousel is ALWAYS OVL-layered now: the card strip lives on the L2
		 * layer and is PANNED by the OVL src_x during a slide (cc_signature excludes
		 * cont_shift, so a slide does not rebuild). No idle<->movement mode switch, so
		 * no flicker; movement is a hardware pan. Every non-home state stays single-buffer. */
		layered = (s_menu.state == 0 && s_menu.open_y == 0.0f && !s_menu.closing);
		t.ovl_split = layered;
#ifdef AYANEO_BIGCORE_EXPT
		/* Split the render across cpu0 + the cached worker when it is up and the
		 * chrome cache is already built (so the two cores never build it at once).
		 * Falls back to single-core otherwise. */
		if (bc_worker_ready() && s_menu.chrome_ready) {
			if (!s_bc_clk_set && BC_MHZ != 2000) { ayaneo_set_cpu_mhz(BC_MHZ); }
			s_bc_clk_set = 1;
			bc_dispatch(fb, pitch, (int)W, (int)H, &s_menu, &t);
		} else
#endif
			snes_menu_render(&s_menu, &t);
		s_perf_render_us = (gpt4_get_current_tick() - t_frame0) / 13u;
		draw_osd(fb, pitch, (int)W);
		draw_perf(fb, pitch);

		/* start any queued one-shot SFX, then mix a frame's worth of audio
		 * and push it to the AFE ring (keeps the ring fed ahead of the DMA) */
		{
			uint32_t h;
			int need;
			while ((h = snes_menu_next_sound(&s_menu)) != 0)
				play_sound(h, 0, 0);
			/* self-clocked: top the ring up to its target lead over the DMA
			 * read cursor, so 15-20 fps can't starve it into replaying */
			need = ayaneo_snes_audio_room();
			if (need > 16384) need = 16384;
			if (need > 0) {
				snes_audio_mix(&s_mix, s_mixbuf, (unsigned)need);
				ayaneo_snes_audio_submit(s_mixbuf, (unsigned)need);
			}
		}

		{
			unsigned t_pre = gpt4_get_current_tick();
			/* Skip the single-buffer path's explicit post-swap vsync wait when this
			 * frame fit in a vsync (config_input's FRAME_DONE wait then already syncs);
			 * only overrunning frames (moving carousel, resume) need it. This is what
			 * restores submenus to 60fps (their render is well under a frame). */
			ayaneo_present_skip_vsync(((t_pre - t_frame0) / 13u) < 15000u);
			if (layered) {
				unsigned l2_size = (unsigned)SNES_L2_W * SNES_L2_BAND_H * 4u;
				unsigned l3_size = pitch * H * 4u;
				uint32_t sig = snes_menu_cardcache_sig(&s_menu);
				unsigned int l2_live, l3_live;
				int rebuilt = 0, l2_pan;
				float vscale = s_menu.aspect ? ASP_CONTENT_S_DRV : 1.0f;
				float save_cont_shift = s_menu.cont_shift;
#ifdef AYANEO_DEBUG_LOGGING
				unsigned t_ph0 = gpt4_get_current_tick(), t_ph1, t_ph2, t_ph3;
#endif
				/* Rebuild the SETTLED wide strip only when the card ORDER changed (a
				 * nav): cc_signature excludes cont_shift/xfade, so a slide leaves it
				 * unchanged and is served by the src_x pan below - no per-frame rebuild. */
				/* On RE-ENTERING layered (from submenu/resume) do NOT force a rebuild: the
				 * L2 buffer at 0x54000000 is untouched by the single-buffer states and the
				 * carousel focus cannot change there, so its cards are still valid - reuse
				 * them (sig-checked) and just re-enable the layer. Kills the ~30ms rebuild
				 * that made the re-entry frame a 66ms freeze (the transition flicker). */
				if (!s_cc_valid || sig != s_cc_sig) {
					snes_target ct = {0};
					ct.fb = (unsigned int *)(unsigned long)
						(SNES_OVL_L2_PA + (s_l2_flip ? 0u : l2_size));
					ct.W = SNES_L2_W; ct.H = SNES_L2_BAND_H; ct.pitch = SNES_L2_W;
					ct.offx = SNES_L2_MARGIN;
					ct.offy = (s_menu.aspect ? 0 : ((int)H - SNES_VH) / 2) - SNES_L2_BAND_Y0;
					snes_menu_build_cardcache(&s_menu, &ct);
					s_l2_flip ^= 1; s_cc_sig = sig; s_cc_valid = 1; rebuilt = 1;
				}
#ifdef AYANEO_DEBUG_LOGGING
				t_ph1 = gpt4_get_current_tick();
#endif
				l2_live = SNES_OVL_L2_PA + (s_l2_flip ? l2_size : 0u);
				/* Pan the strip by the live cont_shift: screen px = cont_shift*viewscale,
				 * src_x = MARGIN - that (reads further left in the wide buffer to shift the
				 * cards right); it settles to MARGIN (= idle cache) as cont_shift -> 0. The
				 * OVL src_x is INTEGER, so quantise the shift to whole panel pixels: an
				 * integer src_x preserves the settled buffer's sub-pixel phase, whereas a
				 * fractional shift would land the panned boxart on a different phase than a
				 * fresh render (visible sampling shimmer, worst in 4:3 where the 1.185x
				 * content zoom magnifies it). L3 is then rendered at the SAME quantised
				 * shift (cont_shift_q) so the focused card tracks the strip exactly. */
				{
					int shift_px = (int)(s_menu.cont_shift * vscale +
						(s_menu.cont_shift >= 0.0f ? 0.5f : -0.5f));
					l2_pan = SNES_L2_MARGIN - shift_px;
					s_menu.cont_shift = (float)shift_px / vscale;   /* quantised; restored after L3 */
				}
				/* Render the colour-pulsing selection cursor + focused card into the L3
				 * back buffer every frame at the (quantised) live focused-card position. */
				{
					snes_target curt = {0};
					curt.fb = (unsigned int *)(unsigned long)
						(SNES_OVL_L3_PA + (s_l3_flip ? 0u : l3_size));
					curt.W = (int)W; curt.H = (int)H; curt.pitch = pitch;
					curt.offx = ((int)W - SNES_VW) / 2; curt.offy = ((int)H - SNES_VH) / 2;
					snes_menu_render_cursor_layer(&s_menu, &curt, !s_was_layered);
					s_l3_flip ^= 1;
				}
				s_menu.cont_shift = save_cont_shift;   /* restore the true animation value */
#ifdef AYANEO_DEBUG_LOGGING
				t_ph2 = gpt4_get_current_tick();
#endif
				l3_live = SNES_OVL_L3_PA + (s_l3_flip ? l3_size : 0u);
				/* Decide the layered present vsync sync from the WHOLE frame CPU cost (render
				 * + L2 build + L3 cursor), measured just before the swap. Fit a vsync (steady
				 * idle/pan) -> skip the explicit wait, config_input FRAME_DONE syncs it (60fps);
				 * overran (L2 rebuild OR a live crossfade render in movement) -> wait so the swap
				 * lands on vblank not mid-scanout. The old render-only skip missed the cursor +
				 * build cost, so overrunning movement pan frames tore (the flicker). */
				ayaneo_present_skip_vsync(((gpt4_get_current_tick() - t_frame0) / 13u) < 15000u);
				ayaneo_canvas_present_layers(l2_live, l2_pan, rebuilt,
							     l3_live, SNES_CURSOR_Y0, SNES_CURSOR_Y1);
#ifdef AYANEO_DEBUG_LOGGING
				t_ph3 = gpt4_get_current_tick();
				snes_present_log(rebuilt, (t_ph1 - t_ph0)/13u, (t_ph2 - t_ph1)/13u,
						 (t_ph3 - t_ph2)/13u);
#endif
				s_was_layered = 1;
			} else if (s_was_layered) {
				/* leaving the layered state (opening a submenu): disable the upper OVL
				 * layers ONCE, at a frame boundary, so no stale cards/cursor linger. */
				ayaneo_canvas_present_layers(0u, 0, 0, 0u, 0, 0);
				s_was_layered = 0;
				/* keep s_cc_valid: the L2 cards stay valid across the excursion (nothing
				 * writes 0x54000000 in the single-buffer states, focus cannot change), so
				 * re-entry reuses them (no 30ms rebuild freeze). sig-checked on return. */
			} else {
				/* steady single-buffer state (moving carousel, submenu, resume): the
				 * plain present, exactly the tear-free pre-layering path - it never
				 * touches the (already-disabled) L2/L3 layers. */
				ayaneo_canvas_present();
			}
			s_perf_present_us = (gpt4_get_current_tick() - t_pre) / 13u;
#ifdef AYANEO_DEBUG_LOGGING
			/* Transition capture: when the layered<->single-buffer mode flips (idle <-> menubar/
			 * submenu, or the movement rebuild pattern), log the NEXT few frames UNTHROTTLED so the
			 * exact per-frame rend/present timing across the boundary is visible (the flicker the
			 * user sees at the switch). Rare, so it does not spam. */
			{
				static int s_prev_lay = -1, s_burst;
				if (layered != s_prev_lay) { s_burst = 8; s_prev_lay = layered; }
				if (s_burst > 0) {
					s_burst--;
					_dprintf("SNESX lay=%d st=%d openy=%d rend=%uus pres=%uus tot=%uus\n",
						 layered, s_menu.state, (int)s_menu.open_y, s_perf_render_us,
						 s_perf_present_us, s_perf_render_us + s_perf_present_us);
				}
			}
			/* single-buffer (non-layered) states: log render + phase breakdown so the
			 * render-bound suspend/submenu costs are in the log too (worst per 30 frames). */
			if (!layered) {
				static unsigned nn, wtot, wr2, wp2, wg[5];
				unsigned tot = s_perf_render_us + s_perf_present_us, i;
				if (tot >= wtot) {
					wtot = tot; wr2 = s_perf_render_us; wp2 = s_perf_present_us;
					for (i = 0; i < 5; i++) wg[i] = g_perf[i] / 13u;
				}
				if (++nn >= 90u) {
					_dprintf("SNESN st=%d rend=%uus pres=%uus phase{wp=%u ch=%u car=%u fs=%u ot=%u}\n",
						 s_menu.state, wr2, wp2, wg[0], wg[1], wg[2], wg[3], wg[4]);
					nn = 0; wtot = 0;
				}
			}
#endif
		}
		mtk_wdt_restart();
		{
			static int armed;
			int p = pmic_detect_powerkey();
			if (!p) armed = 1; else if (armed) mt_power_off();
		}
	}
	return 0;
}

/* ---- entry points the boot/charging hooks call ---- */
void ayaneo_gbc_start(void)
{
	thread_t *t = thread_create("ayaneo_snes", &snes_emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t) thread_resume(t);
}
void ayaneo_gbc_charging_screen(void) { }
int  ayaneo_gbc_select_held(void) { return 0; }
