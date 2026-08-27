# MT6785 multi-core-at-LK revival research

Live research log. Goal: get a 2nd (or more) CPU core doing useful work during the LK-stage
SNES menu (2 cores at a lower clock -> 60fps at lower power, or just more compute). This
revisits a path previously concluded a "dead end" with fresh eyes and a specific new lever.

Repo: /work/svoboda_lk (branch lk-snes-menu). Kernel src: /work/mt6785_kernel_source.
ATF disasm: /tmp/atf.dis, /tmp/bl31_dis.txt (tee.img BL31, base 0x4ce00000, dis = file-0x400).
MTK ATF source (same-family mt8186/mt8192): /tmp/mtk-atf. SoC = MT6785 / Helio G95:
cluster0 = 6x Cortex-A55 (MPIDR 0x000..0x500), cluster1 = 2x Cortex-A75 (0x600/0x700),
single DSU (DynamIQ). Boot core = cpu0 (0x000, A55). LK = NS BL33 under resident EL3 ATF.

## THE WALL (exhaustively proven on HW in the prior effort)
- 2nd core (cpu1, A55, reg 0x100, same cluster0/DSU as cpu0) CAN be powered on + brought up
  cached (adopts cpu0's LPAE MMU snapshot, D/I-cache + VFP on) via: ATF cold-boot patch that
  arms the CPC power controller (tee.img 4-byte @0x9688: bl NOTICE -> bl 0x94d0 spmc/CPC arm)
  + re-sign cert2, then plain PSCI CPU_ON(0x100) from LK. Confirmed alive + cached on HW.
- worker -> cpu0 WRITES work (worker cleans to DRAM/PoC, cpu0 invalidates + reads: correct).
- cpu0 -> worker READS FAIL the instant the worker's MMU is ON. Proven for EVERY memory
  attribute: WB Inner-Shareable, Device-nGnRE, strongly-ordered+Outer-Shareable, D-cache-off.
  Worker reads a FIXED stale/uninit value; faults on garbage menu_ptr. At MMU-OFF the worker
  reads cpu0's (DRAM-resident, cleaned) data fine (that's how it adopts the MMU snapshot).
- Root cause (prior conclusion): the CPC-hardware power-on bypass powers the core + wires its
  WRITE channel but never admits the core as a DSU snoop-READ participant. That admission is
  done by the SSPM-driven SPMC power sequence, which is DORMANT at LK stage (it's why a plain
  full PSCI CPU_ON HANGS at LK without the CPC-arm bypass - SSPM never acks the power-on).
- Tried + failed to fix at LK: ATF w21-ungate patch to force the full per-core
  pwr_domain_on_finish body (no change - the CCI/DSU admit routines it gates are CLUSTER-level
  fixed addrs, already run for cluster0 at cpu0 boot; there is NO per-core snoop-enable reg
  that cpu0 programs for cpu1). fake-kernel-jump (spmc_init is one-shot, already ran). Every
  memory attribute. So: "the worker never becomes a DSU snoop-read participant; that requires
  the SSPM-driven power sequence that only runs at kernel stage."

## THE NEW LEVER (why this might not actually be dead)
The kernel gets coherent secondaries "for free" because by kernel time the SSPM coprocessor is
LIVE and actively SERVICING CPU-power (the full SPMC sequence that admits each core to DSU
snoop). The unexplored question: **what exactly makes SSPM service coherent CPU-power, and can
LK trigger/replicate it?** Prior effort deemed this "mechanism unclear, likely not NS-reachable"
but did NOT deeply RE the kernel's SSPM/MCDI/SPM init to find the actual trigger. That is the
focus now. Sub-questions:
1. Is SSPM already RUNNING at LK (preloader-loaded) but not servicing CPU-power until commanded,
   or does the kernel boot/enable it? What is the exact enable trigger (mailbox IPC? register)?
2. What does the kernel do on cpu_up beyond `psci_cpu_on -> smc`? Any SSPM/SPM command that
   arms per-core snoop admission?
3. Can that trigger be issued from LK (NS PL1) directly, via an existing ATF SiP, or by porting
   the SSPM driver / setting MTK_TINYSYS_SSPM_SUPPORT=yes in LK?
4. Reconsider the "MMU-on read fails even for Device" conclusion - is there a hole? (The worker
   reading back its OWN MMU-on write to shared DRAM was never definitively done.)
5. Feasibility of the A75 big cores (0x600/0x700, separate powered-off cluster1) vs the A55.

## STATUS
- 2026-08-28: research restarted (user request). Prior GPU/OVL SNES work paused + reverted to a
  correct state (staged). Launching a deep parallel RE (kernel SSPM/MCDI/SPM + ATF + online +
  frankenstein brainstorm). Findings appended below.
- 2026-08-28 (cycle 1 done): wall REFRAMED (SPM not SSPM; ack already fires at LK; kernel does
  nothing extra; no per-core snoop reg; ATF on-finish already runs CLUSTERPWRDN). Implemented +
  built + signed the DECISIVE Lever-1 probe. See OPERATOR NOTES below.

## OPERATOR NOTES (what to flash when you wake, and how to read it)
The decisive experiment is BUILT, SIGNED and STAGED. It is one flash and it tells us whether the
2nd core is blocked by a real hardware snoop wall or by a fixable LK bug.

Image: /mnt/c/pairmini/lk_a_snes_bigcore_probe.img  (built with AYANEO_BIGCORE_EXPT=yes)
Flash: to the lk_a partition (SP Flash Tool / fastboot flash lk_a <img>), boot into the SNES menu,
       capture UART.
PREREQUISITE: the multicore worker only powers on if the CPC was armed at cold boot, which needs
the PATCHED + re-signed tee.img already flashed. If CPC is NOT armed the log prints
"BC: CPC not armed -> SKIP PSCI" and the probe fields stay zero (safe, but no data). So flash the
patched tee.img first if it is not currently on the device. The RECON line still prints either way.

Read these UART lines:
  "BC RECON: CPC_SPMC_ST(0x0C53A840)=0x..  SPM_CPU_PWR_STATUS(0x10006160)=0x.."
     -> if CPC_SPMC_ST has bits 6 and/or 7 set, the A75 cluster1 cores are reset-released at cold
        boot => Lever 3 (power cpu 0x600) is worth trying. If clear, cluster1 needs a bigger patch.
  "BC LEVER1 PROBE: w_self_wb=0x.. w_self_dev=0x.. canpar_lo=0x.. selfpar_lo=0x.."
     Decision tree (also in FINDINGS Lever 1):
       - w_self_wb == 0x5E1Fxxxx AND w_self_dev == 0x0DE0xxxx AND canpar_lo bit0==0 with PA==0x51000
         => the worker's OWN MMU + cache + Device paths are all SANE. The failure is purely
            cpu0->worker cross-core visibility => intra-cluster snoop wall CONFIRMED. Next: Lever 4
            (producer offload, guaranteed win) and/or Lever 3 (A75 fresh cluster).
       - w_self_wb STALE (not 0x5E1Fxxxx) => the worker's cacheable load path is broken in isolation
         => LK-LOCAL BUG (fixable), NOT a coherency wall. This REOPENS a full coherent split. Root
            cause the TTBR0/TLB adoption in bigcore_entry.S.
       - w_self_dev STALE (not 0x0DE0xxxx) => the worker's Device access is not reaching DRAM =>
         translation/routing bug for that region; fix the device_map/snapshot.
       - canpar_lo bit0==1 (fault) or PA != 0x51000xxx => the worker mistranslates the canary VA =>
         TLB/snapshot bug; fix and retest.
  Also still logged: "BC CANARY (non-comms 0x51000000): cpu0->worker worker-read=0x.. ;
       worker->cpu0 cpu0-read=0x.." (the actual cross-core result both directions), and
       "BC CANARY PROBE: cpu0 self-readback / cpu0 PAR-hi / worker PAR-hi".
Report those three lines back and the next lever is chosen deterministically from the tree above.

## FINDINGS

### Cycle 1 (2026-08-28): six-angle deep RE + solo ATF re-read. THE WALL IS REFRAMED.

Ran a 6-agent parallel RE (kernel SSPM, kernel MCDI/PSCI, kernel SPM, ATF SiP re-RE,
public DSU knowledge, "poke holes in the wall") + verified every load-bearing claim
myself against the device BL31 (/tmp/bl31_dis.txt). Net result: the old "SSPM is dormant
so the core is never snoop-admitted" framing is WRONG. Corrected ground truth (PROVEN =
read from the device binary/source):

- **SPM, not SSPM, produces the CPU power-on ack, and it ALREADY FIRES at LK.** PROVEN.
  spm_poweron_cpu (bl31 0x126dc): set MCUCFG_CPC_FLOW_CTRL(0x0C53A814)|=0x2000
  (SSPM_ALL_PWR_CTRL_EN, a hardware flow-ownership bit, NOT a firmware handshake), write
  PWR_ON to per-CPU PWR_CON (0x10006768 bank, indexed cpu<<2), then poll the ack at
  SPM_CPU_PWR_STATUS = 0x10006160, bit (cpu<=3 ? cpu+9 : cpu+11). Prior HW runs already
  saw this ack (pwrstat p=0x474, cpu1 bit10 set). So "SSPM never acks" was false; the
  core powers on through the sanctioned SPM sequence, at LK, today.
- **The current bigcore.c register map was already correct.** The PWR_CON bank is 0x768
  (from the BL31 rodata pointer table at 0x20c70 = 0x10006768), exactly bigcore.c:48. The
  0x208 bank is SPM_CPU_RSTCON (used only by spmc_init), a different register. The mt8186/
  mt8192 sibling offsets do NOT transfer 1:1 for this bank; trust the device BL31 + LK src.
- **The kernel does NOTHING for coherency beyond one PSCI SMC.** PROVEN. arm64
  cpu_psci_cpu_boot -> psci_ops.cpu_on() is the whole secondary bring-up (kernel
  arch/arm64/kernel/psci.c:47-55); no CPUECTLR/SMPEN/CCI/DSU/mailbox poke on the cpu_up
  path anywhere in arch/arm64. MCDI is idle/suspend-only and is PAUSED around hotplug.
  MTK_SIP_POWER_UP_CORE/CLUSTER (0x82000211/213) are defined-but-DEAD in this kernel. So
  there is no "kernel step LK omits" and no NS shortcut to copy.
- **No per-core software snoop-admit register exists.** PROVEN. The only admit code is CCI
  admit 0x971c (clears bit0 of 0x0C533308 / 0x0C533B08) + DSU admit 0x99d4 (RMW 0x0082C820
  into 0x0C533240 / 0x0C533A40). Both are fixed CLUSTER addresses (cluster0 = ..3308/..3240,
  cluster1 = ..3B08/..3A40), already run for cluster0 at cpu0 boot. There is no per-core
  snoop-enable register cpu0 could program for cpu1.
- **ATF's pwr_domain_on_finish ALREADY runs CLUSTERPWRDN on the woken core, and the PSCI
  path already reaches it.** Verified solo: 0x1a7f4 = write_clusterpwrdn_el1
  (`and x0,x0,#3; msr s3_0_c15_c3_6,x0`); pwr_domain_on_finish (0xbe88) calls it at 0xbf40.
  Our working bring-up is PSCI CPU_ON (the MCUCFG-bootaddr bypass is NS-firewalled and
  DROPs, so it was never usable), which makes the core reset into ATF's secure_entrypoint
  and traverse on-finish before EReting NS. The prior effort also FORCE-ran the full
  on-finish body (w21-ungate patch) with no change. So CLUSTERPWRDN is already handled and
  is NOT the missing step. This DOWNGRADES the "worker self-exec CLUSTERPWRDN" lever.
- ATF grants EL2/EL1 access to the IMP-DEF cluster registers (cold boot sets ACTLR_EL3 +
  ACTLR_EL2 bits 10/11/12 at 0x1a4a8-0x1a4ec), so a NS-EL1 AArch64 core COULD legally
  execute msr CLUSTERPWRDN_EL1. But per above, ATF already does it, so this only matters if
  the worker's execution state itself somehow changes coherency (it should not: DSU
  membership is physical, independent of AArch32 vs AArch64 at NS-EL1).

**Net reframing of THE WALL:** power-on + SPM ack + ATF on-finish (CLUSTERPWRDN, GIC, arch
state) all already happen at LK. The residual gap between our bring-up and the kernel's is
NOT an SSPM service and NOT a per-core snoop register. It is exactly one of two things,
and they are separable by a single never-run experiment:
  (i) a genuine intra-cluster snoop-admission wall (the manually-woken 7th core into the
      already-live cluster0 is powered + executing but not admitted as a snoop READ
      consumer), OR
  (ii) a mundane, fixable LK-LOCAL bug in the worker's MMU-on load path (wrong TTBR0
      adoption, stale TLB alias, load/store reordering) that was assumed to be a coherency
      wall but was never actually measured in isolation.

### Ranked levers (act in this order)

1. **DECISIVE PROBE (run first, one flash, no ATF change): worker self-readback + full
   PAR-LO.** The worker writes a word only it touches, cleans+invalidates it, reads it back
   with its MMU on, and captures the FULL PAR (PA + F bit) of the failing canary VA. This is
   the only test that separates (i) from (ii). It was designed twice in the case history and
   NEVER executed - the single biggest information gap. Decision tree:
     - self-readback correct AND PAR resolves to the right PA, F=0 -> worker's own MMU/cache
       path is sane; the failure is specifically cpu0->worker cross-core visibility ->
       intra-cluster snoop wall CONFIRMED -> go to Lever 4 (offload) or Lever 3 (A75).
     - self-readback STALE -> NOT coherency; the worker's MMU-on load path is broken ->
       root-cause TTBR0/TLB adoption in bigcore_entry.S. This would REOPEN a full coherent
       split (the best outcome) and close the "unfixable wall" narrative.
     - PAR mistranslates (wrong PA or F=1) -> fix the snapshot/TLBIALL; LK-local, fixable.
   STATUS: IMPLEMENTED this cycle (see snes_driver.c bc_worker_entry + bc_dispatch, new
   comms fields w_self / w_self_noinv / w_canpar_lo / w_selfpar_lo). Ready to flash.

2. **Producer offload on cpu1 (the pragmatic WIN; sidesteps the wall entirely).** Give cpu1
   a job whose inputs are snapshotted ONCE (MMU-off, at bring-up) from STATIC DRAM and that
   only ever WRITES results - the two directions proven working in ~17 flashes (worker
   MMU-off read of static data; worker->cpu0 clean+publish). For the SNES menu the natural
   fit is: cpu1 continuously (re)builds the per-card boxart caches round-robin from the
   static asset blob into fixed DRAM slots; cpu0 invalidate+reads whichever slot it needs.
   Zero per-frame cpu0->worker coherent read, so it works regardless of the wall. Directly
   attacks the scroll-time card-cache-rebuild bottleneck. Gate the design on Lever 1's
   result (if only cross-core CACHEABLE reads fail but Device reads work, a 1-word
   non-cacheable "current index" mailbox lets cpu1 prioritise; else round-robin all cards).

3. **A75 cluster1 core 0x600 (fresh-cluster admit).** Target the powered-off cluster1
   instead of a 7th core into live cluster0. A first-core-in-cluster power-on takes the
   CLUSTER-level on-finish path (larger admit incl 0x16b64 mcusys/DSU init that never runs
   on a core-level join, plus cluster1's never-yet-written CCI/DSU admit regs
   0x0C533B08/0x0C533A40). Genuine first-time full-ATF snoop-domain join, the architectural
   opposite of the broken intra-cluster late-join; also ~2x compute (A75). Staged/safe test:
   NS-read CPC_SPMC_ST(0x0C53A840) at LK to see if cores 6/7 bits are set at cold boot; if
   so PSCI CPU_ON(0x600, MMU-off heartbeat) reusing the 0x100 harness; then the probe.

4. **Worker self-exec CLUSTERPWRDN in AArch64 (DOWNGRADED, likely redundant).** ATF already
   runs write_clusterpwrdn_el1 in on-finish and forcing it did not help, so only pursue if
   Lever 1 + external evidence resurrect it. Kept for completeness.

### Definitively DEAD (do not revisit)
- "SSPM must be flipped dormant->servicing to admit a coherent core." SSPM is
  preloader-started and already live; it services mcusys-off/suspend only; it is NOT on the
  core-power-on ack path (that ack is SPM 0x10006160). This ATF barely references SSPM (one
  mmap descriptor); no SSPM mailbox code. No NS-issuable SSPM trigger helps.
- "Fake kernel jump so SSPM/SPMC is live." spmc_init is one-shot, already consumed by the
  cold-boot patch; handover lands in the same state.
- "A kernel NS SPM/CPC write at boot arms secondaries and LK can copy it." The kernel never
  NS-writes POWERON_CONFIG_EN or CPC_FLOW_CTRL; arming is an EL3 SMC. POWER_UP_CORE/CLUSTER
  SiPs are dead.
- "Re-run CCI/DSU admit or w21-ungate for cpu1." Fixed cluster addresses, already run;
  w21-gated body is cluster-level. Flashed, no effect.
- "cpu0 sets cpu1's CPUECTLR.SMPEN / toggle a coherency-enable at runtime." On A55, SMPEN is
  HW-set at reset and cannot be toggled at runtime; coherency-connect is a PPU WARM_RST
  device handshake driven by the power controller, not a core-written register.
- "Replicate spm_poweron_cpu from LK is the missing step." The LK code already does the exact
  ATF sequence with the correct register map and already acked on HW.

Key file/address references: bigcore.c:48,259,269-274; snes_driver.c:96-172 (bc_worker_entry),
188-320 (bc_dispatch + probe); bigcore_comms.h:49-109; bigcore_entry.S:38-148; bl31_dis
spm_poweron_cpu 0x126dc, PWR_CON table 0x20c70, ack read 0x12574, ack-bit 0x125b4, CLUSTERPWRDN
0x1a7f4, on_finish 0xbe88 (calls it 0xbf40), CCI admit 0x971c, DSU admit 0x99d4, spmc_init
0x125f4, cluster init 0x16b64; kernel arch/arm64/kernel/psci.c:47-55.
