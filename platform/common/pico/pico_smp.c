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
extern void mtk_wdt_restart(void);
unsigned long long g_pico_smp_pt[512] __attribute__((aligned(4096)));

unsigned g_pico_smp_save[64] __attribute__((aligned(4096)));
static unsigned s_diag[64] __attribute__((aligned(4096)));
unsigned g_pico_smp_comm[1024] __attribute__((aligned(4096)));

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
static unsigned smc4(unsigned fid, unsigned a1, unsigned a2, unsigned a3)
{
	register unsigned r0 __asm__("r0") = fid;
	register unsigned r1 __asm__("r1") = a1;
	register unsigned r2 __asm__("r2") = a2;
	register unsigned r3 __asm__("r3") = a3;
	__asm__ volatile(".arch_extension sec\n smc #0"
			 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r4", "r5", "r6", "r7", "r12", "memory");
	s_smc_r[0] = r0; s_smc_r[1] = r1; s_smc_r[2] = r2; s_smc_r[3] = r3;
	return r0;
}

/* Marks that outlive a restart: the tail of the SRAM the RAM console sits in (device memory in
 * the LK's map, so nothing is cached). */
#define BC ((volatile unsigned *)0x0011D7C0u)
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
		for (to = 0; to < 4000000u && (rd32(SPM_BASE + 0x160u) & 0x0001FC00u); to++) { if ((to & 0xFFFFFu) == 0u) mtk_wdt_restart(); }
		c[28] = 0u; arch_clean_cache_range((addr_t)c, 256);
		snprintf(sl, sizeof sl, "stop: pwrsta %08x (siblings bits 10..16 clear = all off)", rd32(SPM_BASE + 0x160u));
		fastboot_info(sl);
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "coh", 3)) {
		unsigned cohn;
		/* true-coherency test: worker runs with MMU + caches on (Normal WB Inner-Shareable);
		 * cpu0 and worker exchange via barriers ONLY, no clean/invalidate. If the worker sees
		 * cpu0's cached canary, the DSU keeps them coherent and offload needs no per-use flush. */
		volatile unsigned *c = g_pico_smp_comm;
		volatile unsigned *mk = (volatile unsigned *)0x0011D7C8u;
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
		*(volatile unsigned *)0x0011D7E0u = (unsigned)g_pico_smp_comm; /* BC[8] comm base */
		*(volatile unsigned *)0x0011D7E4u = (unsigned)g_pico_smp_pt;   /* BC[9] page table */
		arg += 3; cohn = hexarg(&arg); if (cohn == 0u) cohn = 1u;
		/* stop any running worker: set STOP, wait for the power-status siblings to clear */
		c[28] = 1u; arch_clean_cache_range((addr_t)c, 256);
		for (to = 0; to < 4000000u && (rd32(SPM_BASE + 0x160u) & 0x0001FC00u); to++) { if ((to & 0xFFFFFu) == 0u) mtk_wdt_restart(); }
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
		*(volatile unsigned *)0x0011D7E0u = (unsigned)g_pico_smp_comm;  /* BC[8]: hand the worker its base */
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
#define SRAM_PARAM_BASE  0x0011D7C0u  /* parameter page cpu0 writes before a core powers on */

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

void pico_smp_stop_all(void)
{
	volatile unsigned *c = g_pico_smp_comm;
	unsigned to;
	c[28] = 1u;                        /* STOP: workers self power-off via PSCI CPU_OFF */
	arch_clean_cache_range((addr_t)c, 256);
	for (to = 0; to < 4000000u && (rd32(SPM_BASE + 0x160u) & 0x0001FC00u); to++)
		if ((to & 0xFFFFFu) == 0u) mtk_wdt_restart();
	c[28] = 0u;
	arch_clean_cache_range((addr_t)c, 256);
}

void pico_smp_register_cmds(void)
{
	fastboot_register("oem smp", cmd_smp, 1, 0);
}
