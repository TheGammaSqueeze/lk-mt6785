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

#ifndef __ASSEMBLER__
struct bc_comms {
	/* line 0 */
	volatile unsigned magic;                 /* 0  */
	volatile unsigned _r0;                    /* 4  */
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
};
#endif

#endif /* BIGCORE_COMMS_H */
