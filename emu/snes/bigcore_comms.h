/*
 * Shared big<->little comms block for the multicore experiment. Included by both
 * bigcore.c (C struct) and bigcore_entry.S (byte offsets). Keep the two in exact
 * sync: every field is a 32-bit word, offsets are hand-assigned to 64 B lines.
 *
 * The block lives at BC_COMMS_PA (0x54000000), Normal-WB DRAM.
 *
 * Coherency discipline (A55 DSU, HW-managed SMP coherency):
 *  - Line 0: cpu0 writes the MMU snapshot (then cache-cleans) so the MMU-OFF
 *    secondary can read it uncached from DRAM; the secondary also drops its
 *    "magic" (reached-the-stub) marker here with an UNCACHED store, before it
 *    turns the MMU on. Only uncached traffic touches line 0.
 *  - Line 4 (cached_ok/counter): the secondary writes these AFTER enabling MMU +
 *    caches (cached, Inner-Shareable), so they must NOT share a line with the
 *    pre-MMU uncached stores (avoids cached/uncached aliasing on one line).
 *  - go / done sit on their own lines (no false sharing during the fork/join).
 */
#ifndef BIGCORE_COMMS_H
#define BIGCORE_COMMS_H

#define BC_COMMS_PA   0x54000000u
#define BC_MAGIC      0xB16C0DE5u

/* line 0 (0..63): pre-MMU - cpu0 MMU snapshot + secondary reached-marker */
#define BC_O_MAGIC        0    /* secondary (uncached): reached the stub */
#define BC_O_STAGE        4    /* worker progress marker (debug: where it wedges) */
#define BC_O_TTBR0_LO     8    /* MMU snapshot from cpu0 (read MMU-off) */
#define BC_O_TTBR0_HI    12
#define BC_O_TTBCR       16
#define BC_O_MAIR0       20
#define BC_O_MAIR1       24
#define BC_O_DACR        28
#define BC_O_SCTLR       32    /* cpu0 SCTLR (A/TRE/AFE bits to match) */
#define BC_O_STACK_TOP   36    /* worker stack top (grows down), DRAM WB */
/* line 1/2: fork-join flags, each on its own 64B line */
#define BC_O_GO          64    /* cpu0 -> worker: frame seq to render */
#define BC_O_DONE       128    /* worker -> cpu0: frame seq completed */
/* line 3 (192): job payload published by cpu0 (read-only to worker) */
#define BC_O_JOB        192
/* line 4 (256): post-MMU cached heartbeat written by the secondary */
#define BC_O_CACHED_OK  256    /* secondary (cached): MMU+caches enabled OK */
#define BC_O_COUNTER    260    /* secondary (cached) heartbeat */
/* line 5 (320): worker fault capture (its exception handler records these) */
#define BC_O_FAULT_TYPE 320    /* 1=undef 2=prefetch-abort 3=data-abort */
#define BC_O_FAULT_FAR  324    /* faulting address (DFAR/IFAR) */
#define BC_O_FAULT_FSR  328    /* fault status (DFSR/IFSR) */
#define BC_O_FAULT_PC   332    /* faulting instruction PC */
#define BC_O_FAULT_SPSR 336    /* SPSR at fault (mode/state) */
/* line 6 (384): worker CPU-state snapshot captured just before the render */
#define BC_O_W_MPIDR    384
#define BC_O_W_SCTLR    388
#define BC_O_W_CPACR    392
#define BC_O_W_FPEXC    396
#define BC_O_PAR_LO     400    /* PAR after AT-translating the comms VA (attrs/SH) */
#define BC_O_PAR_HI     404
#define BC_O_W_MENU     408    /* menu_ptr the worker actually READ from comms (coherency probe) */
#define BC_O_W_MENUW0   412    /* first word the worker read THROUGH that pointer (real data vs garbage) */
#define BC_O_W_CAN1     416    /* what the worker read for cpu0's canary in a NON-comms cacheable region */
#define BC_O_W_CANPAR   420    /* worker PAR-hi of the canary VA 0x51000000 (attrs it actually sees) */
/* Lever-1 decisive probe: isolate the worker's OWN MMU-on load path from cross-core
 * coherency. Stale readback here => LK-local MMU/TLB bug, not a snoop-admission wall. */
#define BC_O_W_SELF_WB    424  /* worker WB-cacheable self write->clean->inval->readback */
#define BC_O_W_SELF_DEV   428  /* worker Device self write->readback at 0x51000080 */
#define BC_O_W_CANPAR_LO  432  /* worker PAR-lo (bit0=F, +PA) of failing canary VA 0x51000000 */
#define BC_O_W_SELFPAR_LO 436  /* worker PAR-lo of the WB self VA */
#define BC_O_W_CAN_MMUOFF 440  /* worker read of 0x51000000 with MMU turned OFF (post MMU-on) */
#define BC_O_W_STATIC_CAN 444  /* worker read of a PRE-bringup static value (producer-offload viability) */
#define BC_O_WARM_CYCLE   448  /* cpu0->worker: nonzero = self PSCI CPU_OFF on first bringup (warm-cycle expt) */
#define BC_O_W_SGI_COUNT  452  /* worker->cpu0: count of GIC SGI-pending signals the worker saw via MMIO */
#define BC_O_W_SPM_SCRATCH 456 /* worker->cpu0: worker MMIO read of an SPM scratch reg cpu0 writes each frame */

#ifndef __ASSEMBLER__
struct bc_comms {
	/* line 0 */
	volatile unsigned magic;                 /* 0  */
	volatile unsigned stage;                  /* 4  worker progress marker */
	volatile unsigned ttbr0_lo;              /* 8  */
	volatile unsigned ttbr0_hi;              /* 12 */
	volatile unsigned ttbcr;                 /* 16 */
	volatile unsigned mair0;                 /* 20 */
	volatile unsigned mair1;                 /* 24 */
	volatile unsigned dacr;                  /* 28 */
	volatile unsigned sctlr;                 /* 32 */
	volatile unsigned stack_top;             /* 36 */
	volatile unsigned _r1[6];                /* 40..63 */
	/* line 1 */
	volatile unsigned go;                    /* 64 */
	volatile unsigned _rg[15];
	/* line 2 */
	volatile unsigned done;                  /* 128 */
	volatile unsigned _rd[15];
	/* line 3: job payload */
	volatile unsigned fb, pitch, W, H, offx, offy;   /* 192.. */
	volatile unsigned band_y0, band_y1;
	volatile unsigned vsx, vsy, vdx, vdy;
	volatile unsigned menu_ptr, seq;
	volatile unsigned _rj[2];                /* -> 256 */
	/* line 4: post-MMU cached heartbeat */
	volatile unsigned cached_ok;             /* 256 */
	volatile unsigned counter;               /* 260 */
	volatile unsigned _rc[14];               /* -> 320 */
	/* line 5: worker fault capture */
	volatile unsigned fault_type;            /* 320 */
	volatile unsigned fault_far;             /* 324 */
	volatile unsigned fault_fsr;             /* 328 */
	volatile unsigned fault_pc;              /* 332 */
	volatile unsigned fault_spsr;            /* 336 */
	volatile unsigned _rf[11];               /* -> 384 */
	/* line 6: worker CPU-state snapshot (captured just before the render) */
	volatile unsigned w_mpidr;               /* 384 */
	volatile unsigned w_sctlr;               /* 388 */
	volatile unsigned w_cpacr;               /* 392 */
	volatile unsigned w_fpexc;               /* 396 */
	volatile unsigned par_lo;                /* 400 */
	volatile unsigned par_hi;                /* 404 */
	volatile unsigned w_menu;                /* 408 menu_ptr the worker read */
	volatile unsigned w_menuw0;              /* 412 first word read through it */
	volatile unsigned w_can1;                /* 416 worker's read of cpu0's non-comms canary */
	volatile unsigned w_canpar;              /* 420 worker PAR-hi of canary VA (attrs it sees) */
	volatile unsigned w_self_wb;             /* 424 worker WB self readback (Lever1) */
	volatile unsigned w_self_dev;            /* 428 worker Device self readback (Lever1) */
	volatile unsigned w_canpar_lo;           /* 432 worker PAR-lo of failing canary VA (F+PA) */
	volatile unsigned w_selfpar_lo;          /* 436 worker PAR-lo of the WB self VA (F+PA) */
	volatile unsigned w_can_mmuoff;          /* 440 worker read of canary with MMU OFF (post MMU-on) */
	volatile unsigned w_static_can;          /* 444 worker read of a pre-bringup static value */
	volatile unsigned warm_cycle;            /* 448 cpu0->worker: self PSCI CPU_OFF request (warm-cycle) */
	volatile unsigned w_sgi_count;           /* 452 worker->cpu0: GIC SGI-pending signals seen via MMIO */
	volatile unsigned w_spm_scratch;         /* 456 worker->cpu0: worker MMIO read of SPM scratch cpu0 wrote */
};
#endif

#endif /* BIGCORE_COMMS_H */
