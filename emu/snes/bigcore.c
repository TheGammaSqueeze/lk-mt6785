/*
 * EXPERIMENTAL: bring up a second CPU core from LK to later split the SNES-menu
 * render across two cores. Debug-gated (AYANEO_DEBUG_LOGGING); the release build
 * compiles the whole experiment out so the menu always works single-core.
 *
 * Boot core MPIDR = 0x81000000 (MT bit set) -> cores are indexed by Aff1:
 * core N = 0x(N)00. Little A55 = cores 0..5, big A76 = cores 6,7.
 *
 * WHY NOT PSCI: PSCI CPU_ON (SMC 0x84000003) to any secondary BLOCKS forever in
 * ATF. Root cause (from disassembling the device ATF/BL31 = tee.img, and MTK's
 * open-source ATF siblings mt8186/mt8192 which share MT6785's MCUCFG_BASE
 * 0x0C530000 / SPM_BASE 0x10006000): the platform pwr_domain_on -> spm_poweron_cpu
 * ends in `while (!(SPM_CPU_PWR_STATUS & BIT(cpu)));` - it spins waiting for an
 * SPMC power-on ack that never arrives in our LK-stage environment.
 *
 * THE BYPASS (this file): the whole power-on is plain MMIO. Replicate it here -
 * set the per-CPU MCUCFG boot-address register to our ARM (AArch32) stub, select
 * AArch32 via MCUCFG_INITARCH, run the SPMC power-on, and poll the ack WITH A
 * TIMEOUT so LK can't hang. The core then resets straight into bc_entry_arm (no
 * ATF, no PSCI), which writes a magic + a bounded counter to DRAM so the little
 * core's OSD/UART telemetry proves it ran. Register offsets verified against the
 * MT6785 ATF disassembly (0xc900 bootaddr, 0xc8e4 initarch, 0xa814 cpc, 0x208
 * cpu_pwr_con, SPM base 0x1000_6000, MCUCFG base 0x0c53_0000).
 */

extern void arch_clean_invalidate_cache_range(void *addr, unsigned long len);
extern void arch_invalidate_cache_range(unsigned long addr, unsigned long len);
extern void arch_sync_cache_range(unsigned long addr, unsigned long len);
extern int mboot_common_load_part(char *part_name, char *img_name, unsigned long addr);
extern unsigned long mt_secure_call_all(unsigned long fn, unsigned long a0,
	unsigned long a1, unsigned long a2, unsigned long a3,
	unsigned long *r1, unsigned long *r2, unsigned long *r3);
extern int _dprintf(const char *fmt, ...);   /* UART, emits even in release */
extern void udelay(unsigned long usec);

/* ---- MMIO helpers ---- */
static inline unsigned rd32(unsigned a) { return *(volatile unsigned *)a; }
static inline void wr32(unsigned a, unsigned v) { *(volatile unsigned *)a = v; }
static inline void setb(unsigned a, unsigned m) { wr32(a, rd32(a) | m); }
static inline void clrb(unsigned a, unsigned m) { wr32(a, rd32(a) & ~m); }

/* ---- MT6785 CPU-power register map (all VERIFIED by decoding the device ATF's
 * mcucfg_set_bootaddr / mcucfg_init_archstate / spm_poweron_cpu / spm_get_cpu_
 * powerstate; NOT the mt8186 values which differ for the SPM CPU-power regs) ---- */
#define MCUCFG_BASE            0x0C530000u
#define SPM_BASE               0x10006000u
#define SPM_CPU_PWR_STATUS     (SPM_BASE + 0x160u)   /* ack (ATF reads SPM+0x160) */
#define SPM_CPU_PWR_CON(n)     (SPM_BASE + 0x768u + ((n) << 2))  /* per-CPU, base from ATF rodata table */
#define MCUCFG_BOOTADDR(n)     (MCUCFG_BASE + 0xc900u + ((n) << 3))  /* per-CPU reset vector (0x0C53C900) */
#define MCUCFG_INITARCH        (MCUCFG_BASE + 0xc8e4u)               /* bit(16+cpu): 1=AArch64 */
#define MCUCFG_CPC_FLOW_CTRL   (MCUCFG_BASE + 0xa814u)
#define SPM_POWERON_CONFIG_EN  (SPM_BASE + 0x000u)   /* write-unlock: str 0x0b160001 */
#define SPM_UNLOCK             0x0B160001u           /* PROJECT_CODE(0xb16<<16)|BCLK_CG_EN */
#define SPM_MCUSYS_RSTCON      (SPM_BASE + 0x200u)               /* reset bank (spmc_init) */
#define SPM_CPUTOP_RSTCON      (SPM_BASE + 0x204u)
#define SPM_CPU_RSTCON(n)      (SPM_BASE + 0x208u + ((n) << 2))  /* cpu0..7: 0x208..0x224 */
#define PWR_ON                 (1u << 2)             /* ATF writes 0x4 to CPU_PWR_CON (0x768 bank) */
#define PWR_RST_B              (1u << 0)             /* reset-released */
#define RESETPWRON_CONFIG      (1u << 5)
#define CPC_CTRL_ENABLE        (1u << 16)            /* 0x10000 in CPC_FLOW */
#define SSPM_ALL_PWR_CTRL_EN   (1u << 13)            /* 0x2000 in CPC_FLOW */
/* SPM_CPU_PWR_STATUS ack bit for a cpu (ATF: cpu<=3 -> cpu+9, cpu>3 -> cpu+11) */
#define ACK_BIT(cpu)           (1u << ((cpu) <= 3u ? (cpu) + 9u : (cpu) + 11u))

/* ---- SPM PCM firmware (optional prerequisite; announce returns ret=0) ---- */
#define SIP_SPM_ARGS  0x8200022Au
#define SPM_ARGS_INIT 1u
#define SPMFW_PA      0x55000000u   /* mapped + free, just above the SNES comms */
#define FW_SIZE       0x10000u

/* PSCI CPU_ON (SMC32): the SANCTIONED path. LK is NS BL33 and CANNOT write the
 * secure-only MCUCFG/SPM CPU-power regs (proven: MCUCFG_BOOTADDR write read back
 * 0, no abort -> bus firewall drops the NS write). PSCI traps to EL3 ATF, which
 * does init_archstate/set_bootaddr/spm_poweron_cpu at EL3 (where writes land) and
 * returns the core NON-SECURE at our AArch32 stub (so comms stay NS-visible). */
#define PSCI_CPU_ON_SMC32  0x84000003u
/* MTK_SIP_KERNEL_BOOT (0x82000115): LK's normal aarch64 kernel-handoff SiP. On this
 * device's BL31 its handler runs spmc_init (arms the SPMC/CPC CPU-hotplug sequencer)
 * as its FIRST action, BEFORE it validates the kernel entry address - and returns to
 * LK if the entry is out of [0x40000000,0xFFFFFFFF]. So calling it with a BOGUS entry
 * arms CPU power without booting anything. This is the precondition the kernel gets
 * for free at handoff and LK-at-menu-time lacks (why PSCI CPU_ON hung). */
#define MTK_SIP_KERNEL_BOOT  0x82000115u
#define KBOOT_BOGUS_ENTRY    0x0u          /* out of range -> handler arms then returns */
/* extra probe offsets (firewall read-after-write map) */
#define MCUCFG_RW_RSVD0    (MCUCFG_BASE + 0x006Cu)   /* low MCUCFG scratch */
#define MCUCFG_CPC_SPMC_ST (MCUCFG_BASE + 0xA840u)   /* CPC-side SPMC ack, bit=1<<cpu */

/* ---- big<->little comms + entry stubs (shared layout: bigcore_comms.h) ---- */
#include "bigcore_comms.h"
static struct bc_comms *const bc = (struct bc_comms *)BC_COMMS_PA;
extern void bc_entry_arm(void);    /* MMU-off proof-of-life stub */
extern void bc_entry_arm2(void);   /* cached bring-up worker stub (bigcore_entry.S) */

/* Worker stack: a dedicated buffer in LK's OWN BSS, so it is GUARANTEED mapped +
 * cacheable by the page tables the worker adopts (a hardcoded DRAM address like
 * 0x54100000 might sit in an unmapped gap -> the worker faults on its first call).
 * Grows down from the top; 16-byte aligned; only used once the worker MMU is on. */
static unsigned char bc_worker_stack[65536] __attribute__((aligned(16)));
#define BC_WORKER_STACK_TOP  ((unsigned)(unsigned long)(bc_worker_stack + sizeof(bc_worker_stack)))

/* telemetry read by the driver OSD */
int      g_bc_target   = -1;
int      g_bc_psci_ret = 0x7fffffff;   /* reused: SPMC ack result (0 ok, -1 timeout) */
unsigned g_bc_seen     = 0;
unsigned g_bc_mpidr    = 0;
unsigned g_bc_pwrstat  = 0;            /* SPM_CPU_PWR_STATUS after power-on */

static unsigned read_mpidr(void)
{
	unsigned m;
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));
	return m;
}

/* Snapshot cpu0's live LPAE MMU config into the comms block so the secondary can
 * adopt the identical mappings and turn its MMU + caches on (see bc_entry_arm2).
 * Read the 64-bit TTBR0 with MRRC; the rest are 32-bit. cpu0 cache-cleans the
 * block afterwards (in bigcore_start) so the MMU-off secondary reads real DRAM. */
extern void arch_clean_cache_range(unsigned long start, unsigned long len);

/* Remap a DRAM range [pa, pa+size) as Device (non-cacheable) in cpu0's LIVE LPAE
 * page tables, WITHOUT unmapping anything else. The MT6785 LK maps DRAM
 * 0x40000000.. as 1 GB L1 BLOCKS; a naive page/section remap would split a block
 * into a fresh (zeroed) L2 and unmap the rest of that GB (incl LK's own code).
 * So we split the containing 1 GB block into a full 512-entry L2 that REPRODUCES
 * the block's mapping for every 2 MB section (identity, same attrs), then flip
 * only the target sections to Device+XN. The worker adopts the same TTBR0 tables,
 * so this makes the shared region non-cacheable for BOTH cores: cpu0's writes go
 * straight to DRAM and the (non-coherent) worker reads them from DRAM. Framebuffer
 * and everything else stay Normal-WB cacheable. EXPT only. */
#define BC_L1_BLOCK   0x1u
#define BC_L1_TABLE   0x3u
#define BC_ATTR_MASK   (0x7ull << 2)
#define BC_SH_MASK     (0x3ull << 8)
/* Match the working MMU-off default EXACTLY: Device-nGnRnE (strongly-ordered,
 * AttrIndx=0 -> MAIR byte0=0x00) + OUTER-Shareable (SH=0b10). The on-HW finding is
 * that the worker reads cpu0's writes with the MMU OFF (endpoint-direct) but not with
 * the MMU on and Device-nGnRE Inner-Shareable (routed through the DSU snoop fabric,
 * which the un-admitted worker is not a read-consumer of). Reproducing the MMU-off
 * attribute may route the reads endpoint-direct and bypass the un-admitted snoop path. */
#define BC_ATTR_SO     (0x0ull << 2)
#define BC_SH_OUTER    (0x2ull << 8)
#define BC_XN          0x0040000000000000ull   /* L1/L2 block XN (bit 54) */
#define BC_MK_NC(d)    (((d) & ~BC_ATTR_MASK & ~BC_SH_MASK) | BC_ATTR_SO | BC_SH_OUTER | BC_XN)
static unsigned long long bc_l2_tbl[512] __attribute__((aligned(4096)));

static void bc_device_map(unsigned pa, unsigned size)
{
	unsigned ttbr0_lo, ttbr0_hi, i;
	unsigned long long *l1, *l2, ent;
	unsigned l1i = pa >> 30;
	unsigned s0 = (pa >> 21) & 0x1FFu;
	unsigned s1 = ((pa + size - 1u) >> 21) & 0x1FFu;

	__asm__ volatile("mrrc p15, 0, %0, %1, c2" : "=r"(ttbr0_lo), "=r"(ttbr0_hi));
	l1 = (unsigned long long *)(unsigned long)(ttbr0_lo & ~0x1Fu);
	ent = l1[l1i];

	if ((ent & 0x3u) == BC_L1_BLOCK) {
		unsigned long long attrs = ent & ~0x0000FFFFFFFFF000ull;   /* keep upper+lower attrs+type, drop PA */
		unsigned long long gb = (unsigned long long)l1i << 30;
		for (i = 0; i < 512; i++)
			bc_l2_tbl[i] = ((gb + (unsigned long long)i * 0x200000ull) & 0x0000FFFFFFE00000ull) | attrs;
		l2 = bc_l2_tbl;
	} else if ((ent & 0x3u) == BC_L1_TABLE) {
		l2 = (unsigned long long *)(unsigned long)(unsigned)(ent & 0xFFFFF000u);
	} else {
		_dprintf("BC: device_map: L1[%u]=0x%x%x not block/table, abort\n",
			 l1i, (unsigned)(ent >> 32), (unsigned)ent);
		return;
	}

	for (i = s0; i <= s1; i++) {
		unsigned long long d = l2[i];
		if ((d & 0x3u) == BC_L1_BLOCK) {           /* 2 MB section: type is here */
			l2[i] = BC_MK_NC(d);
		} else if ((d & 0x3u) == BC_L1_TABLE) {    /* -> L3: type is in the 4KB pages */
			unsigned long long *l3 = (unsigned long long *)(unsigned long)(unsigned)(d & 0xFFFFF000u);
			unsigned p;
			for (p = 0; p < 512u; p++)
				if ((l3[p] & 0x3u))                /* valid page */
					l3[p] = BC_MK_NC(l3[p]);
			arch_clean_cache_range((unsigned long)l3, 512u * 8u);
		}
		_dprintf("BC: device_map L2[%u]=0x%x%x (type %u)\n",
			 i, (unsigned)(l2[i] >> 32), (unsigned)l2[i], (unsigned)(l2[i] & 0x3u));
	}
	arch_clean_cache_range((unsigned long)l2, 512u * 8u);

	if ((ent & 0x3u) == BC_L1_BLOCK) {
		l1[l1i] = ((unsigned long long)(unsigned)(unsigned long)bc_l2_tbl) | BC_L1_TABLE;
		arch_clean_cache_range((unsigned long)&l1[l1i], 8u);
	}
	__asm__ volatile("dsb sy");
	__asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));   /* TLBIALL */
	__asm__ volatile("dsb sy");
	__asm__ volatile("isb");
	/* flush any stale WB-cached lines for the now-Device region so a later eviction
	 * cannot clobber the Device writes (mixed-attribute aliasing) */
	arch_clean_invalidate_cache_range((void *)(unsigned long)pa, size);
	_dprintf("BC: device_map pa=0x%x size=0x%x sections %u..%u of GB%u (l1 was %s)\n",
		 pa, size, s0, s1, l1i, ((ent & 0x3u) == BC_L1_BLOCK) ? "block->split" : "table");
}

static void bc_snapshot_mmu(void)
{
	unsigned ttbr0_lo, ttbr0_hi, ttbcr, mair0, mair1, dacr, sctlr;
	__asm__ volatile("mrrc p15, 0, %0, %1, c2" : "=r"(ttbr0_lo), "=r"(ttbr0_hi));
	__asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
	__asm__ volatile("mrc p15, 0, %0, c10, c2, 0" : "=r"(mair0));
	__asm__ volatile("mrc p15, 0, %0, c10, c2, 1" : "=r"(mair1));
	__asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	bc->ttbr0_lo = ttbr0_lo; bc->ttbr0_hi = ttbr0_hi;
	bc->ttbcr = ttbcr; bc->mair0 = mair0; bc->mair1 = mair1;
	bc->dacr = dacr; bc->sctlr = sctlr;
	bc->stack_top = BC_WORKER_STACK_TOP;
	_dprintf("BC: mmu snapshot ttbr0=%x:%x ttbcr=%x mair0=%x mair1=%x dacr=%x sctlr=%x\n",
		 ttbr0_hi, ttbr0_lo, ttbcr, mair0, mair1, dacr, sctlr);
}

/* Load the SPM PCM firmware and announce it to ATF. The SPMC hardware may need
 * SPM running to service the power-on ack; harmless if not (returns ret=0). */
static void bc_load_spmfw(void)
{
	int sz = mboot_common_load_part((char *)"spmfw", (char *)"spmfw", SPMFW_PA);
	unsigned long r;
	if (sz < 0) { _dprintf("BC: spmfw load FAILED %d\n", sz); return; }
	arch_sync_cache_range((unsigned long)SPMFW_PA, FW_SIZE);
	r = mt_secure_call_all(SIP_SPM_ARGS, SPM_ARGS_INIT, SPMFW_PA, FW_SIZE, 0, 0, 0, 0);
	_dprintf("BC: spmfw loaded sz=%d, SIP ret=%ld\n", sz, (long)r);
}

/* ATF spmc_init (@BL31 0x125f4), replayed so our bypass is self-contained and
 * does not depend on ATF's boot-time state: unlock SPM, release reset on the
 * off cores, clear reset-power-on config, enable the CPC. All idempotent. */
static void bc_spmc_init(void)
{
	unsigned n;
	wr32(SPM_POWERON_CONFIG_EN, SPM_UNLOCK);
	for (n = 1; n <= 7; n++) setb(SPM_CPU_RSTCON(n), PWR_RST_B);  /* 0x1000620c..0x10006224 */
	clrb(SPM_MCUSYS_RSTCON, RESETPWRON_CONFIG);                   /* 0x10006200 */
	clrb(SPM_CPUTOP_RSTCON, RESETPWRON_CONFIG);                   /* 0x10006204 */
	clrb(SPM_CPU_RSTCON(0), RESETPWRON_CONFIG);                   /* cpu0 0x10006208 */
	setb(MCUCFG_CPC_FLOW_CTRL, CPC_CTRL_ENABLE);
}

/* Direct SPMC power-on of core `cpu` (1..7), booting it AArch32 into `entry`. */
static int bc_manual_poweron(unsigned cpu, unsigned entry)
{
	int i, ack = 0;

	_dprintf("BC: manual power-on cpu%u, boot=0x%x\n", cpu, entry);
	bc_spmc_init();   /* ensure the SPMC/CPC is armed (idempotent over ATF's boot init) */
	/* ATF's plat_power_domain_on: init_archstate + set_bootaddr, then spm_poweron_cpu. */

	/* boot the core in AArch32 at our ARM stub (clear the AArch64 bit) */
	clrb(MCUCFG_INITARCH, 1u << (16 + cpu));        /* 0 = AArch32 */
	wr32(MCUCFG_BOOTADDR(cpu), entry);              /* per-CPU reset vector */
	wr32(MCUCFG_BOOTADDR(cpu) + 4, 0);              /* high word */

	_dprintf("BC: pre pwrstat=0x%x initarch=0x%x boot=0x%x ackbit=0x%x\n",
		 rd32(SPM_CPU_PWR_STATUS), rd32(MCUCFG_INITARCH),
		 rd32(MCUCFG_BOOTADDR(cpu)), ACK_BIT(cpu));

	/* spm_poweron_cpu: tell CPC (SSPM_ALL_PWR_CTRL_EN), assert PWR_ON, poll the
	 * ack - WITH A TIMEOUT (ATF has none, which is why its CPU_ON hangs). */
	setb(MCUCFG_CPC_FLOW_CTRL, SSPM_ALL_PWR_CTRL_EN);
	setb(SPM_CPU_PWR_CON(cpu), PWR_ON);
	for (i = 0; i < 100000; i++) {
		if (rd32(SPM_CPU_PWR_STATUS) & ACK_BIT(cpu)) { ack = 1; break; }
		udelay(1);
	}
	clrb(MCUCFG_CPC_FLOW_CTRL, SSPM_ALL_PWR_CTRL_EN);

	g_bc_pwrstat = rd32(SPM_CPU_PWR_STATUS);
	_dprintf("BC: pwrstat post=0x%x ack=%d (waited %d us)\n", g_bc_pwrstat, ack, i);
	return ack ? 0 : -1;
}

/* Firewall read-after-write map: probe which register windows accept writes from
 * NS LK. Every write is RESTORED; this does no power-on and CANNOT hang. Run it
 * BEFORE the PSCI attempt so a PSCI hang still leaves this data in the UART. */
static void bc_probe_one(const char *name, unsigned a, unsigned pat)
{
	unsigned s = rd32(a), rb;
	wr32(a, pat);
	rb = rd32(a);
	_dprintf("BC probe %s @0x%x rd=0x%x wr=0x%x rb=0x%x %s\n",
		 name, a, s, pat, rb, (rb == pat) ? "STICK" : "DROP");
	wr32(a, s);   /* restore */
}

static void bc_probe_mmio(void)
{
	/* P1 readability (reads only) */
	_dprintf("BC probe reads: RSVD0=0x%x CPCFLOW=0x%x INITARCH=0x%x BOOT1=0x%x SPMCST=0x%x\n",
		 rd32(MCUCFG_RW_RSVD0), rd32(MCUCFG_CPC_FLOW_CTRL),
		 rd32(MCUCFG_INITARCH), rd32(MCUCFG_BOOTADDR(1)),
		 rd32(MCUCFG_CPC_SPMC_ST));
	/* P2 low-MCUCFG write, P3 high-MCUCFG (the known-failing bootaddr) */
	bc_probe_one("MCUCFG_lo", MCUCFG_RW_RSVD0, 0x1234ABCDu);
	bc_probe_one("BOOTADDR1", MCUCFG_BOOTADDR(1), 0xCAFEF00Du);
	/* P4 control writes: mid-range CPC + high INITARCH */
	bc_probe_one("CPC_FLOW", MCUCFG_CPC_FLOW_CTRL, rd32(MCUCFG_CPC_FLOW_CTRL) | CPC_CTRL_ENABLE);
	bc_probe_one("INITARCH", MCUCFG_INITARCH, rd32(MCUCFG_INITARCH) ^ (1u << 17));
	/* P5 SPM side: does the SPM (0x10006xxx) firewall differently from MCUCFG? */
	bc_probe_one("SPM_UNLOCK", SPM_POWERON_CONFIG_EN, SPM_UNLOCK);
	bc_probe_one("SPM_PWRON1", SPM_CPU_PWR_CON(1), rd32(SPM_CPU_PWR_CON(1)) | PWR_ON);
	bc_probe_one("SPM_RST1", SPM_CPU_RSTCON(1), rd32(SPM_CPU_RSTCON(1)) | PWR_RST_B);
}

void bigcore_start(void)
{
/* CONCLUSION (run-2, 2026-08-26): the 2nd-core-during-LK experiment is a DEAD END
 * and is now OFF by default (opt in with -DAYANEO_BIGCORE_EXPT). Proven on HW:
 *  (1) ALL of MCUCFG (0x0C53xxxx) is NS write-locked - boot-addr/initarch/CPC
 *      writes silently DROP (reads work), so LK can't set where a core boots.
 *  (2) PSCI CPU_ON (SMC to EL3) HANGS in ATF's un-timed SPMC ack spin at this
 *      pre-kernel stage - the power-on ack never fires before the kernel's SPM/
 *      SSPM stack is up. So even the sanctioned path can't bring a core up here.
 * Kept compiled (behind the opt-in) for a future ATF-patch retest; default builds
 * (release AND debug) no-op so the menu always boots. */
#ifndef AYANEO_BIGCORE_EXPT
	(void)bc; (void)bc_entry_arm; (void)bc_manual_poweron; (void)bc_load_spmfw;
	(void)read_mpidr; (void)bc_probe_mmio;
	return;
#else
	/* EXPERIMENT 1b (paired with the patched+re-signed tee.img that arms the CPC at
	 * COLD BOOT): read MCUCFG_CPC_FLOW_CTRL - the patched ATF should have set bit16
	 * (CPC_CTRL_ENABLE) before LK. GATE the PSCI CPU_ON on that bit: if armed, issue
	 * PSCI CPU_ON(0x100=cpu1) and check comms; if NOT armed (stock/control tee), SKIP
	 * PSCI so this same LK is SAFE to boot on an un-patched TEE (no freeze). */
	unsigned long r1 = 0, r2 = 0, r3 = 0, ret;
	unsigned entry = ((unsigned long)&bc_entry_arm2) & ~1u;  /* cached worker, ARM */
	unsigned cpc;

	g_bc_mpidr = read_mpidr();
	(void)&bc_manual_poweron; (void)&bc_load_spmfw; (void)&bc_probe_mmio;
	(void)&bc_entry_arm; (void)MTK_SIP_KERNEL_BOOT; (void)KBOOT_BOGUS_ENTRY;

	cpc = rd32(MCUCFG_CPC_FLOW_CTRL);
	_dprintf("BC: boot MPIDR=0x%x CPC_FLOW=0x%x (CPC_CTRL_ENABLE bit16 %s)\n",
		 g_bc_mpidr, cpc, (cpc & CPC_CTRL_ENABLE) ? "SET=armed by patched ATF" : "clear=NOT armed");
	g_bc_target = (int)cpc;   /* OSD 't' shows CPC_FLOW so we see armed-at-boot state */

	if (!(cpc & CPC_CTRL_ENABLE)) {
		_dprintf("BC: CPC not armed -> SKIP PSCI (safe on stock/control tee). Flash the patched tee.img.\n");
		g_bc_psci_ret = 0x7fffffff;   /* sentinel: skipped */
	} else {
		bc->magic = 0; bc->cached_ok = 0; bc->counter = 0;
		/* PATH B step 1 (de-risk): make the canary's 2 MB section (0x51000000, comp
		 * staging, free during render) Device/non-cacheable via a sibling-preserving
		 * L2 split. If this boots AND the canary flips cpu0->worker to 0xCA5Axxxx, the
		 * split is safe and non-cacheable is the fix; then extend to comms+menu+scene. */
		bc_device_map(0x51000000u, 0x200000u);
		bc_snapshot_mmu();   /* publish cpu0's LPAE MMU config for the worker to adopt */
		/* clean the WHOLE comms block to DRAM so the MMU-off secondary reads the real
		 * snapshot (its early reads bypass caches). */
		arch_clean_invalidate_cache_range((void *)bc, sizeof(*bc));
		/* CPC armed -> the ack should now fire. (If arming is somehow insufficient the
		 * un-timed ATF ack poll can still freeze -> power-cycle; that is the datapoint.) */
		_dprintf("BC: CPC armed -> PSCI CPU_ON mpidr=0x100 entry=0x%x (cached worker) ...\n", entry);
		ret = mt_secure_call_all(PSCI_CPU_ON_SMC32, 0x100u, entry, 0, 0, &r1, &r2, &r3);
		_dprintf("BC: PSCI CPU_ON ret=0x%lx\n", (unsigned long)ret);
		g_bc_psci_ret = (int)(unsigned)ret;

		udelay(3000);
		arch_clean_invalidate_cache_range((void *)bc, sizeof(*bc));
		g_bc_seen = (bc->magic == BC_MAGIC) ? bc->counter : 0;
		_dprintf("BC: after PSCI magic=0x%x cached_ok=0x%x counter=%u\n",
			 bc->magic, bc->cached_ok, bc->counter);
	}
	g_bc_pwrstat = rd32(SPM_CPU_PWR_STATUS);
#endif
}

unsigned bigcore_counter(void)
{
	arch_clean_invalidate_cache_range((void *)bc, sizeof(*bc));
	if (bc->magic != BC_MAGIC) return 0;
	return bc->counter;
}

/* Ungated diagnostics: read whatever is in the comms words RIGHT NOW, without
 * the magic handshake gate. Lets a single flash fully classify the outcome:
 *   raw_magic==BC_MAGIC + raw_counter climbing -> full win
 *   raw_magic==BC_MAGIC + raw_counter static    -> reached stub, loop stalled
 *   raw_magic==0        + raw_counter climbing   -> counter visible, magic write not (odd; still alive)
 *   raw_magic==0        + raw_counter==0         -> nothing visible (core didn't run, OR comms not
 *                                                   visible to the NS little core -> check pwrstat bit)
 */
/* These are polled every frame by the OSD while the worker is running the render
 * fork/join. They must NOT clean-invalidate the whole comms block: the clean half
 * would write cpu0's stale copies of the worker-owned lines (done/counter/cached_ok)
 * back over the worker's fresh DRAM values (a cross-core clobber). Invalidate ONLY
 * the specific worker-owned line, then read it fresh from DRAM. magic/stage live on
 * line0 (offset 0); counter/cached_ok on line4 (offset 256). */
unsigned bigcore_raw_magic(void)
{
	arch_invalidate_cache_range((unsigned long)bc + 0u, 64u);
	return bc->magic;
}

unsigned bigcore_raw_counter(void)
{
	arch_invalidate_cache_range((unsigned long)bc + 256u, 64u);
	return bc->counter;
}

/* nonzero once the worker (bc_entry_arm2) has enabled its MMU + I/D caches and is
 * running cached. Distinguishes "reached the stub" (raw_magic) from "came up
 * cached" (this) - if magic is set but this stays 0, the MMU/cache enable wedged. */
unsigned bigcore_cached_ok(void)
{
	arch_invalidate_cache_range((unsigned long)bc + 256u, 64u);
	return bc->cached_ok;
}
