/*
 * GammaOS Pico: a second CPU core in the LK. Bring up work, driven over USB:
 *
 *   oem smp            where the boot core stands: mode, ids, MMU setup, the power controller
 *   oem smp:arm        ready the CPU power controller through the firmware's kernel start service
 *                      (pico_smp_asm.S), and report what came back
 *   oem smp:on,N       power core N on (PSCI CPU_ON) into the entry stub
 *   oem smp:st         what the started core reports
 *   oem smp:ping,HEX   hand the started core a word (cleaned out of the cache), :pingd without
 *   oem smp:r,ADDR,N   N words of a register block
 *
 * A call that never returns leaves the menu thread unable to feed the watchdog, which then
 * restarts the console by itself.
 */
#include <debug.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <arch/ops.h>
#include <kernel/thread.h>
#include <arch/arm/mmu.h>
#include <platform/mt_typedefs.h>
#include "pico_smp.h"

/* The SMP shared buffers live in the 1 MB of DRAM right after the LK's own 9 MB (0x4CD00000, a
 * free memory block on both consoles, inside the LK's section mapping, used by nothing else), NOT
 * in the LK's BSS: the LK has only ~0.5 MB of heap above _end and these 4 KB aligned buffers would
 * eat it and stop it booting. Not the emulator scratch at 0x4E000000 either: closing or starting a
 * game writes over that. cpu0 and the workers both reach them by physical address. */
#define SMP_SCRATCH   0x4CDC0000u
#define g_pico_smp_save  ((volatile unsigned *)(SMP_SCRATCH + 0x4000u))      /* 256 B, must match pico_smp_asm.S */
#define g_pico_smp_pt    ((volatile unsigned long long *)(SMP_SCRATCH + 0x1000u))
#define s_l2             ((volatile unsigned long long *)(SMP_SCRATCH + 0x2000u))
#define s_l3             ((volatile unsigned long long *)(SMP_SCRATCH + 0x3000u))
#define s_diag           ((volatile unsigned *)(SMP_SCRATCH + 0x5000u))

extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern void fastboot_info(const char *reason);
extern void fastboot_register(const char *prefix, void (*handle)(const char *arg, void *data, unsigned sz),
			      unsigned char security_enabled, unsigned char requires_unlock);

extern unsigned pico_smp_arm_call(unsigned *save, unsigned *diag, unsigned fid, unsigned k64);
extern void pico_smp_sec_entry(void);
extern void pico_smp_hyp_back(void);
extern void pico_smp_kboot(unsigned entry, unsigned a1, unsigned a2, unsigned width);
extern void pico_smp_a32_land(void);
extern void pico_smp_a64_land(void);
extern void pico_smp_coh_land(void);
extern void pico_smp_elsw_land(void);
extern void pico_smp_gba_land(void);
extern void pico_smp_clk_land(void);
extern signed int pwrap_read(unsigned int adr, unsigned int *rdata);
extern signed int pwrap_write(unsigned int adr, unsigned int wdata);
extern void udelay(unsigned long usec);
static unsigned rd32(unsigned a);

/* The big cluster's clock and supply, per console.
 *
 * Clock: the cluster's own PLL in APMIXED, f = 26 MHz * PCW / 2^14 / 2^POSDIV (CON1: PCW bits
 * 21..0, POSDIV bits 26..24, bit 31 latches a new PCW).
 * Supply: a PMIC buck whose target code is written and whose present code is read back; the
 * cluster's memory supply follows the buck by itself (the PMIC's tracking, on at boot, 100 mV
 * above). The tables are the consoles' own stock levels (the Pocket Advance's from its kernel, the
 * Pocket Block's from its device tree): nothing here goes above the stock top level or below the
 * level the console booted at.
 *
 *                      Pocket Advance (MT6785 + MT6359)     Pocket Block (MT6768 + MT6358)
 *   PLL CON1           0x1000C214                           0x1000C20C
 *   buck               VPROC1: set 0x172e, now 0x1726       VPROC11: set 0x13a6, now 0x139e
 *   buck voltage       0.4 V + code * 6.25 mV               0.5 V + code * 6.25 mV
 *   memory supply now  0x1e8e bits 14..8 (0.5 V base)       0x1b60 bits 14..8 (0.5 V base)
 * (the little cluster and the bus are on the other buck, VPROC2 / VPROC12: never touched here) */
#if defined(KONKR_BLOCK)
#define BIG_PLL_CON1   0x1000C20Cu
#define BIG_VPROC_SET  0x13a6u
#define BIG_VPROC_NOW  0x139eu
#define BIG_VSRAM_NOW  0x1b60u
#define BIG_VBASE_MV   500u
#define BIG_CODE_MAX   94u
#define BIG_CODE_FLOOR 48u        /* 0.8 V, as the preloader leaves the buck */
/* The Block's own clock code for the boot core (block_pico.c) sets both CPU bucks alike. The big
 * cluster's buck therefore carries the higher of the two wishes: what the boot core's clock asks
 * for (read from the other buck) and what the big core's clock needs (g_pico_big_rail_code, which
 * block_pico.c honours when it moves the bucks). */
#define BIG_PEER_NOW   0x141eu
#define BIG_MHZ_MAX    2000u
#define BIG_MENU_FIRST 4          /* the menu offers the levels from 1532 MHz up */
/* the divider after the PLL: by one from this clock up, by two below (the PLL's own range) */
#define BIG_POSDIV_FOR(mhz) ((mhz) >= 1600u ? 0u : 1u)
static const struct { unsigned short mhz; unsigned char code; } k_big_opp[] = {
	{ 1176, 48 }, { 1295, 55 }, { 1354, 59 }, { 1443, 64 }, { 1532, 69 }, { 1621, 75 },
	{ 1710, 80 }, { 1800, 85 }, { 1850, 88 }, { 1900, 90 }, { 1950, 92 }, { 2000, 94 },
};
#else
#define BIG_PLL_CON1   0x1000C214u
#define BIG_VPROC_SET  0x172eu
#define BIG_VPROC_NOW  0x1726u
#define BIG_VSRAM_NOW  0x1e8eu
#define BIG_VBASE_MV   400u
#define BIG_CODE_MAX   115u
#define BIG_CODE_FLOOR 64u        /* 0.8 V, as the console boots */
#define BIG_MHZ_MAX    2050u
#define BIG_MENU_FIRST 3          /* the menu offers the levels from 1530 MHz up */
static const struct { unsigned short mhz; unsigned char code; } k_big_opp[] = {
	{ 1169, 64 }, { 1308, 71 }, { 1419, 76 }, { 1530, 82 }, { 1670, 89 }, { 1733, 93 },
	{ 1796, 98 }, { 1860, 102 }, { 1923, 106 }, { 1986, 111 }, { 2050, 115 },
};
#endif
volatile unsigned g_pico_big_rail_code;   /* the code the big core's clock needs now; 0 = no wish */

static unsigned pico_big_get_mhz(void)
{
	unsigned c1 = rd32(BIG_PLL_CON1), pcw = c1 & 0x3FFFFFu, posdiv = (c1 >> 24) & 7u;
	unsigned long long f = 26000000ull * pcw; f >>= 14; f >>= posdiv;
	return (unsigned)(f / 1000000ull);
}
static void pico_big_set_mhz(unsigned mhz)
{
	volatile unsigned *con1 = (volatile unsigned *)BIG_PLL_CON1;
	unsigned c = *con1, posdiv = (c >> 24) & 7u, want = posdiv;
	unsigned long long pcw;
	if (mhz < 400u) mhz = 400u;
	if (mhz > BIG_MHZ_MAX) mhz = BIG_MHZ_MAX;
#ifdef BIG_POSDIV_FOR
	want = BIG_POSDIV_FOR(mhz);
	if (want > posdiv) {            /* a larger divider first: the clock only ever steps down on the way */
		c = (c & ~0x87000000u) | (want << 24);
		*con1 = c; udelay(20);
		posdiv = want;
	}
#endif
	pcw = ((unsigned long long)mhz * 1000000ull) << 14; pcw <<= want; pcw /= 26000000ull;
	if (pcw > 0x3FFFFFu) pcw = 0x3FFFFFu;
	c = (c & ~0x003FFFFFu & ~0x80000000u) | (unsigned)pcw;
	*con1 = c; udelay(2); *con1 = c | 0x80000000u; udelay(20);   /* PCW, then the latch bit: relock */
	if (want < posdiv) {            /* a smaller divider last: the PLL already runs the new rate */
		c = (c & ~0x87000000u) | (want << 24);
		*con1 = c; udelay(20);
	}
}
static unsigned pico_big_get_code(void)
{
	unsigned v = 0;
	pwrap_read(BIG_VPROC_NOW, &v);
	return v & 0x7Fu;
}
static void pico_big_set_code(unsigned code)
{
	unsigned cur = 0, now = pico_big_get_code();
	if (code < BIG_CODE_FLOOR) code = BIG_CODE_FLOOR;
	if (code > BIG_CODE_MAX) code = BIG_CODE_MAX;
	g_pico_big_rail_code = code;
#ifdef BIG_PEER_NOW
	{ unsigned peer = 0; pwrap_read(BIG_PEER_NOW, &peer); peer &= 0x7Fu; if (code < peer) code = peer; }
#endif
	pwrap_read(BIG_VPROC_SET, &cur);
	pwrap_write(BIG_VPROC_SET, (cur & ~0x7Fu) | code);
	/* the buck slews about 10 mV per microsecond up, slower down; the memory supply follows it */
	udelay(200u + (code > now ? code - now : now - code) * 2u);
}
/* the stock voltage code for a clock: the first level at or above it */
static unsigned pico_big_code_for(unsigned mhz)
{
	unsigned i;
	for (i = 0; i < sizeof k_big_opp / sizeof k_big_opp[0]; i++)
		if (k_big_opp[i].mhz >= mhz) return k_big_opp[i].code;
	return BIG_CODE_MAX;
}
/* Clock and voltage together, in the safe order: the voltage first on the way up, the clock first
 * on the way down. The clock is capped at the stock top level. */
static void pico_big_set_opp(unsigned mhz)
{
	unsigned code, now = pico_big_get_code();
	if (mhz > BIG_MHZ_MAX) mhz = BIG_MHZ_MAX;
	code = pico_big_code_for(mhz);
	if (code < BIG_CODE_FLOOR) code = BIG_CODE_FLOOR;
	if (code >= now) { pico_big_set_code(code); pico_big_set_mhz(mhz); }
	else { pico_big_set_mhz(mhz); pico_big_set_code(code); }
}
volatile int g_pico_smp_gba_req;      /* opt in: the next GBA game runs its CPU loop on a big core */
volatile unsigned g_pico_smp_gba_core = 6u;
volatile unsigned g_pico_smp_gba_mhz = BIG_MHZ_MAX;   /* the big core's clock while a GBA guest runs: the stock top
                                                 * level, at its stock voltage; 0 leaves it as is */
static unsigned s_big_saved_mhz, s_big_saved_code;   /* the big cluster as the boot left it */
extern void mtk_wdt_restart(void);


#define MCUCFG_CPC_FLOW  0x0C53A814u
#define MCUCFG_CPC_SPMC  0x0C53A840u
#define SPM_BASE         0x10006000u
#define SIP_KERNEL_BOOT  0x82000115u
#define PSCI_CPU_ON      0x84000003u
#define PSCI_VERSION     0x84000000u

static char sl[96];

static unsigned rd32(unsigned a) { return *(volatile unsigned *)a; }

static unsigned hexarg(const char **pp)
{
	const char *p = *pp;
	unsigned v = 0;
	while (*p == ',' || *p == ' ' || *p == ':') p++;
	for (;; p++) {
		unsigned c = (unsigned char)*p, d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		v = (v << 4) | d;
	}
	*pp = p;
	return v;
}

static unsigned s_smc_r[4];
static volatile int s_started;   /* a core was asked to power on since boot */
static unsigned smc4(unsigned fid, unsigned a1, unsigned a2, unsigned a3)
{
	register unsigned r0 __asm__("r0") = fid;
	register unsigned r1 __asm__("r1") = a1;
	register unsigned r2 __asm__("r2") = a2;
	register unsigned r3 __asm__("r3") = a3;
	__asm__ volatile(".arch_extension sec\n smc #0"
			 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r4", "r5", "r6", "r7", "r12", "memory");
	s_smc_r[0] = r0; s_smc_r[1] = r1; s_smc_r[2] = r2; s_smc_r[3] = r3;
	if (fid == PSCI_CPU_ON) s_started = 1;
	return r0;
}

/* Marks that outlive a restart: the tail of the SRAM the RAM console sits in (device memory in
 * the LK's map, so nothing is cached). */
#if defined(KONKR_BLOCK)
#define SMP_SRAM 0x0010EF00u   /* Block: RAM console SRAM 0x0010EA00 + 0x540, its last 64 bytes */
#else
#define SMP_SRAM 0x0011D7C0u   /* KPA: RAM console SRAM 0x0011D000 + 0x800, its last 64 bytes */
#endif
#define BC ((volatile unsigned *)SMP_SRAM)
static void bc_map(void) { }
static void bc_set(unsigned v) { BC[0] = v; }

static void info(void)
{
	unsigned cpsr, midr, mpidr, sctlr, ttbcr, pfr1, t0l, t0h, mair0, mair1, i;
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	__asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
	__asm__ volatile("mrc p15, 0, %0, c0, c1, 1" : "=r"(pfr1));
	__asm__ volatile("mrrc p15, 0, %0, %1, c2" : "=r"(t0l), "=r"(t0h));
	__asm__ volatile("mrc p15, 0, %0, c10, c2, 0" : "=r"(mair0));
	__asm__ volatile("mrc p15, 0, %0, c10, c2, 1" : "=r"(mair1));
	snprintf(sl, sizeof sl, "cpsr %08x midr %08x mpidr %08x pfr1 %08x", cpsr, midr, mpidr, pfr1);
	fastboot_info(sl);
	snprintf(sl, sizeof sl, "sctlr %08x ttbcr %08x ttbr0 %08x:%08x", sctlr, ttbcr, t0h, t0l);
	fastboot_info(sl);
	snprintf(sl, sizeof sl, "mair %08x %08x psci %08x", mair0, mair1, smc4(PSCI_VERSION, 0, 0, 0));
	fastboot_info(sl);
	snprintf(sl, sizeof sl, "cpc flow %08x spmc %08x wdt mode %08x", rd32(MCUCFG_CPC_FLOW),
		 rd32(MCUCFG_CPC_SPMC), rd32(0x10007000u));
	fastboot_info(sl);
	snprintf(sl, sizeof sl, "spm en %08x pwrsta %08x", rd32(SPM_BASE), rd32(SPM_BASE + 0x160u));
	fastboot_info(sl);
	for (i = 0; i < 8u; i += 4u) {
		snprintf(sl, sizeof sl, "pwr_con cpu%u.. %08x %08x %08x %08x", i, rd32(SPM_BASE + 0x208u + i * 4u),
			 rd32(SPM_BASE + 0x20Cu + i * 4u), rd32(SPM_BASE + 0x210u + i * 4u),
			 rd32(SPM_BASE + 0x214u + i * 4u));
		fastboot_info(sl);
	}
	snprintf(sl, sizeof sl, "entry %08x back %08x comm %08x", (unsigned)pico_smp_sec_entry,
		 (unsigned)pico_smp_hyp_back, (unsigned)g_pico_smp_comm);
	fastboot_info(sl);
}

static void cmd_smp(const char *arg, void *data, unsigned sz)
{
	unsigned i;
	(void)data; (void)sz;
	while (*arg == ':' || *arg == ' ') arg++;
	if (!*arg) {
		info();
	} else if (!strncmp(arg, "k32", 3) || !strncmp(arg, "k64", 3)) {
		/* a faithful copy of the LK kernel handoff, landing at our own address */
		int k64 = arg[1] == '6';
		unsigned entry = k64 ? (unsigned)pico_smp_a64_land : (unsigned)pico_smp_a32_land;
		for (i = 0; i < 16u; i++) BC[i] = 0;
		arch_clean_cache_range(entry & ~63u, 256);
		arch_clean_invalidate_cache_range((addr_t)g_pico_smp_comm, 256);
		snprintf(sl, sizeof sl, "kboot %s entry %08x (watch bc 08 for 32/66)", k64 ? "64" : "32", entry);
		fastboot_info(sl);
		fastboot_okay("");
		enter_critical_section();
		if (k64) pico_smp_kboot(entry, (unsigned)g_pico_smp_comm, 0, 1);
		else     pico_smp_kboot(entry, 0, (unsigned)g_pico_smp_comm, 0);
		exit_critical_section();
		return;
	} else if (!strncmp(arg, "arm", 3)) {
		unsigned r;
		memset(s_diag, 0, sizeof s_diag);
		arch_clean_invalidate_cache_range((addr_t)s_diag, sizeof s_diag);
		bc_map();
		for (i = 0; i < 16u; i++) BC[i] = 0;
		bc_set(1);
		enter_critical_section();
		r = pico_smp_arm_call(g_pico_smp_save, s_diag, SIP_KERNEL_BOOT, arg[3] == '6');
		exit_critical_section();
		bc_set(6);
		arch_clean_invalidate_cache_range((addr_t)s_diag, sizeof s_diag);
		snprintf(sl, sizeof sl, "arm: %s ret %08x", r ? "call returned" : "back through hyp", s_diag[8]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "hyp cpsr %08x r0 %08x r1 %08x r2 %08x", s_diag[0], s_diag[1], s_diag[2], s_diag[3]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "hcr %08x hsctlr %08x sctlr %08x hcptr %08x", s_diag[4], s_diag[5], s_diag[6], s_diag[7]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "cpc flow %08x spmc %08x", rd32(MCUCFG_CPC_FLOW), rd32(MCUCFG_CPC_SPMC));
		fastboot_info(sl);
	} else if (!strncmp(arg, "stop", 4)) {
		volatile unsigned *c = g_pico_smp_comm;
		unsigned to;
		c[28] = 1u; arch_clean_cache_range((addr_t)c, 256);
		thread_sleep(300);
		c[28] = 0u; arch_clean_cache_range((addr_t)c, 256);
		snprintf(sl, sizeof sl, "stop: pwrsta %08x (siblings bits 10..16 clear = all off)", rd32(SPM_BASE + 0x160u));
		fastboot_info(sl);
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "stall", 5)) {
		/* test: the running guest stops answering at its next frame boundary (a tight loop), as
		 * a stalled game would; its frame watchdog has to cut it off and the game closes */
		arg += 5;
		g_pico_smp_comm[39] = hexarg(&arg) == 2u ? 2u : 1u;   /* ,2: an undefined instruction instead */
		fastboot_info("the guest stalls or faults at its next frame");
	} else if (!strncmp(arg, "a55", 3)) {
		/* the boot core's clock, as a game start and end change it (bring up tests) */
		extern void ayaneo_set_cpu_mhz(unsigned int mhz) __attribute__((weak));
		unsigned m; arg += 3; m = hexarg(&arg);
		if (ayaneo_set_cpu_mhz && m) ayaneo_set_cpu_mhz(m);
		snprintf(sl, sizeof sl, "A55 PLL con1 %08x", rd32(0x1000C204u));
		fastboot_info(sl);
	} else if (!strncmp(arg, "pmicw", 5)) {
		/* bring up: one CPU buck's target code by its register (MT6358 0x13a6 / 0x1426, MT6359
		 * 0x172e / 0x17b2), within the stock range only. To find which buck feeds which cluster. */
		unsigned a, v, cur = 0; arg += 5; a = hexarg(&arg); v = hexarg(&arg);
		if ((a != 0x13a6u && a != 0x1426u && a != 0x172eu && a != 0x17b2u) || v < 0x1cu || v > BIG_CODE_MAX) {
			fastboot_fail("pmicw: a CPU buck's code register and a stock code only");
			return;
		}
		pwrap_read(a, &cur);
		pwrap_write(a, (cur & ~0x7Fu) | v);
		udelay(500);
		pwrap_read(a, &cur);
		snprintf(sl, sizeof sl, "pmic %04x <- code %u, reads %04x", a, v, cur);
		fastboot_info(sl);
	} else if (!strncmp(arg, "pmic", 4)) {
		unsigned a, v = 0; arg += 4; a = hexarg(&arg);
		pwrap_read(a, &v);
		snprintf(sl, sizeof sl, "pmic %04x = %04x", a, v);
		fastboot_info(sl);
	} else if (!strncmp(arg, "bigv", 4)) {
		/* the big cluster's rail by its code alone (bring up); never above the stock top level */
		unsigned code, sr = 0; arg += 4;
		if (*arg == ':' || *arg == ',') { code = hexarg(&arg); pico_big_set_code(code); }
		code = pico_big_get_code();
		pwrap_read(BIG_VSRAM_NOW, &sr); sr = (sr >> 8) & 0x7Fu;
		snprintf(sl, sizeof sl, "big rail code %u (%u mV), memory rail code %u (%u mV)", code,
			 BIG_VBASE_MV + (code * 625u) / 100u, sr, 500u + (sr * 625u) / 100u);
		fastboot_info(sl);
	} else if (!strncmp(arg, "bigc", 4)) {
		/* the big cluster's clock alone, no voltage change (bring up) */
		unsigned m; arg += 4;
		if (*arg == ':' || *arg == ',') { m = hexarg(&arg); pico_big_set_mhz(m); }
		snprintf(sl, sizeof sl, "big clock ~%u MHz", pico_big_get_mhz());
		fastboot_info(sl);
	} else if (!strncmp(arg, "big", 3)) {
		/* read, or set (MHz, decimal digits read as hex are NOT meant: give hex), the big cluster's
		 * clock with its stock voltage */
		unsigned m, code; arg += 3;
		if (*arg == ':' || *arg == ',') { m = hexarg(&arg); pico_big_set_opp(m); }
		code = pico_big_get_code();
		snprintf(sl, sizeof sl, "big clock ~%u MHz at code %u (%u mV)", pico_big_get_mhz(), code,
			 BIG_VBASE_MV + (code * 625u) / 100u);
		fastboot_info(sl);
	} else if (!strncmp(arg, "noio", 4)) {
		/* load testing: GBA games from reset, nothing of theirs read from or written to the card */
		extern volatile int g_gba_test_noio __attribute__((weak));
		arg += 4;
		if (&g_gba_test_noio) {
			if (*arg == ':' || *arg == ',') g_gba_test_noio = hexarg(&arg) ? 1 : 0;
			snprintf(sl, sizeof sl, "GBA test mode (no saves, no states): %s", g_gba_test_noio ? "on" : "off");
		} else snprintf(sl, sizeof sl, "no GBA core in this build");
		fastboot_info(sl);
	} else if (!strncmp(arg, "mhz", 3)) {
		/* the clock a GBA guest's big core runs at (hex MHz); takes effect when the core is next set up */
		unsigned m; arg += 3;
		if (*arg == ':' || *arg == ',') { m = hexarg(&arg); if (m >= 1000u && m <= BIG_MHZ_MAX) g_pico_smp_gba_mhz = m; }
		snprintf(sl, sizeof sl, "guest clock %u MHz", g_pico_smp_gba_mhz);
		fastboot_info(sl);
	} else if (!strncmp(arg, "clk", 3)) {
		/* measure a core's clock: its own cycle counter against the 13 MHz architected timer */
		volatile unsigned *c = g_pico_smp_comm;
		unsigned n = 6u, to; unsigned long long cyc, tk;
		arg += 3; if (*arg==':'||*arg==',') { n = hexarg(&arg); } if (n<1u||n>7u) n=6u;
		for (i=0;i<16u;i++) BC[i]=0; for (i=18;i<28u;i++) c[i]=0;
		arch_clean_invalidate_cache_range((addr_t)c, 256);
		arch_clean_cache_range((unsigned)pico_smp_clk_land & ~63u, 256);
		BC[8] = (unsigned)g_pico_smp_comm;
		smc4(PSCI_CPU_ON, n<<8, (unsigned)pico_smp_clk_land, 0);
		for (to=0; to<8000000u && *(volatile unsigned*)(SMP_SRAM+8u)!=0xc0u; to++) ;
		arch_clean_invalidate_cache_range((addr_t)c, 256);
		cyc = ((unsigned long long)c[25]<<32)|c[24];
		tk  = ((unsigned long long)c[27]<<32)|c[26];
		snprintf(sl, sizeof sl, "core %u midr %08x: %u cycles / %u ticks", n, c[18], (unsigned)cyc, (unsigned)tk);
		fastboot_info(sl);
		if (tk) { unsigned mhz = (unsigned)((cyc*13u)/tk); snprintf(sl, sizeof sl, "=> ~%u MHz", mhz); fastboot_info(sl); }
		c[28]=1; arch_clean_cache_range((addr_t)c,256); { extern void mdelay(unsigned long); mdelay(200); } c[28]=0; arch_clean_cache_range((addr_t)c,256);
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "gba", 3)) {
		/* opt in for the next GBA game: its CPU loop on one big core (gba:0 = off, gba:7 = core 7) */
		arg += 3;
		if (*arg == ':' || *arg == ',') { i = hexarg(&arg); g_pico_smp_gba_req = i != 0u; if (i >= 1u && i <= 7u) g_pico_smp_gba_core = i; }
		else g_pico_smp_gba_req = 1;
		if (!pico_smp_available()) g_pico_smp_gba_req = 0;
		snprintf(sl, sizeof sl, "gba on a big core: %s (core %u)", g_pico_smp_gba_req ? "next game" : "off", g_pico_smp_gba_core);
		fastboot_info(sl);
	} else if (!strncmp(arg, "gst", 3)) {
		volatile unsigned *c = g_pico_smp_comm;
		snprintf(sl, sizeof sl, "go %u done %u flushes %u fault %u mark %02x", c[24], c[27], c[36], c[37], BC[2]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "esr %08x elr %08x far %08x spsr %08x", c[32], c[33], c[34], c[35]);
		fastboot_info(sl);
		{ extern volatile unsigned int g_dbg_smp_deaths, g_dbg_smp_recovered;
		  snprintf(sl, sizeof sl, "guest stopped %u recovered %u", g_dbg_smp_deaths, g_dbg_smp_recovered);
		  fastboot_info(sl); }
	} else if (!strncmp(arg, "elsw", 4)) {
		/* feasibility: can an AArch64 worker switch to AArch32 and run A32 code?
		 * BC[2]=0xa0 A64 reached; BC[5]=CurrentEL raw (4=EL1,8=EL2); BC[6]=0x32 if A32 ran. */
		volatile unsigned *mk = (volatile unsigned *)(SMP_SRAM + 8u); (void)mk;
		unsigned to;
		for (i = 0; i < 16u; i++) BC[i] = 0;
		arch_clean_cache_range((unsigned)pico_smp_elsw_land & ~63u, 512);
		smc4(PSCI_CPU_ON, 1u << 8, (unsigned)pico_smp_elsw_land, 0);
		for (to = 0; to < 2000000u; to++) if (BC[6] != 0u) break;
		snprintf(sl, sizeof sl, "elsw: a64 %08x EL %08x a32ran %08x", BC[2], BC[5], BC[6]);
		fastboot_info(sl);
		if (BC[6] == 0x32u) fastboot_info("RESULT: AArch32 switch WORKS - the existing A32 core can run on a big core");
		else fastboot_info("RESULT: AArch32 did not run (switch not reachable this way)");
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "coh", 3)) {
		unsigned cohn;
		/* true-coherency test: worker runs with MMU + caches on (Normal WB Inner-Shareable);
		 * cpu0 and worker exchange via barriers ONLY, no clean/invalidate. If the worker sees
		 * cpu0's cached canary, the DSU keeps them coherent and offload needs no per-use flush. */
		volatile unsigned *c = g_pico_smp_comm;
		volatile unsigned *mk = (volatile unsigned *)(SMP_SRAM + 8u);
		unsigned seen, cnt0, cnt1, to;
		for (i = 0; i < 512u; i++) g_pico_smp_pt[i] = 0;
		g_pico_smp_pt[0] = 0x405ull;                 /* 0x00000000 Device block */
		g_pico_smp_pt[1] = 0x40000000ull | 0x701ull; /* Normal WB Inner-Shareable */
		g_pico_smp_pt[2] = 0x80000000ull | 0x701ull;
		g_pico_smp_pt[3] = 0xC0000000ull | 0x701ull;
		arch_clean_cache_range((addr_t)g_pico_smp_pt, 4096);
		c[20] = c[24] = c[25] = c[26] = c[27] = 0;
		arch_clean_invalidate_cache_range((addr_t)c, 256);
		for (i = 0; i < 16u; i++) BC[i] = 0;
		arch_clean_cache_range((unsigned)pico_smp_coh_land & ~63u, 512);
		*(volatile unsigned *)(SMP_SRAM + 32u) = (unsigned)g_pico_smp_comm; /* BC[8] comm base */
		*(volatile unsigned *)(SMP_SRAM + 36u) = (unsigned)g_pico_smp_pt;   /* BC[9] page table */
		arg += 3; cohn = hexarg(&arg); if (cohn == 0u) cohn = 1u;
		/* stop any running worker: set STOP, wait for the power-status siblings to clear */
		c[28] = 1u; arch_clean_cache_range((addr_t)c, 256);
		thread_sleep(300);
		c[28] = 0u; c[20] = c[24] = c[25] = c[26] = c[27] = 0u;
		arch_clean_invalidate_cache_range((addr_t)c, 256);
		smc4(PSCI_CPU_ON, cohn << 8, (unsigned)pico_smp_coh_land, 0);
		for (to = 0; to < 4000000u && *mk != 0x89u; to++) ;
		cnt0 = c[20];                 /* coherent read, NO invalidate */
		c[25] = 0xC0FFEEu;            /* CANARY cached, left in cpu0 cache (no clean) */
		__asm__ volatile("dmb sy" ::: "memory");
		c[24] = 1u;                   /* GO cached (no clean) */
		__asm__ volatile("dmb sy" ::: "memory");
		for (to = 0; to < 8000000u && c[27] == 0u; to++) { if ((to & 0xFFFFFu) == 0u) mtk_wdt_restart(); }
		__asm__ volatile("dmb sy" ::: "memory");
		seen = c[26];
		cnt1 = c[20];
		snprintf(sl, sizeof sl, "coh core %u: marker %08x done %08x seen %08x (want C0FFEE)", cohn, *mk, c[27], seen);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "coh midr %08x mpidr %08x", c[18], c[19]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "coh heartbeat %08x -> %08x", cnt0, cnt1);
		fastboot_info(sl);
		if (seen == 0xC0FFEEu && c[27] == 1u)
			fastboot_info("RESULT: COHERENT - worker saw cpu0 cached write via DSU snoop, no flush");
		else if (*mk != 0x89u)
			fastboot_info("RESULT: worker did not reach MMU-on (marker != 0x89); MMU setup faulted");
		else
			fastboot_info("RESULT: NOT coherent by barriers alone (worker up but no snoop)");
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "on64", 4) || !strncmp(arg, "on32", 4)) {
		int a64 = arg[2] == '6';
		unsigned n, r, e;
		arg += 4;
		n = hexarg(&arg);
		e = a64 ? (unsigned)pico_smp_a64_land : (unsigned)pico_smp_a32_land;
		for (i = 0; i < 16u; i++) BC[i] = 0;
		arch_clean_cache_range(e & ~63u, 256);
		arch_clean_cache_range(e & ~63u, 256);
		arch_clean_invalidate_cache_range((addr_t)g_pico_smp_comm, 256);
		*(volatile unsigned *)(SMP_SRAM + 32u) = (unsigned)g_pico_smp_comm;  /* BC[8]: hand the worker its base */
		r = smc4(PSCI_CPU_ON, n << 8, e, a64 ? (unsigned)g_pico_smp_comm : (0x900u + n));
		snprintf(sl, sizeof sl, "cpu_on%s %u: %08x pwrsta %08x (bc 08->32/66)", a64 ? "64" : "32", n, r, rd32(SPM_BASE + 0x160u));
		fastboot_info(sl);
	} else if (!strncmp(arg, "on", 2)) {
		unsigned n, r;
		arg += 2;
		n = hexarg(&arg);
		memset(g_pico_smp_comm, 0, sizeof g_pico_smp_comm);
		arch_clean_invalidate_cache_range((addr_t)g_pico_smp_comm, sizeof g_pico_smp_comm);
		arch_clean_cache_range((addr_t)pico_smp_sec_entry & ~63u, 512);
		bc_set(0x10 + n);
		r = smc4(PSCI_CPU_ON, n << 8, (unsigned)pico_smp_sec_entry, 0x1234u + n);
		snprintf(sl, sizeof sl, "cpu_on %u: %08x pwrsta %08x", n, r, rd32(SPM_BASE + 0x160u));
		fastboot_info(sl);
	} else if (!strncmp(arg, "st", 2)) {
		volatile unsigned *c = g_pico_smp_comm;
		arch_clean_invalidate_cache_range((addr_t)c, 128);
		snprintf(sl, sizeof sl, "magic %08x midr %08x mpidr %08x EL %08x", c[16], c[18], c[19], c[22]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "sctlr %08x count %08x ping %08x pong %08x", c[23], c[20], c[0], c[21]);
		fastboot_info(sl);
	} else if (!strncmp(arg, "ping", 4)) {
		int dirty = arg[4] == 'd';
		arg += dirty ? 5 : 4;
		g_pico_smp_comm[0] = hexarg(&arg);
		if (!dirty) arch_clean_cache_range((addr_t)g_pico_smp_comm, 64);
		fastboot_info("ping set; oem smp:st to read pong (= ping+1 if the worker is live)");
	} else if (!strncmp(arg, "bc", 2)) {
		bc_map();
		for (i = 0; i < 16u; i += 4u) {
			snprintf(sl, sizeof sl, "bc %02x: %08x %08x %08x %08x", i * 4u, BC[i], BC[i + 1], BC[i + 2], BC[i + 3]);
			fastboot_info(sl);
		}
	} else if (!strncmp(arg, "atf", 3)) {
		smc4(0x8200010Au, 0, 0, 0);
		snprintf(sl, sizeof sl, "atf log %08x size %08x flag %08x %08x", s_smc_r[0], s_smc_r[1], s_smc_r[2], s_smc_r[3]);
		fastboot_info(sl);
	} else if (!strncmp(arg, "map", 3)) {
		extern int arch_mmu_map(uint64_t paddr, vaddr_t vaddr, uint flags, uint count);
		unsigned a, n;
		arg += 3;
		a = hexarg(&arg) & ~0xFFFu;
		n = (hexarg(&arg) + 0xFFFu) & ~0xFFFu;
		snprintf(sl, sizeof sl, "map %08x+%x: %d", a, n, arch_mmu_map((uint64_t)a, (vaddr_t)a,
			 MMU_MEMORY_TYPE_NORMAL_WRITE_BACK | MMU_MEMORY_AP_P_RW_U_NA, n));
		fastboot_info(sl);
	} else if (arg[0] == 't') {
		unsigned a, n, k;
		arg++;
		a = hexarg(&arg);
		n = hexarg(&arg);
		if (n > 0x400u) n = 0x400u;
		for (i = 0; i < n; ) {
			for (k = 0; k < 56u && i < n; i++) {
				unsigned char c = *(volatile unsigned char *)(a + i);
				if (c == '\n') { i++; break; }
				sl[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
			}
			sl[k] = 0;
			fastboot_info(sl);
		}
	} else if (!strncmp(arg, "smc", 3)) {
		/* a secure call by number, allowlisted to the CPU power-up SIP ids + their neighbours */
		unsigned fid, a1, a2, a3, r;
		arg += 3;
		fid = hexarg(&arg); a1 = hexarg(&arg); a2 = hexarg(&arg); a3 = hexarg(&arg);
		if (fid < 0x82000210u || fid > 0x82000216u) {
			fastboot_fail("smp smc: 0x82000210..0x82000216 only");
			return;
		}
		r = smc4(fid, a1, a2, a3);
		snprintf(sl, sizeof sl, "smc %08x(%x,%x,%x) = %08x %08x", fid, a1, a2, a3, r, s_smc_r[1]);
		fastboot_info(sl);
		snprintf(sl, sizeof sl, "pwrsta %08x cpc spmc %08x", rd32(SPM_BASE + 0x160u), rd32(MCUCFG_CPC_SPMC));
		fastboot_info(sl);
	} else if (arg[0] == 'w') {
		/* write one word, only to the CPC/MCUCFG or SPM blocks, read back */
		unsigned a, v, b;
		arg++;
		a = hexarg(&arg) & ~3u;
		v = hexarg(&arg);
		b = a & 0xFFFFF000u;
		if ((a < 0x0C530000u || a >= 0x0C540000u) && (a < 0x10006000u || a >= 0x10007000u)) {
			fastboot_fail("smp w: CPC/MCUCFG 0x0C53xxxx or SPM 0x10006xxx only");
			return;
		}
		(void)b;
		*(volatile unsigned *)a = v;
		__asm__ volatile("dsb sy" ::: "memory");
		snprintf(sl, sizeof sl, "w %08x <- %08x, read %08x", a, v, rd32(a));
		fastboot_info(sl);
	} else if (arg[0] == 'r') {
		unsigned a, n;
		arg++;
		a = hexarg(&arg) & ~3u;
		n = hexarg(&arg);
		if (n < 1u) n = 1u;
		if (n > 32u) n = 32u;
		for (i = 0; i < n; i += 4u) {
			unsigned k, len = (unsigned)snprintf(sl, sizeof sl, "%08x:", a + i * 4u);
			for (k = i; k < n && k < i + 4u; k++)
				len += (unsigned)snprintf(sl + len, sizeof sl - len, " %08x", rd32(a + k * 4u));
			fastboot_info(sl);
		}
	} else {
		fastboot_fail("smp: arm | on,N | st | ping,HEX | r,ADDR,N");
		return;
	}
	fastboot_okay("");
}

/* ---- opt-in public API (see pico_smp.h). Nothing here runs at boot. ---- */

#define CPC_CTRL_ENABLE  (1u << 16)   /* set in MCUCFG_CPC_FLOW when the firmware is patched */
#define SRAM_PARAM_BASE  SMP_SRAM  /* parameter page cpu0 writes before a core powers on */

int pico_smp_available(void)
{
	return (rd32(MCUCFG_CPC_FLOW) & CPC_CTRL_ENABLE) ? 1 : 0;
}

int pico_smp_core_up(unsigned n, unsigned entry_pa, unsigned shared_pa, unsigned pt_pa)
{
	if (!pico_smp_available())
		return -1;                 /* stock firmware: a power-on would hang - refuse */
	if (n < 1u || n > 7u)
		return -2;
	*(volatile unsigned *)(SRAM_PARAM_BASE + 32u) = shared_pa;   /* BC[8] shared-block base */
	*(volatile unsigned *)(SRAM_PARAM_BASE + 36u) = pt_pa;       /* BC[9] page-table base */
	arch_clean_cache_range(entry_pa & ~63u, 512);
	return (int)smc4(PSCI_CPU_ON, n << 8, entry_pa, 0);
}

/* Before a reset or power off: a warm reset does not power the other cores down, and a core
 * left on refuses the next CPU_ON. Does nothing when no core was started. */
void pico_smp_before_reset(void)
{
	if (s_started) pico_smp_stop_all();
	if (s_big_saved_mhz) {          /* the big cluster's clock and rail back as the boot left them */
		pico_big_set_mhz(s_big_saved_mhz);
		pico_big_set_code(s_big_saved_code);
		g_pico_big_rail_code = 0u;
		s_big_saved_mhz = 0u;
	}
}

void pico_smp_stop_all(void)
{
	volatile unsigned *c = g_pico_smp_comm;
	(void)0;
	c[28] = 1u;                        /* STOP: workers self power-off via PSCI CPU_OFF */
	arch_clean_cache_range((addr_t)c, 256);
	__asm__ volatile("dsb\n\tsev");    /* a guest asleep at its frame boundary */
	{ extern void mdelay(unsigned long msec); mdelay(300); }
	c[28] = 0u;
	arch_clean_cache_range((addr_t)c, 256);
}

/* A 32 bit guest on core n: its code runs unprivileged (AArch32 EL0) under a small AArch64 EL1
 * host (pico_smp_gba_land). The guest may read, write and execute all of RAM and reach the
 * devices, as the LK's own code does; only the host's two pages are read only. Opt in only. */
/* the guest's tables and its parameters (the mark page); the shared counters start from zero */
static void guest_prepare(unsigned entry, unsigned stack_top)
{
	unsigned i, land = (unsigned)pico_smp_gba_land, blk = land & ~0x1FFFFFu, k = (blk - 0x40000000u) >> 21;
	for (i = 0; i < 512u; i++) g_pico_smp_pt[i] = 0;
	g_pico_smp_pt[0] = 0x445ull | (3ull << 53);              /* devices: EL0 and EL1 read/write, never executed */
	g_pico_smp_pt[1] = (unsigned long long)(unsigned)s_l2 | 3ull;
	g_pico_smp_pt[2] = 0x80000000ull | 0x741ull;
	g_pico_smp_pt[3] = 0xC0000000ull | 0x741ull;
	for (i = 0; i < 512u; i++) s_l2[i] = (0x40000000ull + ((unsigned long long)i << 21)) | 0x741ull;
	s_l2[k] = (unsigned long long)(unsigned)s_l3 | 3ull;
	for (i = 0; i < 512u; i++) s_l3[i] = ((unsigned long long)blk + ((unsigned long long)i << 12)) | 0x743ull;
	for (i = (land - blk) >> 12; i <= (land + 0xC00u - blk) >> 12 && i < 512u; i++)
		s_l3[i] = ((unsigned long long)blk + ((unsigned long long)i << 12)) | 0x7C3ull;   /* the host: read only */
	arch_clean_cache_range((addr_t)g_pico_smp_pt, 4096);
	arch_clean_cache_range((addr_t)s_l2, 4096);
	arch_clean_cache_range((addr_t)s_l3, 4096);
	for (i = 0; i < 16u; i++) BC[i] = 0;
	for (i = 24; i < 40u; i++) g_pico_smp_comm[i] = 0;
	arch_clean_invalidate_cache_range((addr_t)g_pico_smp_comm, 256);
	arch_clean_cache_range(land, 0xC00);
	BC[8] = (unsigned)g_pico_smp_comm;
	BC[9] = (unsigned)g_pico_smp_pt;
	BC[10] = entry;
	BC[11] = stack_top;
}

int pico_smp_guest_start(unsigned n, unsigned entry, unsigned stack_top)
{
	unsigned land = (unsigned)pico_smp_gba_land;
	if (!pico_smp_available()) return -1;
	if (land < 0x40000000u || land >= 0x80000000u) return -3;
	if (g_pico_smp_gba_mhz && (n == 6u || n == 7u)) {
		/* set before the core is powered and then left alone: the guest stays up between games */
		if (!s_big_saved_mhz) {
			s_big_saved_mhz = pico_big_get_mhz(); s_big_saved_code = pico_big_get_code();
			pico_big_set_opp(g_pico_smp_gba_mhz);
		}
	}
	guest_prepare(entry, stack_top);
	return (int)smc4(PSCI_CPU_ON, n << 8, land + 0x800u, 0);
}

/* The big core's clock levels for the menus (the upper part of the stock table; none on a console
 * without a table), and the guest's clock: kept for the next time the core is set up, and applied
 * at once when it already is. Call it only while no frame runs on the guest (the frontend's own
 * thread between frames does). */
int pico_smp_big_levels(void)
{
	return (int)(sizeof k_big_opp / sizeof k_big_opp[0]) - BIG_MENU_FIRST;
}
unsigned pico_smp_big_level_mhz(int i)
{
	if (i < 0 || i >= pico_smp_big_levels()) return 0u;
	return k_big_opp[i + BIG_MENU_FIRST].mhz;
}
void pico_smp_big_clock(unsigned mhz)
{
	int i, n = pico_smp_big_levels();
	for (i = 0; i < n; i++) if (pico_smp_big_level_mhz(i) == mhz) break;
	if (i == n) return;
	g_pico_smp_gba_mhz = mhz;
	if (s_big_saved_mhz) pico_big_set_opp(mhz);
}

/* True when core n is powered (the power controller's status). */
int pico_smp_core_on(unsigned n)
{
	return n < 8u && ((rd32(MCUCFG_CPC_SPMC) >> n) & 1u);
}

#define GUEST_FAULT   37   /* shared block words, as the host uses them (pico_smp_gbahost64.S.src) */
#define GUEST_RESTART 38

/* A guest that stalls is cut off by its own core: the guest arms the core's timer for each frame
 * and the host holds it when the timer fires (pico_smp_gbahost64.S.src). The boot core cannot do
 * it from outside: it has no way to interrupt the other core's guest, and emptying the translation
 * caches from here does not reach that core (measured). True once the host reports the guest held. */
int pico_smp_guest_held(void)
{
	return g_pico_smp_comm[GUEST_FAULT] != 0u;
}

/* Start a held guest again from `entry` on `stack_top` (after a fault or a kill). Returns 0 once
 * the host took the request. The core stays powered throughout. */
int pico_smp_guest_restart(unsigned entry, unsigned stack_top)
{
	volatile unsigned *c = g_pico_smp_comm;
	unsigned i;
	if (!c[GUEST_FAULT]) return -1;
	guest_prepare(entry, stack_top);       /* clears the fault record too */
	c[GUEST_FAULT] = 1u;                   /* still held until the host takes the request */
	c[GUEST_RESTART] = 1u;
	__asm__ volatile("dsb\n\tsev");
	for (i = 0; i < 200u; i++) {
		if (!c[GUEST_RESTART]) return 0;
		{ extern void mdelay(unsigned long msec); mdelay(1); }
	}
	c[GUEST_RESTART] = 0u;
	return -2;
}

void pico_smp_register_cmds(void)
{
	fastboot_register("oem smp", cmd_smp, 1, 0);
}
