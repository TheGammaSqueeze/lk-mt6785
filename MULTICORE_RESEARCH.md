# MT6785 multi-core-at-LK revival research

## === TL;DR (authoritative current understanding; read this first) ===
Some EARLY entries below are SUPERSEDED - the investigation evolved. Current definitive picture:
- SYMPTOM: an LK-brought-up worker core (PSCI CPU_ON of mpidr 0x100) is fully powered (SPM/SPMC ack bit31 set)
  but non-coherent: its LOADS miss cpu0's STORES (even uncached), while its STORES are seen by cpu0.
- MECHANISM (settled): MT6785 is TWO physical DSU clusters by MPIDR aff1 - DSU0={cpu0-3}, DSU1={cpu4-7} (the
  6/2 cpu-map is a DVFS view, not coherency). cpu0=DSU0, worker cpu4=DSU1. Cross-DSU coherency is admitted by
  CPC/interconnect HARDWARE at cluster power-on; at LK that cross-DSU snoop admission does not complete for DSU1.
  cpu0's cleaned data sits at the in-domain PoC (shared L3), which the un-admitted DSU1 cannot snoop and is not
  in DRAM - hence even uncached worker reads are stale.
- RULED OUT (all): SPM/CPC power regs, CPUECTLR/SMPEN, AArch32, cache set/way, SSPM/MCUPM firmware (no coherency
  IPI; loads at kernel stage), EMI MPU (symmetric AP-domain), software MCSI (this ATF has NO MCSI driver and does
  NOT implement the MCSI SIP block 0x28x - verified by disasm), and the DSU cluster sysregs (ATF DOES set
  CLUSTERPWRCTLR/PWRDN s3_0_c15_c3_5/6 at EL3 on bringup). So the gap is the cross-DSU INTERCONNECT snoop route.
- FIX (open, needs HW): a CPC/mcucfg interconnect register (identify via the whole-DSU1 hotplug-diff) that LK
  pokes, or an ATF patch. The SMC-based MCSI fix CANNOT work on this tee.img (SIP absent).
- DELIVERABLES (staged /mnt/c/pairmini, pushed): lk_a_snes_mcsi_signed.img (diagnostic incl. raw mcucci read),
  lk_a_snes_mcsifix_signed.img (MCSI-admit attempt, diagnostic-only here), lk_a_snes_signed.img (clean restore),
  tee_patched_armcpc.img (prereq). DECISIVE on-device test: tools/live_regpoke/run_mcucfg_diff.sh. The 2-core
  render split is already wired (bc_dispatch, wall-clock fallback, "full split channel LIVE" self-verdict) so a
  working coherency fix immediately yields the speedup. See OPERATOR_FLASH_GUIDE.md.
- STATUS: offline analysis saturated; every remaining step needs the MTK target (0123456789ABCDEF) online.
## === end TL;DR ===

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

### SECOND IMAGE (candidate fix #1, worth flashing right after the probe)
Discovery this cycle: LK's arch/arm/mmu.c was already patched to mark ALL Normal-WB memory
Inner-Shareable (SH=0b11), and TTBCR SH0 is Inner-Shareable, so the worker adopts an entirely
Inner-Shareable view -> it needs the DSU snoop admission a manually-woken core lacks. That is the
mechanism of the wall. The fix to try: map the shared handoff region Normal-WB NON-shareable
(SH=0b00) in the shared tables, so BOTH cores see it non-shareable (consistent, no mismatched
alias), the worker uses it CACHED, and the existing software clean(owner)/invalidate(reader)
handoff bridges coherency WITHOUT snoop admission.

Image: /mnt/c/pairmini/lk_a_snes_bigcore_nonshare.img  (built with AYANEO_BC_NONSHARE=yes)
Same tee.img prerequisite. Look for "BC MODE: shared region ... NON-shareable" then the same
"BC CANARY (non-comms 0x51000000): cpu0->worker worker-read=0x.." line:
  - worker-read == 0xCA5Axxxx  => THE FIX WORKS. Non-shareable cached cross-core read succeeds ->
       this is the coherency-free CACHED 2-core path -> build out a real per-frame split. HUGE.
  - worker-read still garbage   => non-shareable does not bypass the wall either -> the block is a
       true hardware snoop-domain gap -> fall back to Lever 4 (producer offload, static inputs only)
       or Lever 3 (A75 fresh cluster).

RECOMMENDED FLASH ORDER (one wake session, both are safe/isolated behind the flag):
  1. lk_a_snes_bigcore_probe.img   -> classify the wall + read RECON (A75 readiness).
  2. lk_a_snes_bigcore_nonshare.img -> directly attempt the cached-non-shareable fix.
Report the "BC CANARY" and "BC LEVER1 PROBE" and "BC RECON" lines from BOTH and the next step is
deterministic.

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
   a job whose inputs are static + a tiny control word, that only ever WRITES bulk results -
   the directions proven working in ~17 flashes (worker read of static/cleaned DRAM; worker
   ->cpu0 clean+publish). CONCRETE DESIGN grounded in the actual code (corrected 2026-08-28):
   the home carousel is NOT per-card round-robin. snes_menu_build_cardcache (snes_menu.c:926)
   renders ONE cursorless card strip into a panel-sized L2 buffer (SNES_L2_W=2496 x
   SNES_L2_BAND_H=384, ~30 ms: g_cc_us = {clear, draw, band-scan, unpremult}); it is built
   ONCE when snes_menu_cardcache_sig (snes_menu.c:834) changes, then panned every frame via
   the OVL src_x (snes_driver.c:803-829, s_cc_valid/s_cc_sig). So the ~30 ms cost lands only
   on the frame the signature changes (scroll crosses a card, selection changes), which is
   exactly the scroll-time hitch. Offload = move that single strip build off cpu0's critical
   path:
     - cpu0 writes the small carousel state (sel index, cont_shift bucket, aspect) into a
       one-line control mailbox each frame, and the cardcache_sig it wants.
     - cpu1 loops: read the control mailbox, compute cardcache_sig; if it differs from the
       strip it last built, rebuild the full 2496-wide strip into a DRAM slot cpu1 owns,
       clean it to PoC, publish {ready_sig, slot}. It can also SPECULATIVELY prebuild the
       strip for sel+1 / sel-1 so the next scroll step is already done.
     - cpu0 each frame: if the strip it needs (by sig) is published, invalidate+read that slot
       and composite via OVL (worker->cpu0, proven); else build it itself (current path) as
       the fallback. No correctness dependency on cpu1.
   Coherency needs: bulk strip is worker->cpu0 (proven). The only cpu0->worker traffic is the
   one control line; route it through whichever channel Lever 1 shows works (Device mailbox if
   Device cpu0->worker reads succeed, or the Non-shareable-WB region if candidate fix #1 works,
   or - if both fail - have cpu1 round-robin-build strips for all plausible sel positions from
   the fully static card list with NO cpu0->worker signal at all). The static inputs cpu1
   needs (boxart textures, card list, chrome) are all built at menu init and never change, so
   cpu1 snapshots their base once at bring-up. Win: the ~30 ms scroll hitch moves off cpu0, so
   scrolling holds 60 fps; and the whole-strip build can itself be X-split across the two cores
   for a lower-clock steady state once coherency is available.

   QUANTITATIVE FEASIBILITY (cycle 4, grounded in the real constants): held-scroll advances one
   card every CAR_REPEAT_RATE = 0.06 s = 60 ms (snes_menu.c:517; first step after
   CAR_REPEAT_DELAY = 0.22 s). A full strip build is ~30 ms (snes_menu.c:14,890; the 13.5 ms
   unpremult already cut by the reciprocal table). So build time (~30 ms) is HALF the held-repeat
   interval (60 ms): during sustained scrolling cpu1 can build the next-in-direction strip and
   have it ready before the next step fires, with ~2x margin. Held scroll is unidirectional so
   cpu1 only needs to prebuild the single next strip (not sel +/-1); on first-press or direction
   change there is at most one ~30 ms latency, i.e. exactly today's behaviour, so the offload is
   a STRICT improvement (removes the steady-scroll hitch, never worse than current). If both
   cores are coherent (probe/nonshare pass), the fallback build can additionally be X-split so
   even a cpu1-miss strip costs ~15 ms. Conclusion: the producer-offload holds 60 fps for all
   realistic scrolling and degrades gracefully; it is the safe delivery path once Lever 1 says
   how the one control line reaches cpu1.

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

### External corroboration (online, cycle 1)
- ARM DynamIQ/DSU: coherency-connect for a core is an AMBA LPI P-Channel handshake
  (PACCEPT/PDENY) driven by the power controller, SEPARATE from the core executing; it is
  NOT a register a core or a peer writes. This is why "toggle a coherency-enable bit after
  the fact" never worked and why an A55's SMPEN is hardware-set at reset.
- Authoritative sibling ATF source (ARM-software mt8183 drivers/spmc/mtspmc.c
  spm_poweron_cpu, and the mt8192 equivalent) shows the CPU power-ON path is ONLY: set
  PWR_RST_B, set PWR_ON, poll PWR_STATUS ack. There is NO cache-coherency / SCU-DSU-snoop /
  ACINACTM / standby-WFI step anywhere in the software power-on. So coherency admission is
  entirely the DSU hardware sequence that runs when the CPC/SPMC powers the core correctly.
  Consequence: there is genuinely no missing software snoop-admit call for LK to make. The
  only open question is whether our CPC-arm bypass powers the core in a way the DSU treats
  as a full coherency-connect (Lever 1 will tell us empirically via the self-readback).
- (cycle 2 search) ARM DynamIQ material states the DSU does "automatic enabling and disabling
  of coherency with the interconnect... without software intervention" on power up/down. So
  coherency-connect is hardware-automatic and there is NO software register to force it (this
  closes "force a coherency-enable bit" as a lever for good). It also means: if the core were
  powered through the genuine DSU/CPC sequence it WOULD be coherent automatically; if the probe
  shows it is not, the CPC-arm bypass is powering it in a way that skips the DSU auto-connect,
  and since we cannot force the connect, the sanctioned workaround is candidate fix #1.
- (cycle 2 search) ARM documents non-shareable memory + explicit clean/invalidate as the
  correct method for accesses with mismatched shareability / a non-coherent agent (ARMv8
  cache-maintenance rules; TF-A boot paths do exactly this with caches off). This is direct
  support that candidate fix #1 (shared region Normal-WB NON-shareable + sw clean/invalidate)
  is architecturally sound, raising its confidence as the top post-probe lever.

### Candidate LK-local bugs to check IF the probe returns "worker path broken / mistranslate"
(pre-staged so the fix is immediate; do NOT pre-apply, they would confound the probe)
1. TTBCR walk attrs: the worker adopts cpu0's TTBCR (mcr p15,0,r6,c2,c0,2, bigcore_entry.S:83)
   with a cacheable inner-shareable page-table walk. A non-snoop-admitted core doing a
   cacheable-shareable PTW of tables cpu0 recently modified (bc_device_map's L1 split +
   bc_l2_tbl) could walk stale/garbage. Fix to test: force the worker's TTBCR IRGN/ORGN to
   Non-cacheable (0b00) and SH to Non-shareable so its PTW goes endpoint-direct to DRAM
   (tables are already cleaned there). If that fixes cross-core reads it also strongly
   implies the wall is PTW-visibility, not data-visibility, and a non-shareable-private
   cacheable data mapping would then give full-speed cached compute (best case).
2. bc_l2_tbl PA: bc_device_map writes l1[l1i] = (u32)bc_l2_tbl | TABLE (bigcore.c:192). This
   assumes bc_l2_tbl VA==PA (LK identity map). VERIFIED REFUTED 2026-08-28: k85 MEMBASE=
   0x4C400000 and platform.c identity-maps [MEMBASE,+MEMSIZE) Normal-WB, so BSS is identity
   mapped and the L2 base is correct. Not the bug. (Probe PAR-lo PA still cross-checks it.)
3. Worker TLB: bigcore_entry.S:77 TLBIALL runs BEFORE the TTBR0/TTBCR writes (83-87). A stale
   walk-cache entry could survive. Move a TLBIALL + DSB + ISB to AFTER the TTBR0/TTBCR/MAIR
   writes and retest.
4. Snapshot ordering: bigcore_start does bc_device_map (357) then bc_snapshot_mmu (358) then
   clean comms (361). Confirm the snapshot captures the POST-device_map TTBR0 (it does, order
   is right) and that arch_clean on bc_l2_tbl (bigcore.c:189) reached PoC before the worker's
   first PTW.

### Next-experiment plan (deterministic, gated on the probe UART)
- If probe => "wall confirmed" (worker paths sane, only cpu0->worker fails): implement Lever 4
  (producer offload; see the corrected single-strip design in Lever 2 above) as the pragmatic
  2-core win; in parallel, if RECON shows CPC_SPMC_ST bits 6/7 set, try Lever 3 (PSCI CPU_ON
  0x600, A75) reusing the harness.
- If probe => "LK bug": apply candidate 1 (Non-cacheable/Non-shareable worker walk) first, it
  is the highest-value fix and could reopen a FULL coherent 2-core split.

### STATUS after cycle 1
Decisive probe BUILT + SIGNED + STAGED at /mnt/c/pairmini/lk_a_snes_bigcore_probe.img. Blocked
on one HW flash (operator asleep). All levers ranked and gated. Continuing research on Lever
3/4 mechanics and candidate-bug fixes so the post-probe step is immediate.

### STATUS after cycle 2 (2026-08-28, no HW input)
- Candidate-bug #2 (bc_l2_tbl VA==PA) VERIFIED REFUTED from sources: k85v1_64 MEMBASE=0x4C400000
  and platform.c mmu_initial_mappings identity-map [MEMBASE, +MEMSIZE) Normal-WB, so LK BSS
  (where bc_l2_tbl + bc_worker_stack live) is identity-mapped -> the L2 table PA is correct.
- Verified the Non-shareable image is well-formed: LK MAIR (mair0=0xeeaa4400, mair1=0xff000004)
  has AttrIndx 7 = 0xff = Normal Inner+Outer Write-Back, so BC_ATTR_WB=(0x7<<2) maps genuine
  cached WB; AttrIndx 0 = 0x00 = Device (matches the baseline). Both staged images are valid.
- Lever 4 design CORRECTED against the real code: the carousel card strip is a SINGLE
  panel-sized build (snes_menu_build_cardcache, ~30 ms on signature change), panned via OVL
  src_x, NOT per-card round-robin. Rewrote the offload plan accordingly (single-strip producer
  with a one-line control mailbox + speculative sel+/-1 prebuild). See Lever 2 section.
- No new flashable image this cycle (the two staged images remain the decisive gate; further
  images are gated on the probe UART). Next HW-independent readiness task when resumed: begin
  the Lever 4 producer skeleton behind a flag (host-validatable), or Lever 3 0x600 prep.

### STATUS after cycles 3-5 (2026-08-28, no HW input)
- Cycle 3: external RE. ARM DynamIQ docs confirm the DSU does coherency-connect AUTOMATICALLY in
  hardware on power-up (no software register to force it) and that non-shareable + clean/invalidate
  is the sanctioned non-coherent-access method (boosts confidence in candidate fix #1). Recorded in
  the External corroboration section.
- Cycle 4: quantitative Lever 4 feasibility. Held-scroll steps every CAR_REPEAT_RATE=60ms vs a
  ~30ms strip build, so cpu1 stays ~2x ahead; the producer offload holds 60fps and degrades to
  today's behaviour. Recorded in the Lever 2/4 section.
- Cycle 5: IMPLEMENTED + HOST-VALIDATED the 2-core strip split (the mechanism for the low-power
  steady state: halve the ~30ms build to ~15ms across cpu0/cpu1). Added snes_menu_build_cardcache_band
  (snes_menu.c) using the proven scanline band-clip that bc_dispatch already uses for
  snes_menu_render; left snes_menu_build_cardcache UNTOUCHED so the single-core menu is byte-identical
  and cannot regress. New host_render.c "vsplit" mode proves full == [0,mid)+[mid,H) pixel for pixel:
  "VSPLIT: PASS (958464 px identical)" in both 4:3 and native aspect. Device release build clean
  (rc=0); the function is available but not yet called (its on-device use needs cpu1 running the
  build, gated on the probe/coherency result). This is reusable regardless of which coherency path
  wins: with a coherent cpu1 it halves every strip build; it also composes with the producer offload
  (a cpu1-miss strip can be X/Y-split to ~15ms). Net: the split-build lever is now DONE and proven,
  not just designed.

### STATUS after cycle 6 (2026-08-28, no HW input): split MUST be content-balanced
Generalised the vsplit host test to prove the split is exact at ARBITRARY rows (buffer-mid,
content-mid, quarter, three-quarter, and row 1), with a 0xDEADBEEF poison-fill confirming the two
bands together cover every pixel. All PASS. But the balance data is the real finding:
  - native aspect: content band rows [42,317]; buffer-mid (192) => cpu0 150 / cpu1 126 rows
    (~54/46, tolerable); content-mid (179) => 137 / 139 (balanced).
  - 4:3 aspect:    content band rows [136,383]; buffer-mid (192) => cpu0 56 / cpu1 192 rows
    (23/77, BADLY unbalanced -> ~1.3x not 2x); content-mid (259) => 123 / 125 (balanced).
So the 2-core split MUST cut at the CONTENT-band midpoint (cc_y0+cc_y1)/2, NOT the buffer midpoint,
or the low-power win mostly evaporates in 4:3 mode. Since the vertical card layout is stable during
a horizontal scroll, cc_y0/cc_y1 from the previous build is a safe split point for the next (first
build falls back to buffer-mid). build_cardcache_band already accepts arbitrary r0/r1, so this is a
pure caller-side choice when the split is wired in; no engine change needed. This is exactly the
kind of thing that would have silently halved the expected speedup on HW; caught it on the host.

### HOLDING STATE
The decisive gate remains one HW flash of the two staged images. Remaining readiness tasks are
genuinely blocked: Lever 4 producer wiring and Lever 3 0x600 bringup both need the probe result
(coherency mechanism + CPC_SPMC_ST) before they can be built correctly; the non-shareable-walk
refinement is low-value (the worker PTW already works). The split-build lever is now DONE, proven
exact at any row, and its balance requirement is known. On HW input, execute the OPERATOR NOTES
decision tree.

### HW PROBE RESULT (2026-08-28, decisive) - WALL CONFIRMED
Flashed tee_patched_armcpc.img + lk_a_snes_bigcore_probe.img. UART (putty.log):
  CPC_FLOW=0xb0000 (CPC_CTRL_ENABLE set); PSCI CPU_ON ret=0; magic+cached_ok=0xb16c0de5, cpu1 up.
  BC RECON: CPC_SPMC_ST=0xc001  SPM_CPU_PWR_STATUS=0x434c (cpu1 ack bit10 set = powered).
  BC LEVER1 PROBE: w_self_wb=0x5e1f3012  w_self_dev=0x0de03012  canpar_lo=0x51000b00  selfpar_lo=0x4c5d5b80
  BC CANARY: cpu0->worker worker-read=0xa86dbdec (want 0xCA5A) ; worker->cpu0 cpu0-read=0x77773012 (OK)
  BC WORKER FAULT: dabt far=0xab102d00 (deref of garbage menu_ptr 0xab102d01), stage=0x22.

CLASSIFICATION (Lever 1 decision tree):
- w_self_wb == 0x5E1F.. AND w_self_dev == 0x0DE0.. -> the worker's OWN WB-cached and Device paths
  are SANE (it reads back its own writes with the MMU on).
- canpar_lo bit0=0, PA=0x51000, selfpar_lo bit0=0 -> the worker's TRANSLATION of the failing VA is
  CORRECT (right PA, no fault). So no TTBR0/TLB bug.
- cross-core: worker->cpu0 WORKS (0x77773012); cpu0->worker FAILS with FIXED garbage (0xa86dbdec,
  identical every frame), even though the region is Device-nGnRnE (the most endpoint-direct type).
=> The failure is PURELY cpu0->worker cross-core read visibility. The intra-cluster DSU snoop
   admission wall is CONFIRMED on HW. It is NOT a fixable LK bug. (Candidate bugs #1-4 all refuted:
   translation correct, worker cache/Device sane.)

SHARPENED PLAN (this changes the nonshare odds UPWARD):
- The failing baseline is Device-nGnRnE OUTER-SHAREABLE. A key ARM detail: with the MMU OFF the worker
  READ cpu0's data fine (it adopted the MMU snapshot MMU-off), but MMU-ON outer-shareable reads fail.
  The un-admitted worker issuing SHAREABLE-domain transactions is exactly what the DSU will not service.
- Candidate fix #1 (lk_a_snes_bigcore_nonshare.img) maps the region NON-shareable (SH=0b00), so the
  worker issues NON-shareable transactions that need no shareability-domain membership -> endpoint-direct
  to DRAM. This is precisely the axis the baseline failed on, so it is the RIGHT next test and its odds
  are better than the Device-outer-shareable baseline suggested. FLASH IT NEXT; look for
  "BC CANARY ... worker-read=0xCA5Axxxx".
- If nonshare ALSO fails: the wall is total for post-bringup cross-core reads -> go to Lever 4 (producer
  offload). Note the proven-good directions from THIS run: worker reads STATIC pre-bringup-cleaned DRAM
  (the MMU snapshot read worked) and worker->cpu0 writes work; Lever 4 uses only those. The one dynamic
  cpu0->worker signal must be avoided (round-robin) or carried some other way.
- Lever 3 (A75 0x600): CPC_SPMC_ST=0xc001 has bits 6/7 CLEAR, so cluster1 is not reset-released at cold
  boot; it would need a larger cold-boot patch. Lower priority than the nonshare test.

### IF the nonshare canary works (0xCA5A): the concrete extension plan
The staged nonshare image only remaps the CANARY 2MB (0x51000000) non-shareable; the real render
reads three more cross-core regions that are still Inner-Shareable, so a canary win is necessary but
not sufficient. To make the full 2-core render work, every region cpu0 hands to the worker must be
non-shareable. Regions (from the probe run + bc_dispatch):
  - comms block 0x54000000 (job/menu_ptr/go/done) - in the SNES scratch window, safe to remap via
    the existing bc_device_map L2-split (add a second call for the 0x54000000 2MB).
  - scene pool 0x50C00000 (4MB, the rnode list the render walks) - also scratch, safe to remap.
  - the menu struct itself: menu_ptr=0x4c5d5584 is in LK's OWN region (MEMBASE 0x4C400000), NOT a
    scratch page. Remapping LK BSS non-shareable is risky (LK/allocator may rely on coherency there).
    CLEANER: relocate the cross-core payload into a dedicated non-shareable ARENA in the scratch
    window - cpu0 copies the 4KB menu struct (and points menu_ptr) into the arena each frame (cheap),
    and builds/points the scene pool inside the arena; the worker reads only the arena. The framebuffer
    band the worker writes needs no remap (worker->cpu0 writes are proven to work).
So the follow-up build = bc_nonshare of {0x51000000 canary, 0x54000000 comms, 0x50C00000 scene} + a
menu-struct copy into a non-shareable arena, then re-enable the bc_dispatch band split (both render
splits are already host-validated exact). Do NOT pre-build this until the canary confirms non-shareable
fixes the basic cross-core read - it is gated on that one datapoint. If the canary FAILS, skip all of
this and go to Lever 4 (producer offload) using the proven MMU-off-static-read + worker->cpu0-write
directions, with the dynamic selection carried by a brief MMU-off read window (the one cross-core read
path that worked on HW).

### HW NONSHARE RESULT (2026-08-28): candidate fix #1 REFUTED
Flashed lk_a_snes_bigcore_nonshare.img. UART:
  BC MODE: shared region 0x51000000 mapped Normal-WB NON-shareable
  worker PAR-hi of 0x51000000 = 0xff000000 (ATTR 0xff = Normal-WB, so the remap took)
  w_self_wb=0x5e1f3052  w_self_dev=0x0de03052  canpar_lo=0x51000a00 (worker paths still SANE)
  BC CANARY: cpu0->worker worker-read=0xa86dbdec (want 0xCA5A) ; worker->cpu0=0x77773052 (OK)
  cpu0 self-readback=0xca5a0001
=> Non-shareable WB fails IDENTICALLY to Device-Outer-Shareable, returning the SAME frozen value
   0xa86dbdec. The wall is total and ATTRIBUTE-INDEPENDENT: once the worker's MMU is on it reads a
   frozen view and never observes cpu0's post-bringup writes, for any memory type or shareability.
   Candidate fix #1 is dead. The identical fixed garbage across Device and WB-nonshare runs supports
   a "frozen shadow" (the worker's read view of shared DRAM is fixed at MMU-enable time; cpu0's later
   writes + cache maintenance never reach it because the worker is not a snoop participant).

### NEXT: the decisive MMU-OFF-readback probe (built + staged)
The one cross-core read path that WORKED on HW was the MMU-OFF snapshot read at bringup. Never tested:
does an MMU-OFF read AFTER the worker's MMU has been on still see cpu0's fresh writes (i.e. does
dropping the MMU un-freeze the view)? This is the hinge for whether ANY cpu0->worker data flow is
possible post-bringup:
  - if the MMU-off read returns 0xCA5Axxxx -> a per-frame MMU-off window can carry cpu0->worker data
    (the dynamic selection for Lever 4), so producer-offload is viable.
  - if it also returns 0xa86dbdec (frozen) -> cpu0->worker is dead by every means; the only multicore
    model left is worker doing work from inputs cpu0 wrote+cleaned BEFORE the worker enabled its MMU
    (fully static), and even then the dynamic per-frame selection cannot reach it -> the card-strip
    offload is not achievable and multicore-at-LK is effectively closed for this workload.
Image: /mnt/c/pairmini/lk_a_snes_bigcore_mmuoff.img (Device baseline + the worker drops SCTLR.M,
reads 0x51000000 physically, restores M; identity map keeps PC/SP valid). Look for "BC MMUOFF PROBE:
worker MMU-off read of 0x51000000 = 0x..".

### HW MMU-OFF RESULT (2026-08-28): cpu0->worker is DEAD by every means
Flashed lk_a_snes_bigcore_mmuoff.img. UART:
  BC MMUOFF PROBE: worker MMU-off read of 0x51000000 = 0xa86dbdec (frozen)
  (canary/self/lever1 all as before: worker paths sane, cpu0->worker garbage 0xa86dbdec)
=> Even an MMU-OFF read AFTER the worker's MMU was on returns the SAME frozen value. So the freeze is
   not the worker's own MMU/D-cache (invalidated first; Device is uncached anyway) - it is a deeper
   DSU-level disconnect. Definitive: the worker can correctly read only (a) memory IT wrote and (b) the
   pre-MMU-enable snapshot; cpu0's post-bringup writes are invisible through EVERY path tried
   (MMU on/off, WB Inner-Shareable, WB Non-shareable, Device Outer-Shareable). worker->cpu0 writes
   always work. This is the DSU snoop-admission wall in full: an un-admitted core is a write-only
   participant with a frozen read view.

CONSEQUENCE FOR THE GOAL: a per-frame 2-core render split (the low-power target) REQUIRES cpu0 to
hand the live menu state to the worker every frame (cpu0->worker), which is impossible. So the
per-frame split is unreachable at LK. The only multicore models that survive:
  - STATIC-prebuild offload: the worker builds outputs from inputs cpu0 wrote+cleaned BEFORE the
    worker enabled its MMU (frozen snapshot is correct), and writes results out. For the card strip
    this means pre-tiling the STATIC card row into 2496px chunks the worker builds once; cpu0 reads
    the chunk for the current focus (worker->cpu0). But cpu0 could prebuild those itself at startup,
    so the 2nd-core benefit is only the one-time startup cost - marginal, not the per-frame goal.
  - EXOTIC (untested): give cpu0->worker a channel on a DIFFERENT memory path than DSU-coherent DRAM
    (on-chip SYSRAM/SRAM), which may not go through the frozen snoop path; OR have the worker read
    input MMIO directly and run its OWN menu state machine in lockstep (fragile dt/RNG sync). Both are
    long shots.

RECOMMENDATION: the DRAM cpu0->worker wall is now proven absolute. Unless the SRAM-channel probe (a
cheap 1-word test) works, multicore-at-LK cannot deliver the per-frame low-power split, and the clean
single-core release build (unaffected; the fps drop is only the EXPT fork/join+fallback overhead)
should stand. Decision point for the operator.

### LIVE-SYSTEM RECON (2026-08-28): adb rooted device attached, big new leverage
Device 0123456789ABCDEF, Android on kernel 4.14.186, root ok. Findings:
- NO /dev/mem (CONFIG_DEVMEM off), mknod gives ENODEV. BUT CONFIG_MODULES=y and CONFIG_MODULE_SIG is
  NOT set, and out-of-tree drivers already load (wlan_drv_gen4m etc). => a custom register-poke kernel
  module (ioremap) can read/write ANY physical register on the live coherent system. This is the tool
  that replaces /dev/mem for the hotplug snoop-admission register diff.
- Topology confirmed: cpu0-5 A55 (0xd05), cpu6-7 A75 (0xd0b), single DSU. All 8 online; cpuN/online
  writable (hotplug works) -> can diff coherent-core register state on/off.
- MTK power surface live: /proc/mcdi/{cpc,info,state}, /proc/mtk_lpm/cpuidle/spm, /proc/cpuhvfs.
  MCDI (SSPM-driven) actively power-manages both clusters. IPI_ID_MCDI=6 over mbox1; spmfw loaded
  (pcm_suspend_v02.05). No ftrace function tracer (available_filter_functions absent), so use the module
  + hotplug diff, not function_graph.
- Register blocks (from DT): mcucci@0x0c510000 (CCI/coherency, 64KB), mcucfg@0x0c530000 (64KB),
  cpcspmc_reg@0x0c53a700, cpccfg_reg@0x0c53a800, sspm@0x10400000 + mbox0..3 @0x10450000/60/70/80,
  psmcu_misc@0x80200000. SPM base 0x10006000.

### ATF RE VERDICT (mt8192 same-gen source): the coherent on-path, and why our bypass fails
plat_power_domain_on -> spm_poweron_cpu (mtspmc.c): the ENTIRE on-side hw sequence is
  set  MCUCFG_CPC_FLOW_CTRL_CFG(0x0c530000+0xa814) |= SSPM_ALL_PWR_CTRL_EN (bit13, "for cpu-hotplug")
  set  SPM_MP0_CPUx_PWR_CON |= PWR_ON (bit2)
  poll SPM_MP0_CPUx_PWR_CON & PWR_ON_ACK (bit31)      <-- the SPMC/DSU snoop-admission handshake
  clear SSPM_ALL_PWR_CTRL_EN
No CCI/DSU snoop sysreg is written; DynamIQ A55/A75 have NO software SMPEN/ACINACTM (that was the
legacy CCI mt8173 path). So snoop admission is performed by the SPMC/CPC hardware FSM while it services
PWR_ON, and the ATF hands the flow to SSPM via bit13. Our LK "CPC-arm bypass" powers the core but never
completes the PWR_ON->PWR_ON_ACK handshake, which is precisely the snoop admission => write-only,
frozen-read core. CONSISTENT with every HW probe.

OPEN QUESTION (decisive): does the SPMC FSM complete PWR_ON_ACK WITHOUT SSPM firmware servicing (pure
HW mode), or is SSPM mandatory? If HW mode exists, LK can drive the exact spm_poweron_cpu sequence
(bit13/PWR_ON/poll ACK) directly instead of the CPC-arm bypass and get a COHERENT core -> the whole
2-core split becomes reachable. Resolve with: (1) the register-poke module reading SPM_MP0_CPUx_PWR_CON
+ cpcspmc on live coherent cores and across hotplug; (2) mt6785-specific ATF spmc source (not local yet;
websearch/obtain). NEXT LK IMAGE: replace the CPC-arm bypass with the real PWR_ON+ACK-poll sequence.

### CRON: switched to RESEARCH MODE v2 (adb-enabled), 15-min. Priorities: build register-poke module,
hotplug-diff snoop bits, determine SSPM-free HW-mode feasibility, then drive real PWR_ON+ACK at LK.

### REFINED LEVER (2026-08-28): the ack-register distinction (new, actionable)
Read bigcore.c bc_manual_poweron (250-294). It already does the spm_poweron_cpu shape: set
SSPM_ALL_PWR_CTRL_EN (MCUCFG+0xa814 bit13), set SPM_CPU_PWR_CON(cpu)=SPM+0x768+(n<<2) PWR_ON bit2,
then POLL `SPM_CPU_PWR_STATUS (SPM+0x160) & ACK_BIT(cpu)` where ACK_BIT = 1<<(cpu+9). That ack DID set
on HW (RECON SPM_CPU_PWR_STATUS=0x434c, bit10 for cpu1). BUT SPM+0x160 is the aggregate RAIL-power
status; the ATF/kernel coherent model completes the handshake on the PER-CPU register
SPM_CPU_PWR_CON(cpu) bit31 = MP0_SPMC_PWR_ON_ACK (see mt6873/85 spm_reg.h: SPM_CPUn_PWR_CON ...
PWR_ON_ACK_CPUn = 1U<<31). These are DIFFERENT registers. Polling rail-power-on instead of the per-CPU
SPMC PWR_ON_ACK could mean LK proceeds after the rail is up but BEFORE the SPMC FSM finishes the
snoop-admission handshake -> powered but non-coherent core, exactly what we see. This is a fresh,
testable hypothesis distinct from "SSPM mandatory".

TWO teed-up experiments (next cycle):
1. LIVE (kernel module, no flash): build a register-poke .ko (vermagic "4.14.186 SMP preempt mod_unload
   modversions aarch64"; MODVERSIONS=y so lift symbol CRCs from a shipped vendor .ko e.g.
   /vendor/lib/modules/*.ko, or patch __versions). Read on a KNOWN-coherent online cpu1: SPM+0x160,
   SPM_CPU_PWR_CON(1) (bit31), MCUCFG_CPC_SPMC_ST(+0xA840), mcucci@0x0c510000 head. That gives the
   TRUE coherent-core ack signature to compare against LK. Also confirms the mt6785 SPM_CPU_PWR_CON
   offset (LK uses 0x768+(n<<2), "base from ATF rodata" - unverified for mt6785).
2. LK IMAGE: add a probe that, after bringup, reads+logs SPM_CPU_PWR_CON(cpu) bit31 (per-CPU
   PWR_ON_ACK) and each MCUCFG_CPC_SPMC_ST bit, and (gated behind a flag) POLLS the per-CPU bit31
   instead of SPM+0x160 before releasing the worker. If bit31 sets at LK -> the FSM is HW-autonomous
   and correct polling yields a coherent core (win). If bit31 never sets -> SSPM servicing is truly
   mandatory (then RE the minimal MCDI-enable IPI).

### HW ACKPROBE v2 (2026-08-28): power-on is COMPLETE - reframes the whole problem
Corrected offset (MP0_CPU1_PWR_CON = SPM+0x20C). After PSCI CPU_ON:
  SPM_CPU_PWR_CON(1)=0x80000005 = PWR_ON_ACK(b31) | PWR_ON(b2) | PWR_RST_B(b0)  => PWR_ON_ACK SET
  CPC_SPMC_ST=0xc003 (cpu1 bit1 set) ; railstat 0x160=0x474c (cpu1 bit10 set)
=> The per-CPU SPMC PWR_ON_ACK is SET. The core is powered EXACTLY as a kernel cpu_up leaves it
   (same PSCI CPU_ON path, same completed handshake). So the frozen-read is NOT a power-sequencing or
   ack-polling bug (that hypothesis is refuted), and it is not "the SPMC never admitted the core" at the
   power-controller level (it did: PWR_ON_ACK + CPC_SPMC_ST + rail all set).

WHAT THIS LEAVES: the core is fully powered/acked yet its reads of cpu0's writes are frozen even for
Device/non-shareable/MMU-off. Since Device+non-shareable reads don't need coherency-domain membership
and STILL return stale, this is not (only) a snoop-domain issue - it looks like the worker's memory
READ path does not observe the same DRAM/point-of-coherency cpu0 writes, while its WRITES do reach
cpu0. Remaining suspects (need live ground truth): DSU L3 / mcucci coherent-interconnect config;
EMI/memory routing that the full SSPM kernel sequence programs but ATF-at-LK does not; or the AArch32
secondary's post-reset cache/coherency state. NOTE the prior "un-admitted snoop participant" framing is
now only partially right: power admission IS complete; the read-visibility gap is a separate mechanism.

NEXT (decisive): build the register-poke kernel module (task 28) and read, on the LIVE coherent cpu1,
the DSU/mcucci (0x0c510000) + mcucfg (0x0c530000) coherency config + L3 control, and hotplug-diff
(offline/online cpu1) to capture what the kernel path programs that ATF-at-LK does not. Compare to the
LK register state. That difference is the fix target.

### LIVE MODULE + HOTPLUG DIFF (2026-08-28): coherency admission is NOT a register - it is cache init
Built a MODVERSIONS-matching register-poke kernel module (tools/live_regpoke/, vermagic
"4.14.186 SMP preempt mod_unload modversions aarch64") and loaded it on the live rooted device.
Arbitrary physical reads work. Findings:
- MP0_CPU1_PWR_CON (0x1000620c) on the LIVE coherent system = 0x80000005, IDENTICAL to our LK core.
  So the per-CPU power/ack state is not the difference.
- CPC_FLOW_CTRL (0x0c53a814) live=0x200b0000 vs LK 0xb0000 (bit29 set live) - investigated, transient.
- Hotplug diff (cpu1 online -> offline -> online, reading SPM 0x200-0x224 + CPC 0x0c53a700..+0x140):
  the ONLY registers that change are the per-CPU MP0_CPUn_PWR_CON power bits (PWR_ON b2 / PWR_ON_ACK
  b31 toggling) and a transient CPC_FLOW status bit - and the SAME toggles appear on cpu3/6/7
  (MCDI idle power-gating noise). There is NO distinct snoop/coherency-admission register that flips on
  bring-up. => coherency admission is performed automatically by the DSU power handshake, which our LK
  core ALREADY completes (PWR_ON_ACK set). Confirms the ATF-RE conclusion empirically.

CONCLUSION BY ELIMINATION: the LK read-incoherence is NOT a missing shared register write (power, CPC,
SPM all match live). It is CORE-INTERNAL state. The worker entry stub (bigcore_entry.S:46-49) DELIBERATELY
skips L1 D-cache invalidation, on the assumption ATF already did it. But a freshly SPMC-powered core's
L1 D-cache is in an UNKNOWN state and MUST be invalidated by set/way before SCTLR.C is set. If it is not,
the worker's reads hit stale/garbage cache lines (the fixed 0xa86dbdec) while its writes allocate fresh
lines that drain to DRAM (cpu0 sees them) - which reproduces write-works / read-frozen EXACTLY, and the
fixed value = deterministic power-on cache content. The stub's own comment ("DCISW is not broadcast") is
a misunderstanding: set/way invalidation is LOCAL cache init, which is precisely what a powered-on core
needs; it is not meant to be broadcast. This is the leading root cause and a cheap LK fix.

NEXT: add a full L1 D-cache invalidate-by-set/way loop at the very top of bigcore_entry.S (before the
MMU+cache enable), rebuild the ackprobe/EXPT image, stage for flash. (adb cannot test the LK worker's
internal cache directly, but the hotplug diff rules out every shared-register alternative, so this is
the highest-probability fix.)

### CORRECTION (2026-08-28, same cycle): the D-cache set/way dimension is ALREADY exhausted
Reading bigcore_entry.S:46-56 in full: a PRIOR version of the stub HAD an L1 D-cache invalidate-by-set/way
loop and the author REMOVED it, concluding DCISW corrupts DSU coherency (set/way is not snoop-broadcast;
ARMv8 D5.7.8 makes it safe only when a core is being REMOVED from coherency). The comment claims the
no-set/way version reads cpu0's cached writes. BUT all our HW probes (canary/nonshare/mmuoff) show the
CURRENT no-set/way build is STILL frozen. So: WITH set/way -> frozen (author); WITHOUT set/way -> frozen
(our probes). The cache-invalidate dimension is exhausted and is NOT the fix. (My prior "add the invalidate
loop" conclusion is withdrawn.)

WHERE THIS LEAVES US (all shared registers match live; power handshake complete; cache set/way tried
both ways): the remaining suspects are CORE-INTERNAL to the AArch32 worker and specific to how we bring
it up vs a real AArch64 kernel/ATF secondary:
- CPUECTLR/CPUACTLR coherency bits not set on our AArch32 worker (a real kernel secondary gets them via
  the ATF reset handler / cpu errata init). Need the coherent-core values - but reading CPUECTLR_EL1 /
  CLUSTERECTLR_EL1 live risks an EL1 UNDEF -> kernel oops, and the user is asleep with no reflash, so do
  NOT read trapping sysregs on the live device tonight. Get the values from kernel/ATF SOURCE instead.
- The worker runs AArch32 while every coherent secondary on this SoC runs AArch64; a 32-bit PE in this
  DSU may not participate in inner-shareable coherency the same way (worth checking if AArch64 worker
  bring-up changes the result - but that is an LK flash, deferred).
- cpu0-side: are cpu0's shared writes actually reaching the point the coherent worker snoops from
  (shareability of cpu0's mapping / clean-to-PoC timing)? Re-examine with the worker assumed coherent.

The LIVE MODULE remains the key tool (tools/live_regpoke) for any non-trapping register question. Next
safe adb step: read CPUECTLR setup from kernel SOURCE (arch/arm64 errata + MTK) to learn the exact
coherency bits a coherent core carries, and check whether our AArch32 worker can/should set them.

### SOURCE: no coherency-enable CPUECTLR bit exists (2026-08-28)
ATF cortex_a55.S/a75.S reset funcs set only ERRATA/perf bits in CPUACTLR/CPUECTLR (disable dual-issue,
disable write-streaming, L1WSCTL, disable-L1-pagewalks - all errata workarounds). No SMPEN/coherency
-enable bit (DynamIQ A55/A75 are coherent by default once DSU-powered). Kernel arch/arm64 touches
CPUECTLR nowhere for coherency. So there is NO missing coherency register/sysreg bit anywhere: power
(PWR_ON_ACK), CPC, SPM all match the live coherent core; hotplug shows no admission register; no CPUECTLR
coherency bit. The worker SHOULD be coherent, yet its reads are frozen even MMU-off/Device.

STANDING PUZZLE + TOP NEXT LK TEST (deferred, needs a flash): the single remaining difference is that our
worker runs AArch32 while every coherent secondary on this SoC runs AArch64. The next decisive LK image
should bring the worker up in AArch64 (leave the INITARCH AArch64 bit set, provide an A64 entry stub that
adopts cpu0's TTBR1/TTBR0 64-bit MMU) and re-run the canary. If an A64 worker reads coherently, AArch32
participation in this DSU was the wall. Errata bits (write-streaming disable etc.) are a secondary thing
the A32 worker could also mirror via CP15. The live-regpoke module stays available for any further
non-trapping register question.

### CORRECTION 2 (2026-08-28): AArch32 is NOT the wall (cpu0 is AArch32 + coherent)
The comms MMU snapshot includes DACR (an AArch32-only register) and all MMU programming is CP15/p15, so
LK/cpu0 itself executes AArch32 and IS coherent. So AArch32 execution state cannot be the coherence wall,
and the "bring the worker up AArch64" test is both weak (cpu0 disproves it) and impractical (an A64 core
cannot adopt cpu0's AArch32 LPAE snapshot). Deprioritized.

TRUE REMAINING DIFFERENCE (honest state): cpu0 and the worker are both AArch32, same cluster0/DSU, same
adopted MMU, and every power/CPC/SPM register matches. The ONLY real difference is TIMING: cpu0 is the
COLD-BOOT core that joined the DSU coherency domain when the cluster/L3 was first powered and clean; the
worker is a LATE-powered core that joins an already-running L3/snoop domain. The frozen-even-MMU-off/Device
symptom means the worker's LOADS do not observe cpu0's stores at the point of coherence, while its STORES
are seen by cpu0. This is a DSU late-join / snoop-filter-sync behavior, not a register we have found. Hard
to probe further without risky live writes (deferred while user is away) or LK flashes.

CANDIDATE NEXT DIRECTIONS (for future cycles):
- SSPM firmware angle: at kernel time SSPM runs the MCDI service that performs CPU power AND may do the
  DSU/L3 sync for late-joined cores. Even though PWR_ON_ACK completes without it, the SNOOP-FILTER
  inclusion of a late core might be an SSPM/MCUPM step. RE the SSPM MCDI firmware bringup (mbox1) for any
  post-power "coherency sync" step, and whether LK can invoke it.
- DSU CLUSTERECTLR/ACP/L3 config: read via a CAREFULLY-guarded module (avoid EL3-only regs) or from
  kernel source, to see if late cores need an L3/snoop-filter action.
- cpu0-side: have cpu0 issue a DVM/TLBI+DSB ISH broadcast or a dummy coherent transaction after the worker
  joins, to force the DSU to include it, then re-test (LK experiment).

### MMIO SWEEP COMPLETE (2026-08-28): no snoop-admission register anywhere
Hotplug-diffed (cpu1 online/offline/online) the remaining mcusys MMIO blocks with the live module:
- mcucci (0x0c510000, first 4KB): only 4 non-zero words, all at 0xff0-0xffc = component-ID bytes
  0x43 0x42 0x49 0x55 ("CBIU"), constant across hotplug. No config, no change.
- mcucfg (0x0c530000, first 4KB): 29 non-zero words; the ones that change (0x530224 ticking 5b3a->5b38,
  and 0x530700/0a00/0f00 groups going non-zero<->0 with cpu1 on/off) are PER-CPU PERFORMANCE COUNTERS
  (DT node mcucfg_mp0_counter@0c530000), not coherency config.
So across SPM (0x10006200), CPC (0x0c53a700), mcucci (0x0c510000) and mcucfg (0x0c530000) there is NO
memory-mapped snoop/coherency-admission register that toggles when a core joins/leaves coherency. This
definitively confirms: DSU snoop admission is INTERNAL to the DSU hardware (driven by the PACTIVE/PDENY
power handshake), not an MMIO register write. Our LK core completes that handshake (PWR_ON_ACK=1), so the
frozen-read is NOT a missing register write of any kind. The cause is DSU-internal late-join state or a
memory-path effect, not software-visible via MMIO. Next: SSPM/MCUPM firmware RE (does it do a post-power
L3/snoop-filter sync a late core needs) and the cpu0-side DVM/TLBI-ISH broadcast LK experiment.

### RIGOROUS ELIMINATION + DVM LEVER (2026-08-28, continuous adb session)
The entire hypothesis space is now empirically eliminated:
- POWER: PWR_ON_ACK(bit31) SET at LK, MP0_CPU1_PWR_CON=0x80000005 IDENTICAL to the live coherent core.
- MMIO SNOOP REGISTER: hotplug-diffed SPM(0x10006200), CPC(0x0c53a700), mcucci(0x0c510000),
  mcucfg(0x0c530000) on the live device - only power-gating/counter bits change; NO snoop-admission reg.
- CACHE INIT: git history (17247f4 add, a9434f0 remove) shows a CORRECT DCISW-by-CCSIDR-geometry invalidate
  before cache-enable was present through multiple failing runs; removing it did not fix it either. Both
  directions frozen. Cache-init is not the cause.
- AArch32: cpu0/LK is itself AArch32 (has DACR) and coherent, so execution state is not the wall.
- SHAREABILITY/ATTR: Device, WB-inner-shareable, WB-non-shareable all frozen.
- FIRMWARE MAILBOX: mt6785 SSPM MCDI mailbox (MCDI_MBOX slot map) is pure idle power-off coordination
  (CLUSTER_CAN_POWER_OFF/ATF_ACTION_DONE/AVAIL_CPU_MASK) - no L3/coherency/snoop slot. mt8192 MCUPM
  L3_CACHE_MODE is a different-gen mechanism (its mbox at 0x0c55fce0 reads 0xdeaddead = unmapped on mt6785).
- 0xa86dbdec is WORKER-GENERATED (live read of 0x51000000/0x54000000 = 0, normal DRAM), not a decode artifact.

CONVERGED MECHANISM: the worker's WRITES reach shared DRAM (cpu0 reads them; worker->cpu0 canary works) and
it reads back its OWN writes, but it NEVER observes cpu0's writes even after DC IVAC. That is a core that is
powered + PWR_ON_ACK'd but is NOT RECEIVING SNOOPS (snoop-input side of DSU not wired). The SSPM-controlled
kernel bringup establishes this; our CPC-arm + PSCI path completes PWR_ON_ACK with identical registers but
apparently leaves the snoop-input unwired. No MMIO/firmware/cache lever closes it.

LAST UNTRIED MECHANISM (staged): lk_a_snes_bigcore_dvm.img - cpu0 issues TLBIALLIS (Inner-Shareable) + DSB
ISH after publishing the canary, forcing a DVM Sync round-trip across the inner-shareable domain. If a DVM
transaction is what finally wires the late core's snoop-input, the worker canary flips 0xa86dbdec ->
0xCA5Axxxx. Flash lk_a_snes_bigcore_dvm.img + tee_patched_armcpc.img; read BC CANARY. Low-probability but it
is the one coherency-fabric interaction not yet tried, and cheap. If it fails too, the honest conclusion is
that coherent snoop-input for a manually-woken core at LK requires the SSPM CPU-power service to be running
(which it is not at LK), and that is the fundamental wall - the offload must then use only the proven
directions (worker reads static pre-bringup data + worker->cpu0 writes).

### MECHANISM PINNED (2026-08-28): CPC-arm bypass skips the DSU coherency P-Channel
Three independent sources converge:
1. ARM DynamIQ DSU TRM (web): the DSU enables/disables coherency with the interconnect AUTOMATICALLY
   during core power up/down "without software intervention", via the power-controller P-Channel. A
   properly-powered core is auto-coherent; there is no software snoop-enable (matches our MMIO sweep).
2. MTK MCSI snoop-filter code (mt8183 mcsi.c: cci_init_sf / cci_enable_cluster_coherency / per-slave-
   interface config) shows the lineage concept, but on mt6785/DSU that is absorbed into the DSU (only
   MCSI_DCM clock bits remain), i.e. handled by the P-Channel, not software.
3. mt6785 SSPM MCDI mailbox is idle power-off coordination only (no bringup/coherency slot).
=> The coherent-bringup chain is: ATF PSCI CPU_ON -> SPMC power request (SSPM_ALL_PWR_CTRL_EN) -> SSPM
   services it -> SPMC drives the DSU P-Channel -> DSU auto-enables coherency for the core. At LK, SSPM
   does NOT service the request (a plain PSCI CPU_ON HANGS - no ack), so we use the CPC-arm bypass, which
   makes the CPC hardware power the core + set PWR_ON_ACK, but WITHOUT the SSPM-driven SPMC->DSU P-Channel
   coherency handshake. Result: identical power registers, but the DSU never auto-joined the core to
   coherency -> writes drain out (seen by cpu0) but the core never receives snoops (frozen reads). This
   is exactly consistent with every probe.

CONSEQUENCE: the fix requires the DSU coherency P-Channel to fire for the core, which only the full
SSPM-serviced SPMC sequence does. No MMIO/firmware lever at LK triggers it (verified). Two theoretical
paths remain, both needing a flash and both hard: (1) get SSPM to service SPMC CPU-power at LK so a plain
PSCI CPU_ON completes the P-Channel (requires starting SSPM's CPU-power service, which is NOT the MCDI
idle mailbox and may not be reachable/loaded at LK); (2) the staged DVM lever (lk_a_snes_bigcore_dvm.img)
- a long shot that DVM traffic nudges the DSU to include the core. If both fail, the wall is fundamental
for a manually-woken LK core, and the win is the producer-offload using only proven directions.

### REFRAME (2026-08-28): SSPM is RUNNING at LK - the wall may be surmountable
Established via source + live: the kernel does NOT load SSPM firmware (no request_firmware; it only
interfaces with an already-running SSPM via is_sspm_ready / sspm_mbox_read/write). So SSPM is
PRELOADER-loaded and running BEFORE LK. Live SSPM mbox3 (0x10480000) has active data (0x1c=0xff,
0x24=2, 0x44=1, 0x50/54/58/5c populated), confirming SSPM alive. So coherent bringup is NOT
fundamentally impossible at LK - SSPM is present; it simply is not SERVICING SPMC CPU-power at LK (why a
plain PSCI CPU_ON hangs), which is why the DSU coherency P-Channel never fires and the CPC-arm bypass is
needed (and skips coherency).

THE ENABLE MECHANISM (mt8192 mt_mcdi.c mcdi_init_1, the concrete model): a coprocessor is switched into
CPU-power control by: wait TASK_STA==INIT, then mcdi_mbox_write(PWR_CTRL_EN, MCUSYS_CTRL|BUCK_CTRL|
ARMPLL_CTRL) then mcdi_mbox_write(AP_READY, 1). This is done by ATF (mcdi_try_init), and on a system that
never idles/hotplugs at LK it is never triggered - matching "SSPM does not service power at LK".

FIX HYPOTHESIS (the real win, needs a flash): from LK, perform the mt6785 SSPM CPU-power-control ENABLE
(the SSPM equivalent of PWR_CTRL_EN + AP_READY), THEN do a PLAIN PSCI CPU_ON (no CPC-arm bypass). If
SSPM then services the SPMC power request, it drives the DSU P-Channel -> the core is auto-coherent ->
frozen-read solved and the whole 2-core split opens. CAVEATS: mt6785 uses SSPM (older) not mt8192 MCUPM;
its MCDI mailbox (SSPM mbox3, slots CLUSTER_CAN_POWER_OFF/AVAIL_CPU_MASK...) is idle-only with NO
PWR_CTRL_EN slot, so the mt6785 power-control enable is a DIFFERENT SSPM IPI/command not in the kernel
MCDI driver (likely an ATF-side or SSPM-init step). mt6785 ATF source is not local (only mt8192) and the
SSPM firmware is a blob, so the exact mt6785 enable command needs either the mt6785 ATF/preloader source,
a WebSearch, or a careful live-system RE of the SSPM mbox0/1 IPI channels during a CPU hotplug.

NEXT: (1) live-diff the SSPM mailboxes (mbox0..3) across a cpu1 hotplug to catch the power-service
IPI/command; (2) find mt6785 ATF/preloader SSPM power-enable; (3) build an LK image that issues it +
plain PSCI and reads BC CANARY. Meanwhile the DVM long-shot (lk_a_snes_bigcore_dvm.img) and the
producer-offload fallback both remain staged/specced.

### SSPM MAILBOX HOTPLUG DIFF (2026-08-28): power-service is not a snapshottable command
Diffed all 4 SSPM mailboxes (mbox0..3 @0x10450000/60/70/80) across cpu1 online/offline/online. ONLY one
register changes: 0x1048001c (mbox3 slot 7 = MCDI_MBOX_AVAIL_CPU_MASK) 0xff<->0xfd (cpu1 bit). That is the
idle-coordination AVAIL_CPU_MASK; NOTHING coherency/bringup-related appears. So the SPMC power-on + DSU
P-Channel that a coherent bringup needs is serviced INSIDE the SSPM firmware / SPMC hardware, not via an
AP-writable mailbox command LK could replay. The coherent-bringup enable is therefore not reachable by
mailbox from LK; it needs the mt6785 ATF/preloader SSPM power-service init (source not local) or a blind
flash experiment.

### INVESTIGATION STATUS (honest): adb+source avenues for coherent bringup are exhausted
Everything reachable without the mt6785 ATF/preloader source or a flash has been tried. Definitive
conclusion: a manually-woken (CPC-arm) core at LK is powered (PWR_ON_ACK) but never joins DSU coherency
because the SSPM-serviced SPMC->DSU P-Channel handshake does not run at LK, and that handshake is internal
firmware/hardware with no AP/MMIO/mailbox lever LK can pull. Remaining ways to the REAL (coherent) win,
both out of the pure-adb reach: (A) obtain mt6785 ATF/preloader source, find the SSPM CPU-power-service
init, replay it from LK before a plain PSCI CPU_ON; (B) the staged DVM long-shot flash. The achievable win
WITHOUT coherent bringup is the producer-offload (worker reads static pre-bringup assets + writes tiles
out; both directions proven), a real anti-hitch perf gain needing no cpu0->worker coherence.

### BREAKTHROUGH: the mt6785 ATF (mtspmc.c) IS recoverable from tee.img - RE started
The mt6785-specific ATF (the source the earlier RE lacked) is the BL31 inside tee.img, which we have.
Regenerate the disasm anytime:
  aarch64-linux-gnu-objdump -D -b binary -m aarch64 --adjust-vma=0x4ce00000 \
    --start-address=0x4ce00400 /mnt/c/pairmini/tee_STOCK_rollback.img > /tmp/bl31_dis.txt
Confirmed strings: "plat/mediatek/mt6785/drivers/spmc/mtspmc.c", "spmc: power on core %d.%d [successfully]",
"mp0_spmc: 0x%x", "save_pwr_ctrl"/"load_pwr_ctrl", "[RGU-ATF] SSPM". Located functions:
- 0x4ce014ec: save/load_pwr_ctrl - reads CPC_FLOW_CTRL (0xc53a814), clears bit17 (0x20000), writes back,
  logs. (bit17, NOT SSPM_ALL_PWR_CTRL_EN bit13 - a distinct control bit worth understanding.)
- 0x4ce02084 / 0x4ce0210c: per-core helpers using mpidr->linear-id (bl 0x4ce0488c) with spinlocks.
- 0x4ce01590: SPM unlock+write pattern (writes 0x1000d000 with a 0x22000000 project-code key).
The actual spm_poweron_cpu (PWR_ON write to MP0_CPUn_PWR_CON 0x1000620c + PWR_ON_ACK bit31 poll, and any
SSPM handshake) is in this binary and is the RE target: determine whether it waits on an SSPM ack (hangs
at LK) or has an SSPM-service ENABLE we can replay from LK before a plain PSCI CPU_ON.

STATUS: this is the concrete path to the REAL coherent-bringup win. Next phase = finish the mtspmc.c RE
in the BL31 disasm (find the SSPM power-service enable / the ack-wait), then build an LK image that does
the enable + plain PSCI + reads BC CANARY. This is a focused RE sub-project; the DVM long-shot flash and
the producer-offload fallback remain staged/specced in the meantime.

### ATF RE RESULT (2026-08-28): power-on is PURE HARDWARE - SSPM-enable path REFUTED
Full RE of mt6785 BL31 spm_poweron_cpu (VA 0x4ce12adc, logs "spmc: power on core"): it is a pure
SPM/MCUCFG hardware handshake with ZERO SSPM references (confirmed: no movk of any 0x1040..0x1048 SSPM
mbox/SRAM base exists anywhere in BL31). Sequence: set CPC_FLOW_CTRL(0x0c53a814) bit13 (0x2000); set
PWR_ON bit2 on MP0_CPUn_PWR_CON (0x10006208+n*4); SPIN reading SPM 0x10006160 masked by 1<<(n<4?n+9:n+11)
until set; clear bit13. spmc_init (0x4ce129f4, once at boot): SPM unlock 0x0B160001, set RST_B bit0 on
per-cpu PWR_CON, clear bit5, set CPC_CTRL_ENABLE bit16. save/load_pwr_ctrl (0x4ce014ec, bit17 toggle) is
NOT called on the power-on path.
=> The coherent power-on needs NO SSPM servicing; it is fully AP-register-driven, and our LK PSCI CPU_ON
   already runs this exact ATF code and completes (PWR_ON_ACK set). So "enable SSPM CPU-power service at
   LK" is a DEAD path (there is nothing to enable). The earlier "plain PSCI hangs because SSPM never acks"
   was really the poll on 0x10006160 spinning because CPC_CTRL_ENABLE (bit16) was not armed; once armed
   (our CPC-arm), it completes. This is why the register state is identical to the live coherent core.

PARADOX + remaining lead: identical pure-hardware power-on, yet incoherent at LK. The ONE persistent
register difference found is CPC_FLOW_CTRL bit29 (0x20000000): live=0x200b0000 (bit29 SET, stable over 3
reads), LK=0xb0000 (bit29 CLEAR). ATF does not set it (the 0x20000000 writes in BL31 target 0x10001f9c,
not CPC_FLOW); it is likely set by the kernel MCDI CPC-config SMC or is a CPC status/mode bit. Cheap
direct test staged: set CPC_FLOW_CTRL bit29 at LK bring-up and re-run the canary.

### bit29 RE (2026-08-28): likely status/DCM, not a control bit - CPC full-diff is the real value
Ran the ATF CPC-config SMC handler (0x4ce0e4d4, jump-table): it toggles CPC_FLOW bit5 (auto-off), 0x0c53ab00
bit0, 0x0c53ab74=3, per config sub-cmd - NOT bit29. 0x4ce09ba4 clears CPC_FLOW bit20 (dormant read path).
Kernel: no direct 0x0c53a814/bit29 write anywhere; no CPC_FLOW bit-field named for bit29. Nearby concepts
are GIC_WAKEUP / GIC_SYNC_DCM. => CPC_FLOW bit29 is most likely a CPC hardware STATUS or DCM bit, not a
coherency control, so setting it at LK (the staged cpcbit29 image) is a LONG SHOT. Kept the experiment
anyway (cheap) but the REAL value of that flash is the BC CPCDUMP: live coherent has 14 non-zero CPC regs
(0x704,708,70c,710,714,718,740,748,808,814,840,844,848,898 - see tools/live_regpoke/live_cpc_reference.txt)
and we currently know the LK value for only 2 of them (CPC_SPMC_ST, CPC_FLOW). The dump exposes the other
12; any that differ coherent-vs-LK is a fresh, concrete candidate for the missing setup.

HONEST STANDING: power-on is proven pure-HW and identical to the live core; no MMIO snoop register exists;
bit29 is likely status. If the CPCDUMP diff shows no real control-register difference either, the
coherent-bringup wall is confirmed DSU-internal with no software lever, and the achievable deliverable is
the producer-offload (worker reads static pre-bringup assets + writes tiles out; both directions proven).

### PRODUCER-OFFLOAD VIABILITY PROBE folded into the diagnostic (2026-08-28)
Since the coherent-bringup path is looking like a hardware wall, added a test that de-risks the fallback
(producer-offload) in the SAME flash. cpu0 writes a STATIC value 0x57A70DED to 0x51000024 BEFORE PSCI
(pre-bringup) and cleans it; the worker reads it MMU-on WITHOUT invalidate (exactly how an offload worker
reads static, pre-cleaned assets). New log line "BC STATICPROBE: worker read of PRE-bringup static
0x51000024 = 0x..". Interpretation: 0x57A70DED => the worker CAN read pre-bringup static data even though
the per-frame canary stays frozen => the producer-offload (worker builds card-strip tiles from static
assets into fixed DRAM slots, cpu0 reads them via the proven worker->cpu0 direction) is VIABLE with zero
cross-core coherency. Garbage => even static pre-bringup reads fail and the offload is blocked too.
The lk_a_snes_bigcore_cpcbit29.img now yields THREE datapoints per flash: BC CANARY (bit29 coherent test),
BC CPCDUMP (full CPC coherent-vs-live diff), BC STATICPROBE (producer-offload viability).

### PRODUCER-OFFLOAD DESIGN (2026-08-28) - implementable, gated on STATICPROBE viability
Web search confirmed no documented software fix for the snoop-input asymmetry (writes-out work,
reads-in stale) - consistent with the DSU-internal wall. So the realistic deliverable is the
producer-offload, which needs ZERO cross-core coherency. Concrete design against the existing engine:

REUSE SURFACE (emu/snes/snes_menu.c):
- snes_menu_build_cardcache(m,t) @926: builds the SNES_L2_W(2496)px card strip, focus-CENTERED, panned
  via OVL src_x, rebuilt when cc_signature(m) @817 changes (scroll past the +/-608 margin) = the ~30ms
  hitch we want to offload.
- snes_menu_build_cardcache_band(m,t,r0,r1) @967: host-validated band split (row range) - reusable.

DESIGN:
1. Add snes_menu_build_cardcache_tile(m,t,tile_x): same draw path but ABSOLUTE row offset tile_x instead
   of focus-centered, so tiles are fixed windows of the infinite card row (row width = N_cards*pitch).
   Tiles at tile_x = 0, 2496, 4992, ... cover the whole row; each built ONCE.
2. cpu0 (before worker bringup) finalizes the static card list + boxart and CLEANS them to DRAM (the
   worker's inputs must be pre-bringup-cleaned - STATICPROBE confirms the worker can read them).
3. Worker builds every tile round-robin into fixed DRAM slots (0x54/0x55 window), writes done-flags.
   cpu0 reads the tile covering the current focus via the PROVEN worker->cpu0 direction (invalidate+read),
   composites it with OVL src_x. No dynamic cpu0->worker signal needed (worker builds ALL tiles; cpu0
   selects). Result: the scroll-rebuild hitch never runs on cpu0.
4. HOST-VALIDATE (no flash): tiles composited == focus-centered strip at every focus (diff, like the
   earlier band-split validation). Then device integration + the coherent worker->cpu0 tile handoff.
VALUE: eliminates the ~30ms card-strip rebuild stalls during scrolling (steady 60fps scroll). Modest vs
the original per-frame-coherent-split dream (which the DSU wall blocks), but real and coherency-free.
GATE: implement only after STATICPROBE (in the staged lk_a_snes_bigcore_cpcbit29.img) confirms the worker
reads pre-bringup static data (0x57A70DED). Both paths (coherent CPC-diff + offload viability) resolve on
that one flash.

### OFFLOAD FEASIBILITY, quantified (2026-08-28) - viable for moderate game counts
Corrected the memory assumption: the WB DRAM window is [0x4E000000,0x56000000) = 128MB (not 32MB).
Numbers: one card-strip tile = SNES_L2_W(2496) x SNES_L2_BAND_H(384) x 4 = 3.66MB. The strip is positioned
by m->sel_world (snes_menu.c:641 `cardx = 640 + sel_world + cont_shift`), and is pannable +/-608px (margin)
before rebuild, i.e. one tile covers ~2496px of card row and any ~1280px sub-window. Tiles stepped by
1280px of sel_world cover the whole row; tile count ~= row_px/1280 ~= ngames/5 (visible ~5 cards/1280px).
=> For a moderate list (say <=50 games) that is ~10 tiles ~= 37MB - fits the 128MB window (minus LK/pack/fb).
So PREBUILD-ALL-TILES is memory-feasible for moderate ngames, needing NO dynamic cpu0->worker focus signal
(worker builds every tile, cpu0 selects the focus tile). For very large lists it would exceed the window
and need focus-relative tiling (blocked) - acceptable limitation; can cap to the current system's games.

TILE-BUILDER approach: snes_menu_build_cardcache_tile(m,t,sel_world) = save m->sel_world; set it to the
tile center; snes_menu_build_cardcache(m,t) (already renders by sel_world); restore. OPEN QUESTION to
resolve before coding: does draw_carousel load/decode boxart focus-relative (lazy around the focused
card)? If so, a tile far from the live focus may miss boxart and the builder must force-load the tile's
games first. Check draw_carousel boxart handling. This is the only non-trivial part; the rest is a thin
wrapper + host-validation (tiles composited == focus-centered strip at each focus, like the vsplit check).

STATUS: offload confirmed memory-feasible + implementation approach identified; the tile-builder is the
first host-validatable component. Still gated on the staged STATICPROBE (worker reads pre-bringup static
0x57A70DED) before committing to the device path. Everything hinges on that one flash.

### OFFLOAD OBSTACLE (2026-08-28): the L2 card cache is FOCUS-SPECIFIC (skip_focus)
Studied draw_carousel (snes_menu.c:770): it is a RING carousel centered on m->focus (wx = sel_world +
CAR_HGAP*ring_delta(focus,j,n) + cont_shift), and the L2 cache SKIPS the focused card (skip_focus @787)
because that card's bright, blue-framed body is composited LIVE on the L3 overlay (draw_focus_card);
drawing it on both L2 and L3 double-contributes at the semi-transparent card background + AA edges
(OVL_LAYERS.md). Consequences for the offload:
- A prebuilt tile centered on focus K has a HOLE where card K is (skipped). It is only valid while the
  live focus == K (L3 fills the hole). Panning that tile to show focus K+/-1 exposes the missing card K.
  So tiles are NOT reusable across focuses by panning - the clean "prebuild tiles + OVL pan" idea breaks.
- Workable variants, both with real cost:
  (1) One tile PER focus (N tiles). Memory: N*3.66MB. OK only for small N (N<=10 ~= 37MB; N=50 ~= 183MB
      > 128MB window). Could cap to the current system's games (systems have few titles).
  (2) Re-architect the L2/L3 split so L2 holds ALL cards plain (focus-independent) and L3 draws only the
      focus DECORATIONS (blue frame + pulsing cursor), not the card body. Then one focus-independent tile
      set is reusable/pannable. But this is a real render refactor with visual-correctness risk (the exact
      double-contribute the current split was built to avoid) on a menu that currently works well.
HONEST REASSESSMENT: the offload is NOT the easy consolation it looked. The real win (per-frame coherent
split) is a hardware wall; the offload needs either small-N-only prebuild or an L2/L3 refactor. Given the
modest upside (removing ~30ms scroll-rebuild hitches) vs the refactor risk, the offload should be a
DELIBERATE user decision, not an autonomous build. Recommend: get the STATICPROBE datapoint first, and if
the user wants the offload, do variant (1) scoped to per-system games (low risk, bounded memory) rather
than the (2) refactor. Until then, do not destabilize the working single-core menu.

### ATF DSU SYSREG RE (2026-08-28): all power/clock, no missed coherency step - RE CLOSED
Examined every DSU/cluster sysreg + coherency-fabric write in the mt6785 BL31:
- s3_0_c15_c2_7 (0x4ce09b00): `mrs; orr #1; msr; isb; dsb; wfi; b .-4` = a CORE POWER-DOWN stub (set the
  powerdown bit then WFI-loop), the pwr_domain_off leaf. Not coherency.
- 0x0c533308 / 0x0c533b08 (0x4ce09b1c, called from boot setup 0x4ce0971c): mcusys DCM/clock config bits,
  set once at boot (cluster-wide). "cci_adb400_dcm_config" = CCI ADB400 async-bridge DYNAMIC CLOCK mgmt.
  All clock/DCM, not snoop/coherency.
- s3_0_c15_c1_4 (CPUECTLR) / c1_1 (0x4ce1a940): the per-core Cortex-A55 errata/reset function (runs per
  core via reset, so cpu1 gets it too). Not a coherency-enable (matches ARM: A55 has no software SMPEN).
=> No missed cluster-coherency init anywhere in the mt6785 ATF. Combined with: power-on is pure-HW and
completes (PWR_ON_ACK), no MMIO snoop register (SPM/CPC/mcucci/mcucfg swept), coherency is automatic via
the DSU P-Channel (ARM TRM), and the CPC-arm bypass fires the power rail but not that P-Channel handshake.
The coherent-bringup RE is now EXHAUSTIVELY CLOSED: there is no AP/MMIO/sysreg/mailbox lever at LK. The
only untried items are hardware experiments needing a flash (the staged cpcbit29+DVM images).

FINAL STATE: coherent path = hardware wall, one long-shot flash staged. Offload path = viable but needs a
user risk/value decision (focus-specific L2 cache; per-system per-focus tiles = low-risk variant). Both
resolve on the one staged diagnostic flash (BC CANARY/CPCDUMP + BC STATICPROBE). Autonomous non-flash
investigation is complete; do not destabilize the working menu without the user's go-ahead.

### FROZEN-VALUE TRACED (2026-08-28): 0xa86dbdec is boot_b staging data in the SCRATCH window
The worker's fixed frozen read 0xa86dbdec exists in snes_boot_b.img (LE ecbd6da8 at file offset 0x40000c,
exactly once). boot_b loads at SNES_BLOB_PA=0x50000000 inside the k85v1_64 download/SCRATCH window
[0x4E000000,0x56000000), which also holds the canary at 0x51000000. boot_b is staged/decompressed THROUGH
that window during load, so 0x51000000 holds leftover boot_b bytes at the moment the worker's view freezes
(MMU-enable). => The worker is reading a FROZEN SNAPSHOT of DRAM as it was around boot_b-load time; cpu0's
later Device/WB writes to 0x51000000 never update that frozen view. This is direct confirmation of the
frozen-snapshot / no-snoop-input model (not a new coherent-path lever - the wall stands).

IMPLICATION FOR THE OFFLOAD (positive): if the worker's view is a snapshot frozen at MMU-enable, then any
data cpu0 writes+cleans BEFORE the worker enables its MMU is captured in that snapshot and IS readable by
the worker. The STATICPROBE writes 0x57A70DED BEFORE PSCI (pre-bringup), so it should be in the frozen
snapshot => STATICPROBE is predicted POSITIVE and the producer-offload (worker consumes static
pre-bringup-cleaned assets) is predicted VIABLE. The staged flash will confirm.

### SHARED-L3-EVICT hypothesis + probe (2026-08-28)
Model that fits worker->cpu0 working + the frozen boot_b-era reads: the worker SHARES the DSU L3 (its
reads hit L3, its writes allocate L3 which cpu0 snoops - so worker->cpu0 works) but receives NO snoop
INVALIDATIONS. It cached the canary's boot_b-era line at first read; cpu0's per-frame clean-only never
evicts it, so the worker reads the stale L3 copy forever. Test: cpu0 now does clean+INVALIDATE (BC_INVAL,
DC IVAC to PoC) on the canary each frame - DC to PoC reaches the shared L3, evicting the stale line so the
worker re-fills from DRAM. CAVEAT (why low-probability): the Device-mode canary bypasses ALL caches yet
still reads stale (a memory-path symptom, not cache), and the worker's PRIVATE L1/L2 copy (WB mode) is
unreachable by cpu0 maintenance without a snoop broadcast the worker ignores. So this only helps if the
stale copy is specifically in the SHARED L3 and the worker mis-hits it. Cheap + harmless, folded into the
lk_a_snes_bigcore_cpcbit29.img canary path. If BC CANARY now reads 0xCA5Axxxx, shared-L3-stale was it.

### LIVE SPM BLOCK BASELINE + comparison (2026-08-28): no coherency-relevant difference
Captured the full live coherent SPM block (0x10006000..0x100063fc, 166 non-zero regs;
tools/live_regpoke/live_spm_reference.txt). Most is runtime state (PCM data 0x76543210 patterns at
0x6030+, masks, wakeup/timer state) that naturally differs live-vs-LK regardless of coherency. The
coherency/power-relevant subset compared to known LK values:
- MP0_CPUn_PWR_CON (0x208..0x224): live shows 0x00000005 for RUNNING cores (PWR_ON+RST_B, bit31 ACK
  CLEAR) and 0x80000005 transiently; LK cpu1 = 0x80000005 (ACK set STABLE). The bit31 ACK is TRANSIENT
  runtime state (set at power-on, toggled by MCDI idle) - LK just has no MCDI cycling the core, so it
  stays set. NOT a coherency indicator (matches the earlier hotplug-diff finding).
- SPM_CPU_PWR_STATUS (0x160): live 0x24146 vs LK 0x474c - differs only in which cores are on/idle
  (runtime power state), not coherency.
=> The SPM block, like CPC/mcucci/mcucfg, has NO stable coherency-relevant difference between the live
coherent core and our LK core. This is further confirmation (now across ALL four MMIO power/CPC/mcusys
blocks) that the incoherence is not any software-visible register - it is the DSU-internal snoop-input
(P-Channel) the CPC-arm bypass never fires. The coherent-path MMIO investigation is complete and negative.

### KERNEL SMP BRINGUP RE (2026-08-28): standard PSCI, no coherency hook - FINAL CLOSURE
Checked the last unexamined software layer: the kernel SMP secondary-bringup path (arch/arm64/kernel/smp.c
smp_prepare_cpus / cpu_up) is STANDARD PSCI with NO DSU/CCI/L3/coherency hook. The only MTK-specific
mcusys code (base/power/dcm) is DCM = dynamic CLOCK management, not coherency. So the kernel does nothing
special for secondary-core coherency that LK omits - it relies on the SAME automatic hardware coherency
our LK PSCI path already invokes (same ATF pure-HW power-on). Preloader is before BOTH LK and kernel, so
it cannot be the LK-vs-kernel difference either.

=> EVERY software layer is now examined and negative: ATF power-on (pure HW, replicated), kernel SMP
bringup (standard PSCI, no hook), MCDI/SSPM firmware (idle-only), all four MMIO blocks (SPM/CPC/mcucci/
mcucfg, no coherency register), no CPUECTLR coherency bit, and the frozen read traces to a boot_b DRAM
snapshot. There is NO software lever at ANY level (bootloader, ATF, kernel, firmware, MMIO, sysreg) that
LK is missing. The CPC-arm-woken core's incoherence is a DSU-internal hardware behavior: the same PSCI
power-on that yields a coherent core at kernel time yields an incoherent one at LK, with byte-identical
software state, so the difference is DSU-internal (snoop-input P-Channel) and NOT software-addressable at
LK. The coherent-bringup investigation is DEFINITIVELY and EXHAUSTIVELY closed. Only the staged hardware
experiments (cpcbit29 4-probe + DVM) can add anything, and only by luck. The realistic deliverable is the
producer-offload (predicted viable) pending the user's flash + risk/value decision.

### BRINGUP-PATH VERIFICATION (2026-08-28): worker uses full ATF path, refutes reset-skip
Checked whether the worker might skip ATF's EL3 reset (which would make the software state NOT identical
to a kernel-context core). REFUTED by the code: bc_manual_poweron is dead (`(void)&bc_manual_poweron`),
so LK does NOT set MCUCFG_BOOTADDR/INITARCH; the bringup is PURE PSCI CPU_ON(0x100, entry=bc_entry_arm2).
Additionally MCUCFG (0x0C53xxxx) is NS WRITE-LOCKED (boot-addr/initarch writes silently drop), so LK
CANNOT redirect the core's boot to bypass ATF. Therefore cpu1 boots at ATF's RVBAR -> runs ATF's full EL3
warm-boot + per-core reset handler (Cortex-A55 CPUECTLR/errata setup) -> ATF PSCI-finish ERETs to our
AArch32 NS EL1 entry - EXACTLY the path a kernel-context secondary takes. So the worker's core config
(CPUECTLR etc.) is ATF-set, identical to a coherent kernel core. The bringup PATH is byte-identical too.

This removes the last "maybe the software differs" doubt: same PSCI, same ATF EL3 reset, same core config,
same MMIO state - yet coherent at kernel, incoherent at LK. The difference is conclusively DSU-internal
system-state (the snoop-input P-Channel), the ONLY remaining variable being the runtime CONTEXT (at kernel
time the system has been running with SSPM/MCDI active and cores cycled through the full power FSM; at LK
cpu1 is a first-ever cold join). Not reproducible or fixable by any software means at LK. Investigation
remains definitively closed; deliverable is the producer-offload pending the user's flash + decision.

### WARM-CYCLE EXPERIMENT: designed, build-ready, DEFERRED (2026-08-28)
Last untested coherent-path mechanism: does a warm PSCI CPU_OFF->CPU_ON cycle of the worker establish
coherency the cold first-join did not (runtime-context hypothesis)? Design (build-ready):
- Worker (bc_worker_entry, per-frame after the canary read ~line 119): if job.cmd==WARM_OFF, read the
  canary into w_can_cold, then issue PSCI CPU_OFF via SMC (AArch32: r0=0x84000002, smc #0) instead of
  rendering.
- cpu0 (bigcore_start, self-contained, driving the go/done handshake manually): bringup1 (PSCI CPU_ON) ->
  post go cmd=READ (worker reads canary1->w_can_cold) -> post go cmd=WARM_OFF (worker self-powers-off) ->
  poll SPM 0x160 for cpu1 bit clear -> write canary2 -> bringup2 (PSCI CPU_ON, WARM) -> post go cmd=READ
  (worker reads canary2->w_can_warm) -> log "BC WARMCYCLE: cold=0x.. warm=0x..". warm==0xCA5A while
  cold!=0xCA5A => the warm cycle fired coherency (the win).
- ~50 lines across bigcore_comms.h + snes_driver.c (worker cmd path + SMC) + bigcore.c (double bringup).
CONFIDENCE: LOW (~10-15%) - the DSU coherency P-Channel is specified to fire on a cold power-up too, so a
warm cycle is unlikely to differ; our first bringup is already full-ATF PSCI.
DECISION: DEFERRED, not built. Rationale: 6 diagnostic images + the 4-probe cpcbit29 + DVM are already
staged and NONE has been flashed yet. Staging a 7th speculative experiment before any datapoint exists is
premature - the bottleneck is the flash, and the CPCDUMP/STATICPROBE results will redirect what is worth
building. Build the warm-cycle only if the staged diagnostic comes back fully negative AND the user wants
the last coherent-path shot taken. Until a flash happens, further experiment-staging is not productive.

### WARM-CYCLE EXPERIMENT BUILT (2026-08-28): lk_a_snes_bigcore_warmcycle.img staged
Built the last untested coherent-path mechanism (deferred last cycle, now built since the user is away
and relentlessly wants the coherent win). Clean implementation avoiding the frozen-view trap:
- bc_entry_arm2 (MMU OFF, before caches enable): reads comms.warm_cycle UNCACHED (gets cpu0's fresh
  value, not the frozen snapshot); if nonzero, issues PSCI CPU_OFF (SMC 0x84000002) from a clean
  caches-off state.
- bigcore_start (cpu0): sets warm_cycle=1 before bringup1; after PSCI CPU_ON returns, polls SPM 0x160
  until cpu1's rail ack bit clears (worker powered off), logs "BC WARMCYCLE: cpu1 powered off after N us",
  clears warm_cycle + resets magic, then PSCI CPU_ON again (bringup2 = WARM DSU re-join). The existing
  per-frame BC CANARY then reports whether the warm-cycled worker is coherent.
Build flag AYANEO_BC_WARMCYCLE (project/k85v1_64.mk). Interpretation on flash: if BC CANARY worker-read
flips to 0xCA5Axxxx (was 0xa86dbdec on the cold-join builds), the warm PSCI down->up cycle established the
DSU coherency the cold CPC-arm join did not -> the coherent win, and the 2-core split unlocks. Confidence
low (~10-15%; the DSU P-Channel should fire on cold-up too) but it is the last distinct coherent-path
mechanism. If BC WARMCYCLE shows cpu1 did NOT power off (timeout), PSCI CPU_OFF does not work at LK and
the experiment is inconclusive. Two independent flashes now cover the coherent path: cpcbit29 (4 probes)
and warmcycle; plus dvm.

### NOVEL LEAD (2026-08-28): the GIC/SGI channel bypasses the DSU-snoop wall
Fresh idea after exhausting the DRAM/coherency angles: the worker's DSU snoop-input is dead, but the GIC
is a SEPARATE interconnect path. cpu0 can send Software Generated Interrupts (SGIs) to cpu1 via the GIC,
which do NOT traverse the frozen DSU-snoop/DRAM path. This is a working LOW-BANDWIDTH cpu0->worker channel.
Platform: GICv3 (DT arm,gic-v3; GICD @0x0c000000 size 0x40000, GICR @0x0c040000 size 0x200000). The worker
stub currently ignores IRQs (bc_vectors IRQ = `b .`), so SGI receive is not set up yet.

WHY THIS MATTERS - it can make the producer-offload CLEAN (unblocks its only real problem):
The offload's blocker was that the worker needs the DYNAMIC focus (a cpu0->worker signal) to know which
card strip to build, and that direction is dead - forcing the ugly "build ALL tiles" workaround (memory
+ skip_focus mess). With SGIs, cpu0 signals focus CHANGES (SGI#1=focus++, SGI#2=focus--, or an SGI whose
4-bit ID encodes a small delta) and the worker maintains its own focus counter. Then the worker has
EVERYTHING: static assets (frozen-snapshot readable, STATICPROBE-predicted), the live focus (via SGI), and
worker->cpu0 tile output (proven). It builds the CURRENT focus-centered strip AHEAD of cpu0's need - one
strip, minimal memory, using the EXISTING build_cardcache unchanged (correct skip_focus for that focus).
cpu0 just consumes the pre-built strip when it would have rebuilt -> the ~30ms scroll hitch is offloaded.

FEASIBILITY (needs a flash to verify SGIs reach a CPC-arm-woken core, but architecturally sound):
- Worker (AArch32) GICv3 CPU-interface bring-up: wake its GICR (GICR_WAKER clear ProcessorSleep, poll
  ChildrenAsleep=0), enable SGI IDs (GICR_ISENABLER0), set ICC_SRE (sysreg enable), ICC_PMR=0xff,
  ICC_IGRPEN1=1 (all via CP15 in A32). Then POLL ICC_HPPIR1/ICC_IAR1 each frame for pending SGIs (no IRQ
  handler needed - polling avoids the vector-table path).
- cpu0 send: ICC_SGI1R (A32 MCRR p15,0,<lo>,<hi>,c12) targeting cpu1 affinity (MPIDR 0x81000100 ->
  aff1=1,aff0=0, target-list bit0). 
- KEY UNKNOWN: does cpu1's GICR (redistributor) power up with the CPC-arm bringup so it receives SGIs? The
  GIC is a separate block from the DSU; the GICR is likely in the core's power domain and up once powered,
  but the CPC-arm path might leave it asleep. MINIMAL TEST needed.

NEXT: build a minimal SGI-channel viability experiment - worker wakes its GICR + CPU interface and polls
for SGIs; cpu0 sends an SGI each frame; worker reports a received-SGI count into comms (via the WORKING
worker->cpu0 direction). If the count increments, the GIC channel is LIVE and the clean offload is
unlocked. This is a genuinely new, non-coherency path to a real 2-core win and is worth a flash.

### GIC/SGI EXPERIMENT - mapped + simplified to MMIO-poll (2026-08-28), build-ready
Live-mapped the GICv3 redistributors (module reads of GICR TYPER/WAKER, stride 0x20000):
  frame n @ 0x0c040000 + n*0x20000 = cpu_n. TYPER.hi = Affinity. cpu1 (Aff1=1,Aff0=0, TYPER.hi=0x100) is
  frame1 @ 0x0c060000 (RD_base); its SGI frame is +0x10000 = 0x0c070000. frame7 has TYPER.Last set. All
  WAKER=0 (awake) on live.
KEY SIMPLIFICATION: an SGI sets a PENDING bit in the target redistributor readable via MMIO
GICR_ISPENDR0 (SGI frame + 0x0200). So the worker does NOT need the ICC_* CPU interface or an IRQ handler
- it just POLLS MMIO. Minimal viability experiment:
- cpu0 (per frame): send SGI#1 to cpu1 via ICC_SGI1R (A32 mcrr p15,0,<lo=0x01010001>,<hi=0>,c12):
  INTID=1[27:24], Aff1=1[23:16], TargetList=0x1[0] -> 0x01010001. (cpu0/LK is the boot core; ATF enabled
  ICC_SRE for it, so the sysreg send should work.)
- worker (per frame, MMIO only): read GICR_ISPENDR0 @ 0x0c070000+0x0200; if SGI#1 pending bit set,
  increment a comms counter w_sgi_count and clear it via GICR_ICPENDR0 (0x0c070000+0x0280). Report
  w_sgi_count via the WORKING worker->cpu0 direction. (May first need to wake cpu1 GICR: clear WAKER
  ProcessorSleep bit1 @ 0x0c060000+0x14, poll ChildrenAsleep bit2 clear; and enable SGI#1 in
  GICR_ISENABLER0 @ 0x0c070000+0x0100.)
If w_sgi_count increments on HW, the GIC channel is LIVE despite the dead DSU snoop -> cpu0 can feed the
worker the dynamic focus (focus-delta SGIs) -> CLEAN producer-offload (worker builds the current
focus-centered strip ahead via build_cardcache unchanged; one strip, minimal memory, no skip_focus mess).
This is the first realistic non-coherency route to a real 2-core win. NEXT: build this experiment image.

### GIC/SGI CHANNEL EXPERIMENT BUILT (2026-08-28): lk_a_snes_bigcore_sgi.img staged
Built the MMIO-channel viability test (safer than sysreg SGI - no ICC_SRE trap risk):
- cpu0 (bc_dispatch, per frame): MMIO write GICR_ISPENDR0 (cpu1 SGI frame 0x0c070000+0x200) = 0x2, setting
  SGI#1 pending in cpu1's redistributor.
- worker (bc_worker_entry, per frame): MMIO read 0x0c070200; if bit1 set, increment sgi_seen, clear via
  GICR_ICPENDR0 (0x0c070280), publish sgi_seen to comms w_sgi_count (worker->cpu0, the WORKING direction).
- cpu0 logs "BC SGIPROBE: worker saw N GIC SGI-pending signals via MMIO".
Gated AYANEO_BC_SGI. Interpretation: if N INCREMENTS across frames, the worker's MMIO reads observe cpu0's
MMIO writes -> MMIO (the GIC peripheral path) is a LIVE cpu0->worker channel despite the dead DRAM/DSU
snoop -> the clean producer-offload is unlocked (cpu0 feeds the worker the dynamic focus via SGIs/MMIO,
worker builds the current focus-centered strip ahead using build_cardcache unchanged - one strip, minimal
memory, no skip_focus mess). If N stays 0, either the worker's MMIO reads are also frozen (whole read path
dead, not just DRAM) or cpu0's NS write to cpu1's GICR is blocked - both close this path. This is the
highest-value untested experiment: a positive result is a real 2-core win with NO coherency needed.
Flash lk_a_snes_bigcore_sgi.img + tee_patched_armcpc.img; read the BC SGIPROBE line.

### GIC/SGI experiment STRENGTHENED (2026-08-28): + generic SPM-MMIO channel to disambiguate
Added a second, generic MMIO channel to lk_a_snes_bigcore_sgi.img so a null result is not ambiguous:
- cpu0 also writes a rolling 0x5A5A|seq to SPM CPU_SPARE_CON (0x10006250, confirmed unused=0 on live, safe
  scratch, and SPM is definitely LK-mapped since bigcore writes it); the worker reads it MMU-on via MMIO
  into w_spm_scratch. New log "BC MMIOPROBE: worker read SPM scratch 0x10006250 = 0x..".
DECISION TABLE on flash:
- BC MMIOPROBE tracks 0x5A5Axxxx (any channel) => MMIO is a LIVE cpu0->worker path despite the dead DRAM
  snoop => clean producer-offload unlocked (feed the worker the focus via an MMIO word). REAL 2-core win.
- BC SGIPROBE increments too => the GIC path specifically works (nicer: real interrupt semantics).
- Both frozen/zero => the worker's ENTIRE read path is frozen (not just DRAM) - closes the MMIO idea; the
  only channel left would be the worker reading a peripheral it can independently poll (e.g. the gamepad).
So one flash of lk_a_snes_bigcore_sgi.img now cleanly answers "is there ANY cpu0->worker channel".

### KEY FRAMING (2026-08-28): a live MMIO channel unlocks the ORIGINAL per-frame split, not just offload
The dynamic menu state that the render depends on is SMALL. cc_signature (snes_menu.c:817) enumerates the
strip's dynamic inputs: focus, state, sort_rule, ngames, aspect, sel_world, resume_dim, open_y (8 fields);
the full snes_menu_render adds cont_shift, xfade_t, prev_focus, m->clock (cursor pulse) and a few slide
timers (cur_slide_t, screen_oy) - roughly ~20 words total. The rest of the ~4KB menu struct is STATIC
(loaded once at init, in the worker's frozen snapshot -> readable). So if the SGI/MMIO experiment shows
MMIO is a live cpu0->worker channel, cpu0 can PUBLISH those ~20 dynamic words each frame via MMIO (through
SPM scratch regs with a tiny streaming protocol, or a wider MMIO scratch window), and BOTH cores render
their band via snes_menu_render (the split is host-validated pixel-exact - host_render.c rsplit). That is
the ORIGINAL "60fps at lower power" per-frame render split - achievable with NO cache coherency, purely on
the MMIO peripheral path + the proven worker->cpu0 framebuffer writes.
So the SGI/MMIO flash (lk_a_snes_bigcore_sgi.img) is the single most important test in the whole effort:
- BC MMIOPROBE tracks 0x5A5Axxxx => MMIO channel live => the per-frame render split (the real goal) is
  reachable: cpu0 publishes ~20 dynamic words/frame, worker renders its band. Wire it.
- both channels frozen => the worker's whole read path is dead, and the only remaining model is a fully
  INDEPENDENT worker (reads the gamepad MMIO itself, runs its own menu state machine in lockstep) - which
  ALSO needs MMIO reads to work, so a frozen MMIOPROBE likely closes even that.
This is why MMIO viability is the crux: it is the difference between the full original win and a hard wall.

### MMIO STATE-PUBLISH SPEC (2026-08-28), refined: home-carousel split needs ~15-20 words
Enumerated snes_menu_update's per-frame writes: ~48 fields across ALL states (home, menubar, submenu,
resume, dialogs, options, language, sort). That is more than a single small MMIO scratch window - BUT the
perf-critical target is the HOME carousel (state==0), the only SUSTAINED 60fps case (idle scroll). Its
render depends on ~15-20 dynamic fields: focus, prev_focus, sel_world, cont_shift, xfade_t, clock,
cur_slide_t, car_x, car_navd, scroll, scr_speed, scr_dir, cur_scroll_time, cur_scroll_spd, disp_cur,
disp_sel, state, sort_rule, ngames, aspect. The rest of the ~4KB menu struct is static (in the worker's
frozen snapshot). Submenu/resume/dialog states are transient (not sustained load) -> the split can be
GATED to state==0 and fall back to single-core elsewhere (no visual/perf cost).
PUBLISH PLAN (on a positive BC MMIOPROBE): cpu0, after snes_menu_update, writes those ~20 words to an MMIO
scratch window each frame (SPM has SPM_SW_FLAG_0/1 + SPM_SW_RSV_0..5 + CPU_SPARE_CON ~= 9 words; extend
with more SPM SW_RSV or a small SSPM-SRAM/mcusys scratch to reach ~20, or stream via an index+value
handshake through 2 regs). The worker patches those fields into its frozen-snapshot copy of m, then renders
its band via snes_menu_render (host-validated exact). Framebuffer band goes out via the proven
worker->cpu0 direction. That is the full home-carousel per-frame split with NO coherency. Ready to wire the
moment MMIO viability is confirmed by lk_a_snes_bigcore_sgi.img.

### STATE-PACK VALIDATED (2026-08-28): the 23-field pack fully determines the home render
Built snes_menu_pack_state/unpack_state (X-macro, SNES_STATE_NWORDS=23: 9 int + 14 float home-carousel
dynamic fields) and a host_render "statepack" completeness test: render S1 -> A, pack; navigate to S2
(perturb); unpack S1 back; re-render -> B; A==B proves the pack captured every render-relevant dynamic
field. RESULT: PASS - all 6 home-state (state==0) checks show 0 px difference (23 words/frame fully
determine the render). The non-home states (menubar=1, resume=3) FAIL as expected and are ignored - the
split targets ONLY the sustained-60fps home carousel; other states fall back to single-core.
=> The MMIO 2-core split is now CORRECTNESS-COMPLETE on the host side: the band split is pixel-exact
(rsplit) AND the 23-field state channel is complete (statepack). The ONLY remaining unknown is the MMIO
channel viability (lk_a_snes_bigcore_sgi.img BC MMIOPROBE). On a positive MMIOPROBE the split is ready to
wire with confidence: cpu0 packs 23 words + publishes over MMIO each home frame; the worker unpacks into
its snapshot m and renders its band; framebuffer out via worker->cpu0. All host-validated; no coherency.

### MMIO STATE CHANNEL LOCATED (2026-08-28): SPM SW_RSV block @ 0x10006600 (the last prerequisite)
The 23-word state channel needs contiguous safe MMIO scratch. Found it: the SPM SW-reserved block -
SPM_SW_FLAG_0/1 + SPM_SW_RSV_0..19 span 0x10006600..0x1000663C (16 defined words), and a live scan of the
whole 0x10006600..0x100066FF range (64 words) reads ALL ZERO = unused/safe. So 23 words at 0x10006600 is
plenty. These are software-scratch (suspend/resume comm with the PCM firmware, idle at LK where no suspend
runs), safe to write pre-kernel. It is the SAME SPM MMIO path the MMIOPROBE (0x10006250) already tests, so
a positive BC MMIOPROBE validates this channel too.
=> The MMIO 2-core home-carousel split is now FULLY UNBLOCKED and specified end-to-end:
   1. CHANNEL: 0x10006600, 23 u32 words (SNES_STATE_NWORDS). [located, safe]
   2. STATE PACK: snes_menu_pack_state -> the 23 words; completeness host-validated (statepack PASS). [done]
   3. BAND SPLIT: snes_menu_render top/bottom bands, pixel-exact host-validated (rsplit PASS). [done]
   4. cpu0 (home frames): pack 23 words to 0x10006600; render band [0,mid); signal worker.
   5. worker: memcpy cpu0's frozen-snapshot menu -> local m (static fields correct), unpack the 23 MMIO
      words over it, render band [mid,H) into the framebuffer (worker->cpu0 DRAM write - proven direction).
   6. non-home states: single-core fallback.
Everything is host-validated or located; the ONLY remaining unknown is MMIO read viability (BC MMIOPROBE).
On a positive result the wiring is mechanical and low-risk (all pieces proven). This is a real, complete,
coherency-free path to the original 60fps-lower-power 2-core win.

### FULL STATE CHANNEL TEST BUILT (2026-08-28): 23-word transfer validated in-flash
Extended lk_a_snes_bigcore_sgi.img from a 1-word MMIO probe to the FULL split state channel: cpu0 packs
the real menu dynamic state (snes_menu_pack_state, 23 words) and publishes it to the SPM SW_RSV MMIO
window 0x10006600..0x1000665C each frame, summing it (g_bc_state_sum). The worker reads all 23 words from
0x10006600 via MMIO, sums them, and publishes w_state_sum (worker->cpu0). New log:
  "BC STATECHAN: cpu0 packed sum=0x.. ; worker read-back sum=0x.. => MATCH/mismatch"
A MATCH proves the COMPLETE split state channel works over MMIO (the worker receives the whole per-frame
dynamic state cpu0 publishes), which is the last thing to confirm before wiring the actual band render.
So one flash of lk_a_snes_bigcore_sgi.img now yields the full progression:
  BC MMIOPROBE (1-word MMIO live?) -> BC STATECHAN (full 23-word state channel live?) -> [then wire the
  band render: worker memcpy snapshot m + unpack MMIO state + render band -> the 2-core split].
Remaining after a positive STATECHAN: only the band-render wiring (device flash-iteration; all its pieces
- pack complete, band split exact, channel located+validated - are already proven). The coherency-free
per-frame 2-core win is one positive datapoint away from mechanical wiring.

### HW SGI/MMIO/STATE RESULT (2026-08-28): STATICPROBE POSITIVE; MMIO reads frozen
Flashed lk_a_snes_bigcore_sgi.img. Results:
  BC STATICPROBE: 0x57a70ded  => POSITIVE. The worker CAN read PRE-bringup static data (the frozen
     snapshot includes data cpu0 wrote+cleaned before the worker enabled its MMU). PRODUCER-OFFLOAD VIABLE.
  BC SGIPROBE: 0              => GIC SGI channel dead (worker saw no pending signals).
  BC MMIOPROBE: 0x0           => worker read 0 (the bringup value of unused SPM scratch), NOT cpu0's
     per-frame 0x5A5A write.
  BC STATECHAN: cpu0 sum=0x3ff229c6, worker sum=0 => full 23-word channel dead (worker read all zeros).
=> The worker's FROZEN SNAPSHOT covers MMIO peripherals too, not just DRAM: it reads the physical address
   space as of MMU-enable and never sees cpu0's later writes to ANY address (DRAM or MMIO). This kills the
   per-frame MMIO split and every dynamic cpu0->worker channel. CONFIRMED positive: static pre-bringup
   data is readable (STATICPROBE) - so the ONLY viable 2-core model is STATIC-INPUT producer-offload.
ONE ambiguity to close first (staged): did cpu0's MMIO WRITES even land, or drop? Rebuilt sgi image adds
BC WRITEBACK (cpu0 reads back its own 0x10006250 / 0x10006600 writes). Nonzero => writes land, worker read
is frozen (MMIO dead, conclusion stands). Zero => cpu0's SPM-scratch write dropped and MMIO may still be
alive via a different cpu0-writable scratch (would REOPEN the per-frame split). Flash the rebuilt
lk_a_snes_bigcore_sgi.img and read BC WRITEBACK + BC MMIOPROBE.

### SPLIT DATA-MODEL VERIFIED (2026-08-28): only the 23 dynamic words need the channel
Confirmed the per-frame split's data dependencies are all satisfiable from (frozen snapshot + 23 MMIO
words), no hidden dynamic dependency:
- INIT ORDER: bigcore_start (worker bringup, snes_driver.c:802) runs AFTER snes_pack_open (725) and
  snes_menu_init (789). So the worker's frozen MMU snapshot captures a FULLY-INITIALIZED menu struct and a
  fully-built home scene (m->home at SNES_HOME_PA=0x50C00000). Static pointers/scene are valid in the
  snapshot.
- HOME SCENE IS STATIC: snes_menu_render state==0 (line 205-207) does snes_render_scene(&m->home) then
  draw_carousel. snes_menu_update does NOT rebuild the home scene (only submenu/dialog scenes at line 186).
  So m->home is built once at init and never rebuilt - the worker's frozen copy is correct. The
  per-frame animation (wallpaper scroll, card positions, cursor) is computed at RENDER time from m fields,
  all of which are in the 23-word pack (statepack PASS confirmed completeness).
=> The worker needs ONLY: (a) its frozen snapshot (valid static menu + scene, since bringup is post-init),
   and (b) the 23 dynamic words per frame. Both are available if the MMIO channel is live. The scene-pool
   clean in bc_dispatch (line 296) is defensive/legacy - the home scene does not actually change per frame.
So the ONLY remaining gate for the full per-frame split is MMIO channel viability (pending the SPM-unlock
flash: BC WRITEBACK/MMIOPROBE/STATECHAN). If that goes green, the split wiring is fully de-risked end to
end - every data dependency verified satisfiable.

### CHANNEL FINDING (2026-08-28): SPM SW_RSV is PCM-firmware-owned - bad channel
Source: SPM_SW_RSV_0..8 are the SPM PCM firmware's wakeup-status registers (mt6785 mtk_spm_internal.c:
wakesta->r12 = spm_read(SPM_SW_RSV_0); wake_misc = SW_RSV_5; SW_RSV_6=timer_out; 7/8=b_sw_flag). The
running PCM firmware CONTINUOUSLY writes them, so cpu0's writes to 0x10006600 are overwritten - the unlock
cannot fix that. => SW_RSV (0x10006600) is NOT a usable cpu0->worker channel; the STATECHAN word will stay
0 regardless. Viable channel candidates instead:
- CPU_SPARE_CON (0x10006250): not referenced by any driver = likely free SW scratch; the SPM unlock may
  make cpu0's write land (MMIOPROBE tests it).
- GICR_ISPENDR0 (0x0c070200): GIC redistributor, NS-writable (kernel uses the GIC), NOT SPM-firmware-owned
  - the cleanest peripheral-path channel (SGIPROBE + the new GICR write-back test it).
So the pending SPM-unlock flash is still the right viability test via CPU_SPARE_CON + the GICR; SW_RSV was
a poor channel pick (now understood). Once viability is confirmed on a NON-firmware register, the 23-word
split channel will use that register family (e.g. stream through CPU_SPARE_CON with a seq handshake, or a
GICR-based scheme) rather than SW_RSV.

### DEFINITIVE (2026-08-28): worker's ENTIRE read path is frozen - per-frame split DEAD, offload VIABLE
SPM-unlock flash, decisive via the GICR write-back:
  BC WRITEBACK: SPM 0x10006250=0x0 (cpu0 write DROPPED even post-unlock), SW_RSV 0x10006600=0x0 (PCM-owned),
    GICR 0x0c070200=0x813f0002 (cpu0's write LANDED - bit1 SGI#1 set, + 0x813f0000 existing GIC state).
  BC SGIPROBE=0: the worker reads that SAME GICR register and sees bit1=0.
=> cpu0's GICR write demonstrably LANDS and PERSISTS, yet the worker's read of it does not reflect it. So
   the worker's reads are a FROZEN SNAPSHOT of the whole physical address space (DRAM + MMIO) at MMU-enable;
   cpu0's post-bringup writes to ANY address are invisible to the worker. Only pre-bringup data is readable
   (STATICPROBE=0x57a70ded confirms). This is the complete, final characterization.
CONSEQUENCES:
- There is NO dynamic cpu0->worker channel at LK (DRAM, all MMIO, GIC all frozen for the worker's reads).
  The per-frame render split (the "60fps at lower power" dream) is IMPOSSIBLE - it needs the ~20 dynamic
  words per frame and no channel can deliver them. This closes the per-frame split for good.
- The PRODUCER-OFFLOAD with STATIC-ONLY inputs is the sole viable 2-core model, and it is CONFIRMED: the
  worker reads static pre-bringup assets (card list, boxart, the home scene @0x50C00000 built pre-bringup)
  from its frozen snapshot, builds deterministic per-focus card-strip tiles (each tile is fixed given the
  static card list - no dynamic input needed), writes them out (worker->cpu0, proven), and cpu0 selects the
  tile for the live focus. Eliminates the ~30ms scroll-rebuild hitch. Modest vs the per-frame dream but
  real and coherency/channel-free. Constraints remain: memory (N tiles) and the focus-specific L2 skip
  (build tiles WITHOUT skip_focus + draw the focus decorations live on cpu0, or per-system scoping).
DECISION: the coherent/dynamic dream is exhausted and closed. The deliverable is the static-input offload.

### OFFLOAD SIZING (2026-08-28): 21 games -> memory-tight or needs the render refactor
Measured the actual pack: game_count = 21 (snespack.h hdr @byte 0x64). Offload memory analysis:
- PER-FOCUS tiles (reuse build_cardcache as-is, correct skip_focus, cpu0 selects tile[focus]): 21 tiles.
  Each ~2.5-3.66MB (2496 x [~250..384] x 4) => ~52-76MB. The 128MB WB window holds LK + the 14.6MB pack +
  4MB scene + framebuffers + OVL L2/L3, leaving ~50-90MB - so 52-76MB is TIGHT-to-OVER. Risky/borderline.
- RANGE tiles (cover the card row in ~5 wide tiles = ~18MB, fits easily) require FOCUS-INDEPENDENT tiles
  (no skip_focus) => the L2/L3 render refactor (draw all cards on L2, focus decorations live) - real visual
  risk on the working menu.
- "Offload only the on-demand rebuild" (cheapest) is IMPOSSIBLE: it needs the CURRENT focus, which is the
  now-dead dynamic cpu0->worker channel.
So even the surviving offload is NOT a free win: per-focus tiles are memory-tight for 21 games, and the
memory-efficient variant needs the render refactor. This is the concrete tradeoff for the go/no-go call.
FINAL STATE: dynamic 2-core (per-frame split) is definitively dead (worker read path fully frozen). The
static-input offload is viable but constrained (21 games => ~52-76MB per-focus, or an L2/L3 refactor).
Awaiting the user's decision: build the offload (accept the memory/refactor cost) or bank the research.

### LK MEMORY MAP (2026-08-28) - confirms per-focus tiles are fragmented/tight
Window [0x4E000000,0x56000000) = 128MB (only WB DRAM mapped in LK; 0x56000000+ faults). Allocations:
  0x4E000000-0x50000000 (32MB): display framebuffers (~10MB double-buffered 1280x960x4) + slack
  0x50000000 (14.6MB pack blob) ; 0x50C00000 (16MB home scene pool, HOME_CAP; the EXPT canary 0x51000000
    sits inside it) ; 0x53000000 (~4.7MB chrome cache) ; 0x54000000 (3.66MB OVL L2) ; 0x55000000 (OVL L3)
Free gaps are FRAGMENTED (largest ~20-32MB): ~0x51C00000-0x53000000 (20MB), 0x53480000-0x54000000 (12MB),
0x55xxxxxx-0x56000000 (~15MB), plus framebuffer slack. Total free maybe ~50-79MB but NO single large
contiguous block. So 21 per-focus tiles (~76MB) do NOT fit contiguously and barely fit even spread across
gaps - genuinely tight/fragmented. The memory-efficient path (one ~7.5MB focus-independent card-row
texture) fits easily but needs the L2/L3 render refactor.

### FINAL STATE (2026-08-28) - investigation complete, decision is the user's
- DYNAMIC 2-core (per-frame split, the 60fps-lower-power goal): DEFINITIVELY DEAD. The worker's entire
  read path (DRAM + all MMIO + GIC) is a frozen snapshot at MMU-enable; no dynamic cpu0->worker channel
  exists at LK (proven decisively by the GICR write-lands-but-worker-cannot-read test).
- STATIC-INPUT OFFLOAD: viable (STATICPROBE positive) but CONSTRAINED for this 21-game pack: per-focus
  tiles are memory-tight/fragmented (~76MB in ~50-79MB fragmented free), and the memory-efficient variant
  needs the L2/L3 render refactor (visual risk on the working menu). Modest gain (removes ~30ms scroll
  rebuild). The cheapest "offload the on-demand rebuild" is impossible (needs the dead dynamic focus).
Awaiting the user's go/no-go. All host-validated pieces (pack, band split, state-pack) and the full live-
register toolchain (tools/live_regpoke) are preserved. If "build": start with the refactor path (fits
memory) and host-validate focus-independent-row + windowed-blit == single-core. If "bank": restore the
clean single-core build (lk_a_snes_signed.img).

### BETTER OFFLOAD DECOMPOSITION (2026-08-28): per-CARD pre-render (~5.6MB) - memory problem solved
The per-FOCUS-strip tiling (76MB, fragmented, or full render refactor) was the wrong granularity. Better:
- WORKER (static, once at startup): pre-render each of the 21 GAME CARDS in its NORMAL (non-focused) state -
  boxart scaled + framed + icons - into 21 small buffers (~246x270x4 ~= 265KB each => ~5.6MB total). Each
  card is deterministic from the static pack (no dynamic input) - exactly what the frozen-snapshot worker
  can read/build. Writes them out via the proven worker->cpu0 direction.
- cpu0 (per rebuild): build the card strip by BLITTING the pre-rendered cards at focus-computed positions,
  instead of calling draw_card (full render + boxart scale) per card. The blit is far cheaper than the
  render that dominates the ~30ms build, so the scroll-rebuild hitch shrinks a lot. The focused card's
  bright/blue decoration stays on the existing L3 overlay (draw_focus_card) - no skip_focus / L2-L3
  refactor needed, because the L2 strip now holds pre-rendered NORMAL cards (focus handled on L3 as today).
WHY THIS IS BETTER: memory ~5.6MB (fits the fragmented window trivially, no per-focus 76MB); it OFFLOADS
the expensive part (per-card boxart render) to the worker while cpu0 keeps the cheap compositing; and it
needs only a MODERATE change (build_cardcache blits cached cards vs draw_card) rather than a
focus-independent-render refactor. Still static-input only (no dynamic channel), so it works with the
frozen worker. This is the recommended offload path if the user says "build".
GO/NO-GO now: the offload is memory-FEASIBLE and lower-risk than earlier thought (per-card, ~5.6MB,
moderate refactor). Trade: real engineering effort + a build_cardcache change, for a reduced (not
eliminated) scroll hitch. Awaiting the user's decision.

### CLOSING FINDING (2026-08-28): blit is nearest-neighbor + phase-sensitive - the per-card offload cannot be bit-exact
Traced the actual pixel path (snes_render.c blit(), lines 135-156). The fast axis-aligned path samples the
source with a 16.16 FIXED-POINT nearest-neighbor walk: su = (int)(su0f*65536), ix = su>>16, where the start
phase su0f = sx + u0*sw and u0 = ((x0+0.5 - e)*inv_a + px)/dw. e carries the card's FRACTIONAL cx. So the
source texel picked for each dest pixel depends on cx's SUB-PIXEL phase. Consequence for the per-card offload:
pre-rendering a card into a tile at a canonical phase and re-blitting that tile at a different fractional cx
does NOT reproduce draw_card(...,cx,...) pixel-for-pixel - nearest-neighbor sampling shifts by a texel at the
edges whenever the phase differs. It would "fail" the bit-exact worst-tile host validation (and could show a
faint 1-texel edge shimmer vs the reference on a panning carousel), even though the boxart interior is fine.

This closes the last frozen-snapshot-compatible variant:
 - Band split (build_cardcache_band, already prototyped): pixel-exact, but needs cpu0 to tell the worker the
   CURRENT focus each nav -> a dynamic per-nav channel -> DEAD (frozen snapshot).
 - Per-card tiles (focus-independent, survives freezing): re-blit at live cx is nearest-neighbor phase-shifted
   -> NOT bit-exact -> fails the project's exactness bar; only acceptable if the user accepts a visual delta.
So every offload is either dynamic-and-dead or static-and-not-bit-exact. There is no clean, exact, frozen-
compatible win for this menu. RECOMMENDATION: BANK. The dynamic 60fps-lower-power dream is definitively dead;
the salvageable static offload is marginal AND cannot meet the bit-exact standard the rest of the menu holds.
The full toolchain (tools/live_regpoke, band-split code, state-pack, host harness) is preserved so a future
non-exact "good enough" attempt is a small step, not a restart. Clean single-core build stands as shipping.

### BREAKTHROUGH LEAD (2026-08-28): MCSI (Mediatek Cache Snoop Interconnect) + secure-write-protect
Re-examined the kernel (/work/mt6785_kernel_source) for the DSU late-join snoop hypothesis. Two findings:

1. mcusys/mcucfg registers are SECURE-WRITE-PROTECTED. drivers/misc/mediatek/include/mt-plat/mtk_secure_api.h
   defines CONFIG_MCUSYS_WRITE_PROTECT and routes ALL mcusys writes through ATF via SMC
   MTK_SIP_KERNEL_MCUSYS_WRITE (0x82000287). CONSEQUENCE: the earlier plan to hotplug-diff mcucfg(0x0c530000)
   with a raw ioremap+readl is FLAWED - a NS-EL1 read of a secure mcusys reg returns 0 (read-protect), which
   is exactly the "SPM 0x768 phantom 0x0" pattern we already hit. The diff must go through the ATF READ SMC.
   It also means LK at NS-EL1 (AArch32) CANNOT poke a DSU/snoop reg in mcusys directly; it must call an ATF SIP.

2. MCSI is the snoop interconnect and has dedicated secure SMC accessors (same header):
   - MTK_SIP_KERNEL_MCSI_A_WRITE   = 0x82000289  (phys addr write)
   - MTK_SIP_KERNEL_MCSI_A_READ    = 0x8200028A  (phys addr read; returns value in r0)
   - MTK_SIP_KERNEL_MCSI_NS_ACCESS = 0x8200028B  (sub: 0=read,1=write,2=set_bitmask,3=clr_bitmask, by offset)
   - MTK_SIP_KERNEL_CACHE_FLUSH_BY_SF = 0x82000283  (flush caches BY SNOOP FILTER - proves a walkable SF exists)
   - MTK_SIP_KERNEL_L2_SHARING     = 0x82000286
   The MCSI REGISTER MAP is NOT in kernel source (no mcsi_reg_read callers compiled for mt6785) - MCSI is
   managed entirely in ATF/BL31 firmware. So the per-core snoop-admission bit lives in ATF, invisible here,
   but is READ/WRITE-reachable from NS via the SMCs above IF our ATF-at-LK implements those SIP handlers.

WHY THIS FITS THE SYMPTOM: a late-joined core that ATF powered but did NOT admit into the MCSI snoop filter
would have its LOADS bypass cpu0's dirty cache lines (read stale) while its coherent STORES still drain to the
domain - the exact asymmetry observed (worker stores seen by cpu0; cpu0 stores unseen by worker).

NEXT EXPERIMENTS (both now concretely buildable; LK already has mt_secure_call_all() for arbitrary SMCs):
 - LIVE (needs device): kernel module that calls mt_secure_call(MCSI_A_READ=0x8200028A, off) to DUMP the MCSI
   register file with the worker core hotplug-OFFLINE vs ONLINE. The bit(s) that toggle = the snoop-admission
   control. This is the CORRECT (secure) version of the mcucfg hotplug-diff (raw readl reads 0).
 - LK ON-HW (this cycle): AYANEO_BC_MCSI build - after PSCI CPU_ON, cpu0 issues MCSI_A_READ over a sweep of
   offsets and CACHE_FLUSH_BY_SF, logging every SMC return to UART. Tells us (1) whether ATF-at-LK exposes the
   MCSI SIPs at all, and (2) the live MCSI snoop-filter state at LK time. If the worker shows as not-admitted,
   MCSI_A_WRITE/set_bitmask to admit it becomes the candidate fix. Diagnostic-first, read-only + benign flush,
   low risk (user away). SMC32 IDs used directly (LK is AArch32, AARCH bit = 0).

### ROOT-CAUSE CANDIDATE (2026-08-28): MCSI per-cluster snoop-admission, with exact register map + a FIX build
Pulled the ARM ATF mt8183 MCSI driver (plat/mediatek/mt8183/drivers/mcsi, saved to tools/mcsi_ref/) - same
MCSI generation as MT6785. This is the mechanism behind the "late core loads don't see cpu0 stores" wall:

MCSI register map (mcsi.h): CENTRAL_CTRL=0x0, SF_INIT=0x10, SF_CTRL=0x14, SNP_PENDING=0x28, FLUSH_SF=0x500.
Per-slave-interface base = 0x1000 + 0x100*i (i=0..7); SNOOP_CTRL_REG = base+0x0.
SNOOP_CTRL bits: SNOOP_EN(0), DVM_EN(1), SNP_SUPPORT(30), DVM_SUPPORT(31). The CPU CLUSTERS are the
coherent ACE slave ifaces (mt8183 uses iface 3 and 4). cci_enable_cluster_coherency(mpidr) picks the iface
by aff1 (cluster_id) and sets SNOOP_EN|DVM_EN; cci_disable (called on power-OFF) CLEARS them.

WHY IT FITS: our LK worker is mpidr=0x100 (aff1=1) = CLUSTER 1. If both big cores were off at LK time, cluster
1's MCSI slave iface has SNOOP_EN=0 (cleared by the last power-off). ATF's PSCI on_finish is supposed to call
cci_enable_cluster_coherency for the first core of a cluster; if our ATF-at-LK defers that to the SSPM/MCUPM
runtime path (absent pre-kernel), the worker cluster comes up POWERED (SPM ack bit31 set, as observed) but
NOT snoop-admitted. Then: cpu0's dirty lines sit in cluster-0 caches, never snooped by cluster 1 -> worker
LOADS read stale DRAM (miss cpu0 stores), while the worker's writes still drain to DRAM where cpu0 reads them.
That is EXACTLY the observed asymmetry. This supersedes the earlier "frozen snapshot, dead" conclusion.

REACHABLE FROM LK: mcusys/MCSI is secure, but MCSI_NS_ACCESS (SMC 0x8200028B: a0=op 0rd/1wr/2set/3clr,
a1=offset, a2=val; ATF holds the base so we pass OFFSETS) exposes read AND write to NS. LK already has
mt_secure_call_all() for SMCs, so cpu0 can both diagnose and FIX the snoop-admission from NS-EL1 AArch32.

TWO IMAGES BUILT + SIGNED + STAGED to /mnt/c/pairmini (flag-gated, shipping menu untouched):
 - lk_a_snes_mcsi_signed.img (AYANEO_BC_MCSI): after worker join, cpu0 reads CENTRAL_CTRL, SF_INIT,
   SNP_PENDING and SNOOP_CTRL for all 8 slave ifaces, logging SNOOP_EN/DVM_EN/SNP_SUPPORT/DVM_SUPPORT per
   iface, plus MCUSYS_ACCESS_COUNT (SIP liveness) and FLUSH_BY_SF. Pure diagnosis (read-only + benign flush).
 - lk_a_snes_mcsifix_signed.img (AYANEO_BC_MCSI_FIX): same diagnosis, THEN for every iface that SUPPORTS
   snoop but has SNOOP_EN=0 it set_bitmask SNOOP_EN|DVM_EN (admits the worker cluster), re-reads, flushes by
   SF, and re-checks the worker coherency canary (w_can1/w_menuw0/w_static_can). If those flip to real/0xCA5A
   after admission, the multicore wall is BROKEN and the host-validated 2-core render/cardcache splits go live.
FLASH ORDER when the MTK device (0123456789ABCDEF) is back: diagnostic first (confirm SNOOP_EN=0 on the worker
cluster iface + SIP liveness), then the FIX. Both are experiment images; restore lk_a_snes_signed.img after.

NOTE (device hygiene): the adb serial a28c0e0e seen this session is NOT our MT6785 target (it is a
GammaOS/Lineage device); our modules failed to load there (wrong kernel) so nothing ran on it. All live MCSI
probing must target 0123456789ABCDEF only. The live smcpoke path additionally needs a matching Module.symvers
(current out-of-tree build has none -> insmod ENOENT); the on-HW LK experiment above sidesteps that entirely.

### TOPOLOGY CONFIRMATION (2026-08-28): cpu0 and the worker are in DIFFERENT clusters -> MCSI is the right layer
From the SoC's own device tree (mt6785_kernel_source arch/arm64/boot/dts/mediatek/mt6785.dts):
  cpu0 cpu@000 A55 | cpu1 cpu@001 A55 | cpu2 cpu@002 A55 | cpu3 cpu@003 A55   -> CLUSTER 0 (aff1=0)
  cpu4 cpu@100 A55 | cpu5 cpu@101 A55 | cpu6 cpu@102 A75 | cpu7 cpu@103 A75   -> CLUSTER 1 (aff1=1)
cpu0 (the LK boot core) is mpidr 0x000 = CLUSTER 0. The LK worker is brought up at mpidr 0x100 = cpu4 =
CLUSTER 1. So producer (cpu0) and consumer (worker) sit in TWO DIFFERENT DynamIQ/MCSI clusters, NOT one.

This retires the earlier "single DSU, intra-cluster auto-coherency" assumption that made the wall look
intractable. Cross-CLUSTER coherency on MTK is mediated by MCSI: each cluster is its own coherent ACE slave
interface whose SNOOP_EN/DVM_EN must be set for that cluster to participate. During LK only cpu0 (cluster 0)
ever runs, so cluster 1 sat fully powered-off and its MCSI slave iface still has SNOOP_EN=0 (cleared by its
last cci_disable_cluster_coherency at power-off). ATF-at-LK powers cpu4 (rail/SPMC ack bit31 set, as measured)
but does not run cci_enable_cluster_coherency for cluster 1 -> the worker is powered but snoop-unadmitted, so
its loads bypass cluster 0's dirty lines exactly as observed. The staged AYANEO_BC_MCSI / _FIX images test and
(candidate) fix precisely this. Bonus: cpu4 is an A55, same core type as cpu0, so no big/little ISA surprises
for the render-split code. Confidence in the MCSI root-cause is now high; awaiting the diagnostic UART dump.

### ATF POWER-PATH RE (2026-08-28): the coherency-enable is CONDITIONAL and SSPM-gated - matches the wall
Pulled the ARM ATF mt8183 plat_pm.c (nearest public MTK PSCI power path; saved analysis, not committed as it
is a different plat). The cross-cluster coherency enable lives in pwr_domain_on_finish:
  plat_mtk_power_domain_on_finish():
     if (afflvl1 == OFF) plat_cluster_pwron_common(mpidr, cluster);  // <-- the ONLY caller
     ...
  plat_cluster_pwron_common():  enable_scu(mpidr); plat_mtk_cci_enable();   // SCU + MCSI cluster snoop-enable
     (for cluster>0 also: l2c_parity_check_setup, mp1_L2_desel_config, mt_gic_sync_dcm_disable)
  plat_cluster_pwrdwn_common(): plat_mtk_cci_disable(); disable_scu(mpidr);  // clears it on cluster-off

Two facts explain why the worker cluster comes up snoop-DISABLED at LK:
 1. plat_mtk_cci_enable() runs ONLY when the PSCI framework passes afflvl1 == LOCAL_STATE_OFF (i.e. it believes
    the whole cluster was off and is now powering on). LK's minimal PSCI CPU_ON path / power-state coordination
    may not present that transition, so on_finish skips plat_cluster_pwron_common entirely.
 2. Normal hotplug is SSPM/MCDI driven by default (HP_SSPM_CTRL=true, MCDI_SSPM=true): cluster/cpu power and the
    coherency choreography go through mcdi_hotplug_* -> SSPM firmware. At LK there is NO SSPM/MCUPM runtime, so
    that path is inert and the software cci_enable fallback only fires on the afflvl1==OFF condition above.
Net: at LK the cross-cluster MCSI snoop-enable (plat_mtk_cci_enable == cci_enable_cluster_coherency, which sets
SNOOP_EN|DVM_EN on the cluster's MCSI slave iface) is skipped -> worker (cluster 1) powered but not admitted ->
its loads miss cpu0 (cluster 0) dirty lines. This is the same mechanism the staged AYANEO_BC_MCSI_FIX forces by
directly set_bitmask-ing SNOOP_EN|DVM_EN on the unadmitted slave iface via MCSI_NS_ACCESS. The staged diagnostic
will confirm empirically (expect cluster-1 iface: SNP_SUPPORT=1, SNOOP_EN=0; cluster-0 iface: SNOOP_EN=1).
Caveat: enable_scu(mpidr) (the core/DSU-level snoop, ~CPUECTLR.SMPEN) is the WORKER's own on_finish step and
runs on the worker; on DynamIQ A55 the core auto-joins the DSU at reset, so the cpu0-side MCSI enable is the
actionable missing piece. If the diagnostic shows SNOOP_EN already 1, MCSI is not the layer and we pivot to the
SCU/DSU-level or the PSCI-state-coordination angle.

### INDEPENDENT LEVER ADDED (2026-08-28, task d): broadcast DVM/TLBI-IS coherent barrier from cpu0
Added a second, SIP-independent lever to the AYANEO_BC_MCSI path so a single flash triangulates the wall.
After the MCSI snoop-admission read/set + FLUSH_BY_SF + canary readback, cpu0 now also issues a broadcast
inner-shareable DVM/TLBI and coherent barrier (AArch32: DSB ISH; TLBIALLIS = mcr p15,0,Rt,c8,c3,0; DSB ISH;
ISB), then re-reads the worker canary. Rationale: if cluster 1's DSU/MCSI snoop filter is merely STALE rather
than snoop-DISABLED, a cross-cluster DVM sync can force a resync; this does not rely on the MCSI SIP being
implemented in ATF-at-LK. Attribution: canary flips after the MCSI set => snoop-admission was the wall; flips
only after TLBIALLIS => DVM-sync was the wall; flips after neither => pivot (SCU/DSU-level or PSCI-state path).
Rebuilt + signed + staged both lk_a_snes_mcsi_signed.img (diagnostic + DVM) and lk_a_snes_mcsifix_signed.img
(diagnostic + MCSI admit + DVM). Shipping menu untouched (flag-gated). Awaiting the MTK target to flash.

### SSPM/MCUPM MCDI RE (2026-08-28, task b): rules OUT a firmware coherency step; reinforces the ATF/MCSI fix
Examined the kernel MCUPM/MCDI driver (drivers/misc/mediatek/mcupm/v2 + mt68xx headers). Findings:
 - MCUPM IPI channels are exactly: CH_S_PLATFORM, CH_S_CPU_DVFS, CH_S_FHCTL, CH_S_MCDI, CH_S_SUSPEND,
   CH_S_EEMSN. That is DVFS / frequency-hopping / multi-core deep-idle / suspend / thermal. There is NO
   coherency, snoop, L3, or cluster-coherent-on channel. Grep for coher/snoop/l3/dsu across mcupm + mcdi +
   cpuhotplug returns nothing (done a prior cycle too). So the firmware does POWER/DVFS/idle, not snoop-admit.
 - MCUPM firmware lives in reserved memory and is brought up by a KERNEL driver (RESERVEDMEM_OF_DECLARE +
   platform probe), i.e. at kernel stage - it is NOT running during LK. So at LK the ATF mcdi_hotplug ->
   SSPM/MCUPM path is inert; ATF necessarily uses its direct spm_poweron_cpu/cluster branch.
CONCLUSION: the "SSPM/MCUPM post-power DSU/L3 coherency-sync step" hypothesis (from the corrected findings) is
retired. Coherency-enable is ATF-software only (plat_mtk_cci_enable == cci_enable_cluster_coherency, i.e. MCSI
SNOOP_EN|DVM_EN on the cluster slave iface), and it is reached in on_finish ONLY when PSCI passes afflvl1==OFF.
At LK, with MCUPM absent, whether that fires hinges entirely on LK's thin PSCI CPU_ON coordinating the cluster
affinity state - which a bootloader typically does not. That is the single remaining software gap, and it is
exactly what the staged AYANEO_BC_MCSI_FIX bypasses by setting SNOOP_EN|DVM_EN directly via MCSI_NS_ACCESS.
Three independent angles (device-tree topology, ATF on_finish path, MCUPM IPI map) now agree on the mechanism.

### MT6785 CPU-SUBSYS PHYSICAL MAP + SIP-absent contingency (2026-08-28)
From mt6785.dts, the coherence-relevant blocks:
  mcucci  (MCU Cache-Coherent Interconnect = MCSI) @ 0x0c510000
  mcusys_par_wrap / mcucfg                          @ 0x0c530000 (0x10000, 64KB, secure-write-protected)
  mp_cpusys_top                                     @ 0x0c538000
  cpccfg_reg (the CPC-arm patch target)             @ 0x0c53a800 ; cpcdbg_reg @ 0x0c53ab00
  mcdi                                              @ 0x0011b000 + 0x0c53a000
So the MCSI base on MT6785 is 0x0c510000 ("mcucci"). Applying the standard ARM MCSI/CCI-550 layout (mt8183
mcsi.h: central regs 0x0..0x500, slave ifaces 0x1000 + 0x100*N), the cluster-1 coherent slave iface SNOOP_CTRL
would sit near 0x0c510000 + (0x1000 + 0x100*N). NOTE: the staged AYANEO_BC_MCSI diagnostic does NOT need this
base - MCSI_NS_ACCESS takes an OFFSET and ATF applies its own base - it reads offsets 0x0/0x10/0x28 (central)
and 0x1000..0x1700 (all 8 slave ifaces), so it self-identifies the real iface region from the SNP_SUPPORT bits.

CONTINGENCY if our tee.img does NOT implement the MCSI SIPs (the diagnostic's MCUSYS_ACCESS_COUNT probe returns
error/0 => MTK SIP layer absent): a raw NS write to 0x0c510000 will NOT work (mcusys/mcucci is secure-firewalled
from NS, which is the whole reason the SIP exists). The only remaining path would then be an ATF-side patch that
calls cci_enable_cluster_coherency(0x100) in the PSCI on_finish - anchored at MCSI base 0x0c510000 - added to the
same tee.img we already rebuild/re-sign for the CPC-arm bit. That is a much larger change than the SIP poke, so
the SIP path stays strongly preferred; this records the fallback anchor in case the diagnostic shows no SIP layer.
This closes the offline analysis: mechanism (cross-cluster MCSI snoop-admission skipped at LK) is confirmed from
device-tree topology + ATF on_finish path + MCUPM IPI map; fix (MCSI_NS_ACCESS set SNOOP_EN|DVM_EN) and an
independent DVM lever are staged; the empirical verdict now waits solely on a flash of the offline MTK target.

### INTEGRATION CONFIRMED (2026-08-28, task e): the 2-core split is ALREADY WIRED - MCSI fix is the sole keystone
Task (e) needs no new code: the full 2-core pipeline already exists and is flag-gated behind AYANEO_BIGCORE_EXPT
(which AYANEO_BC_MCSI/_FIX imply), so the staged fix image exercises it end-to-end:
 - bringup: bigcore_start() runs the worker PSCI CPU_ON + (in the fix build) the MCSI SNOOP_EN|DVM_EN admit +
   DVM/TLBI lever, ONCE, before the render loop.
 - per frame (snes_driver.c:864): `if (bc_worker_ready() && s_menu.chrome_ready) bc_dispatch(...); else
   snes_menu_render(...)`. bc_dispatch (line 279) forks the BOTTOM scanline band to the worker, cpu0 renders the
   TOP band, then joins with a WALL-CLOCK-bounded fallback (line 382): if the worker misses its deadline cpu0
   renders the worker's band too, so a non-coherent worker degrades to ~30fps, never breaks the menu.
 - self-verdict (line 440): cpu0 checksums the state it published vs g_bc->w_state_sum and prints
   "full split channel LIVE" (match) or "mismatch/frozen". The worker cleans its band to DRAM; cpu0 invalidates
   its stale copy so the display picks up the worker's rows.
So the ordering is exactly: coherency-fix-at-bringup THEN per-frame split. If the MCSI admit works, bc_dispatch's
worker band renders correctly, the state-sum MATCHES, and the menu genuinely runs on two cores with the wall-clock
fallback idle. No worker cache-invalidate is needed post-fix: once SNOOP_EN is on the worker is in the snoop
domain (cpu0's per-frame writes snoop-invalidate the worker's copies), and the static pack/menu input was
published+cleaned before bringup so the worker's read-only cached copies already equal DRAM.

UART INTERPRETATION GUIDE for the flashed fix image (lk_a_snes_mcsifix_signed.img):
 1. "BC MCSI SLV<n> ... SNP_SUP=1 ... SNOOP_EN=0" on the worker cluster iface, SNOOP_EN=1 on cpu0's  => wall
    confirmed = snoop-admission. "BC MCSI FIX: SLV<n> after set ... SNOOP_EN=1" => admit succeeded.
 2. Then "BC MCSI post: w_can1/w_menuw0/w_static_can" flip to real/0xCA5A                             => coherent
    (if they flip only after "post-DVM(TLBIALLIS)" instead, the wall was DVM-sync, not admission).
 3. Then in the running menu: "... full split channel LIVE"                                    => 2-core render
    is genuinely live (the original 60fps-lower-power goal achieved). "mismatch/frozen" => fix insufficient,
    fall back to the SIP-absent / ATF-patch contingency (MCSI base 0x0c510000).
Offline work is fully closed across (b)(c)(d)(e)+topology+physical-map; the empirical verdict waits on a flash.

### PLAN-B ELIMINATED (2026-08-28): EMI MPU is symmetric across clusters, cannot be the wall
Checked whether the MT6785 EMI MPU (memory protection unit at the DRAM controller, BELOW caches - would affect
even uncached/Device reads) could asymmetrically block the worker cluster from cpu0's DRAM. Findings from
drivers/misc/mediatek/emi/mt6785 (mpu_platform.c/.h, mt_emi_api.h):
 - The two CPU clusters ARE separate EMI AXI masters: MT6785_M0_AXI_MST_MP0 (cluster 0, port 0) and
   MT6785_M1_AXI_MST_MP1 (cluster 1). So EMI can distinguish them by master ID - initially a plausible Plan B.
 - BUT MPU permissions are applied per DOMAIN, and domain is transaction-driven: domain_id = (axi_id>>21)&0xF.
   MP0 and MP1 both issue AP-domain traffic (the modem-domain check is_md_master only special-cases domains 1
   and 7 for MDMCU/MD masters). EMI_MPU_DOMAIN_NUM=16 but CPU/AP uses the AP domain for both clusters.
 - set_ap_region_permission gives the AP domain NO_PROTECTION on the general DRAM regions (where the LK
   framebuffer/comms/pack at 0x50000000-0x56000000 live), identically for anything in the AP domain.
CONCLUSION: the EMI MPU treats cluster 0 and cluster 1 the same (same AP-domain permissions), so it cannot
produce the observed asymmetry (worker loads miss cpu0 stores while worker stores are seen). Plan B (EMI MPU
per-cluster firewall) is eliminated. The separate MP0/MP1 master ports do confirm the clusters have independent
paths to memory, consistent with the MCSI-mediated cross-cluster coherency model: a non-snoop-admitted MP1 read
routes MP1 -> MCSI -> EMI but misses cpu0's cluster-0-resident dirty lines. The wall stays in the MCSI/DSU snoop
layer, and the staged MCSI admit remains the primary fix. Every alternative checked (SPM/CPC power regs, CPUECTLR,
AArch32, cache set/way, SSPM/MCUPM firmware, EMI MPU) is now ruled out; MCSI cross-cluster snoop-admission stands
as the single unrefuted mechanism.

### FIX HARDENING (2026-08-28): match cci_enable_cluster_coherency's SNP_PENDING drain
Corroboration: the mt6785 device tree is a TWO-cluster DynamIQ layout (cluster0 aff1=0 = cpu0-3 A55; cluster1
aff1=1 = cpu4-7, 2xA55+2xA75), i.e. two DSUs bridged by MCSI - the classic cross-cluster CCI/MCSI coherency
case, exactly matching the theory. Reviewing the ATF reference cci_enable_cluster_coherency() shows it does not
just set SNOOP_EN|DVM_EN: it polls SNP_PENDING (central reg 0x28, bit31) to drain in-flight snoops BEFORE the
enable and waits for it to clear AFTER, so the admit actually takes effect. My first fix skipped that wait.
Hardened AYANEO_BC_MCSI_FIX: for each unadmitted slave iface it now (1) waits SNP_PENDING clear, (2) set_bitmask
SNOOP_EN|DVM_EN via MCSI_NS_ACCESS, (3) waits SNP_PENDING clear again (bounded 10ms), logging the settle time.
Rebuilt + signed + re-staged lk_a_snes_mcsifix_signed.img. Diagnostic image unchanged. This makes the on-HW
admit faithful to the ATF sequence and less likely to appear set-but-ineffective. Still awaiting the flash.

### SMC ABI VERIFIED (2026-08-28): the MCSI fix pokes the right registers; -1 = SIP-absent sentinel
De-risked a silent-failure mode by verifying the SMC argument mapping against the LK implementation
(app/mt_boot/mt_secure_call.c + mtk_secure_api.h):
  mt_secure_call_all(function_id, arg0, arg1, arg2, arg3, *r1,*r2,*r3) puts function_id->r0, arg0->r1,
  arg1->r2, arg2->r3, arg3->r4, and returns r0 (plus r1..r3 out).
The kernel MTK MCSI ABI is mcsi_reg_read(off)=mt_secure_call(NS_ACCESS,0,off,0,0) and
mcsi_reg_set_bitmask(val,off)=mt_secure_call(NS_ACCESS,2,off,val,0), i.e. r1=op, r2=offset, r3=val.
My experiment calls mt_secure_call_all(MCSI_NS, op, offset, val, 0, ...) -> r1=op, r2=offset, r3=val. EXACT MATCH
for both the reads (op=0) and the admit set_bitmask (op=2, val=0x3). So the fix targets the correct MCSI regs.
Also: mtk_secure_api.h defines SIP_SVC_E_NOT_SUPPORTED = -1. So in the flashed UART, a return of 0xffffffff on
the MCSI reads or on MCUSYS_ACCESS_COUNT means our tee.img does NOT implement the MTK SIP layer -> the SMC path
is unavailable and we fall to the ATF-patch contingency (cci_enable at MCSI base 0x0c510000). A return of 0x0 /
a real SNOOP_CTRL value (SNP_SUPPORT/SNOOP_EN bits sane) means the SIP is live and the diagnosis/fix are valid.
Nothing left to verify offline: mechanism isolated (MCSI cross-cluster snoop-admission), fix correct-by-ABI and
ATF-faithful (SNP_PENDING drained), 2-core split wired with fallback, all alternatives refuted. Verdict = flash.

### OPERATOR GUIDE REWRITTEN (2026-08-28): now reflects the MCSI diagnosis/fix, old guide was misleading
Rewrote OPERATOR_FLASH_GUIDE.md - it still described the retired "DSU hardware wall / CPC bit29 / warmcycle"
framing and pointed at superseded images. New guide: the one-paragraph why (cpu0=cluster0, worker=cluster1,
MCSI cross-cluster snoop-enable skipped at LK), prereq (tee_patched_armcpc.img), FLASH 1 diagnostic
(lk_a_snes_mcsi_signed.img) then FLASH 2 fix (lk_a_snes_mcsifix_signed.img), restore (lk_a_snes_signed.img),
and a full UART decision tree (SIP liveness via MCUSYS_ACCESS_COUNT, the 8 SLV SNOOP_EN lines, the FIX settle
line, the post/post-DVM canary flip, and the "full split channel LIVE" payoff), with the -1=SIP-absent branch
to the ATF-patch contingency. This is what the user reads first on waking; it now routes them correctly.

### SF-INIT GAP CLOSED + SIP SAFETY BOUND (2026-08-28)
Checked the mt8183 mcsi.c cci_init_sf() and cci_reg_access() (the MCSI_NS_ACCESS backend) for prerequisites my
fix might miss:
 - cci_init_sf() initializes the GLOBAL snoop filters SF1/SF2 (SF_INIT_REG 0x10: write TRIG_SF1_INIT/TRIG_SF2_INIT
   bits 0/1, wait SF1_INIT_DONE/SF2_INIT_DONE bits 16/17). This is a ONE-TIME, non-per-cluster init run at boot
   mcsi_init. cpu0 already runs coherently, so the SFs are initialized; admitting cluster 1 needs ONLY SNOOP_EN,
   no SF re-init. The diagnostic already READS SF_INIT_REG (0x10) and logs it, so if (unexpectedly) SF1/SF2_INIT
   _DONE are clear we will see it and add the TRIG_SF init. Gap closed: no SF-init step required by the fix.
 - cci_reg_access confirms SET_BITMASK (op=2) == mmio_setbits_32(base+offset,val)+dsb, i.e. my admit set is a
   read-modify-write OR of SNOOP_EN|DVM_EN with a barrier - exactly right, and the reads are plain mmio_read_32.
 - SAFETY: the SIP handler does `if (offset > MSCI_MEMORY_SZ /*0x10000*/) panic();`. Passing an out-of-range
   offset would PANIC the secure world (reboot/hang). All experiment offsets used are <= 0x1700, well in-bounds.
   Recorded so no future MCSI experiment passes a large offset and bricks the boot.
This is the last prerequisite check; the fix needs no SF-init and its offsets are safe. Verdict still = flash.

### MECHANISM REFINEMENT (2026-08-28): why even UNCACHED worker reads are stale - PoC-in-L3, not DRAM
The one part the bare "snoop-admission" story did not obviously explain (and which made the wall look
intractable) is the corrected finding that the worker misses cpu0's stores EVEN on MMU-off/Device reads - an
uncached read bypasses snoop and goes straight to DRAM, so why stale? Resolution:

The Point of Coherency is defined RELATIVE TO THE COHERENCY DOMAIN. When cluster 1 is not snoop-admitted, the
active coherency domain is {cluster 0 + interconnect}. cpu0's DC CVAC/DCCIMVAC "clean to PoC" only guarantees
visibility to THAT domain; on a DynamIQ + MCSI system the effective PoC for the in-domain agents is the shared
DSU L3 / MCSI snoop-filter level, NOT necessarily DRAM. So cpu0's freshly-cleaned data can sit in the shared
L3/SF while the DRAM behind it stays stale. Cluster 1, being OUTSIDE the domain, cannot snoop that shared L3,
and its uncached read hits the stale DRAM under it. Meanwhile the worker's WRITES post to the interconnect and
are visible to cpu0 (cpu0 snoops/sees them). That reproduces the FULL observed asymmetry, including the
puzzling "even Device/uncached" part, with a single cause: cluster 1 is not in the coherency domain.

CONSEQUENCES / why this matters:
 - It CONFIRMS the MCSI admit is the correct AND sufficient fix: admitting cluster 1 to the snoop domain lets it
   read the shared-L3/PoC where cpu0's data actually lives, fixing BOTH cached and uncached staleness at once.
 - It explains why the earlier DRAM-oriented workarounds (cache set/way, MMU-off reads) could never work: they
   assumed the data was in DRAM, but it is at the in-domain PoC (shared L3) that the outsider cluster cannot see.
 - The 2-core split WANTS the worker to snoop cpu0's L3 (that is the whole point of coherent sharing), so the
   admit is exactly right, not a DRAM-flush hack.
 - Empirically testable by the staged fix: if admitting cluster 1 flips even the uncached canary (w_can_mmuoff)
   to real, this PoC-in-L3 model is confirmed. The diagnostic already captures w_can1/w_menuw0/w_static_can and
   the comms has w_can_mmuoff for the MMU-off read.
This closes the last conceptual paradox; the mechanism is now fully self-consistent end to end.

### ATF BINARY ANALYSIS (2026-08-28): the fix's SIP IS present; corrected a false-negative liveness probe
Analyzed the actual tee_patched_armcpc.img (our BL31/ATF, 162KB, "atf" magic header). Findings:
 - The MediaTek SIP layer IS in this ATF: strings mediatek_sip_handler / mediatek_sip_svc / mtk_sip_svc.c /
   plat/mediatek/mt6785/bl31_plat_setup.c, plus CCI code (cci_adb400_dcm_config). So SMCs are dispatched.
 - SIP func-id scan (32-bit LE literals): MCSI_NS_ACCESS 0x8200028B is PRESENT (the exact SIP the fix uses to
   set SNOOP_EN|DVM_EN and to read the ifaces). MCUSYS_ACCESS_COUNT (0x82000288), CACHE_FLUSH_BY_SF (0x82000283),
   MCSI_A_READ/WRITE, MCUSYS_WRITE, L2_SHARING are all ABSENT as full literals. So this ATF handles MCSI_NS_ACCESS
   but not those others. Positive indicator the fix's SMC path works; the ATF-patch contingency is likely NOT
   needed.
 - BUG FOUND + FIXED in the experiment: the diagnostic used MCUSYS_ACCESS_COUNT as its "SIP liveness" probe, but
   that SIP is ABSENT here, so it would return 0xffffffff and FALSELY report "no SIP layer" - sending the user
   to the ATF-patch contingency even though MCSI_NS_ACCESS works. Corrected: liveness is now judged from the
   MCSI_NS_ACCESS read of CENTRAL_CTRL (0x0) itself - a real value => live, 0xffffffff => absent. Removed the
   MCUSYS_ACCESS_COUNT call. The FLUSH_BY_SF line will return 0xffffffff (expected, harmless; TLBIALLIS still runs).
 - Rebuilt + signed + re-staged both lk_a_snes_mcsi_signed.img and lk_a_snes_mcsifix_signed.img. Updated the
   operator guide's decision tree (step 1) to read the new "BC MCSI SIP-LIVE:" line instead of MCUSYS_ACCESS_COUNT.
This materially de-risks the flash: we now expect SIP-LIVE=YES, the worker-cluster iface SNOOP_EN=0, admit sets
it, and the canary flips. Binary evidence says the SMC fix path exists in the ATF we already run.

### DISASM CORRECTION (2026-08-28): the MCSI SIP is NOT in this ATF - SMC fix cannot work, raw-read added
Corrects the previous cycle. Properly disassembled tee_patched_armcpc.img (aarch64 objdump) instead of scanning
for 32-bit literals (which gave a FALSE positive: 0x8200028b appeared only as coincidental UNALIGNED bytes
spanning an ADD+MOVZ, not a real reference). The real SIP dispatch is a binary-search of 236 `movk w0,#0x8200/
#0xc200 lsl16` + `cmp w20,w0`. Enumerating every handled low-half: the 0x2xx block runs ...0x272 0x273 0x274
0x275 0x276 then JUMPS to 0x2a0 0x2b0 0x2b1. The ENTIRE 0x28x range is ABSENT: MCSI_NS_ACCESS(0x28b),
MCUSYS_WRITE(0x287), MCUSYS_ACCESS_COUNT(0x288), MCSI_A_READ/WRITE(0x28a/0x289), CACHE_FLUSH_BY_SF(0x283),
L2_SHARING(0x286) - none dispatched. (The `mov w1,#0x289/#0x28a` hits are __assert line numbers 649/650, not
SIP ids.) So this BL31 build was compiled WITHOUT the MCSI/mcusys SIP feature.

CONSEQUENCES:
 - The SMC-based fix (MCSI_NS_ACCESS set_bitmask) will return SIP_SVC_E_NOT_SUPPORTED(-1) on this tee.img and
   CANNOT enable snoop. The SIP path is out; the ATF binary patch is now the REQUIRED path, not a fallback.
 - Even the SIP-based diagnostic reads return -1, so they cannot show the SNOOP_EN state. FIX: added a RAW-READ
   probe - LK reads mcucci directly at phys 0x0c510000 (CENTRAL 0x0, SNP_PENDING 0x28, slave SNOOP_CTRL
   0x1000+0x100*n). MCUSYS write-protect firewalls WRITES; NS READS of mcusys previously returned 0 (not an
   abort), so a direct read likely reveals the real snoop-control state without any SIP. Rebuilt+staged both
   images; "BC MCSI RAW ..." lines now report the cluster ifaces regardless of SIP.
NEXT (real fix path): patch the ATF. Either (a) add an MCSI_NS_ACCESS/mcusys-write SIP handler into a free slot
of the dispatch, or (b) simpler, patch the existing PSCI on_finish / plat_mtk_cci_enable path (the ATF already
has CCI code - string cci_adb400_dcm_config) to force cci_enable_cluster_coherency for cluster 1. Requires
locating cci_enable + the afflvl1 gate in the disasm; the ATF is already rebuilt/re-signed for the CPC-arm bit,
so an added binary patch ships the same way. First though: flash the diagnostic and read the RAW lines - if the
raw read shows cluster-1 SNOOP_EN=0, the whole theory is confirmed on-metal and the ATF patch is well-targeted.

### HONEST REFRAMING (2026-08-28): this ATF has NO software MCSI - coherency is CPC/DSU-hardware, not MCSI regs
Disassembly of tee_patched_armcpc.img shows: ZERO references to the MCSI base 0x0c510000 (mcucci), and many to
the mcucfg region 0x0c53xxxx - but the identifiable 0x0c53 accesses are DCM/clock config (strings
"cci_adb400_dcm_config", "sync_dcm_config" are debug prints about DCM addresses, not snoop control). So:
 - This BL31 build has NO software MCSI driver (consistent with the absent MCSI SIP block). The mt8183 MCSI
   model I used as the reference (SNOOP_EN|DVM_EN on a slave iface at 0x0c510000+0x1000+0x100*n) likely does
   NOT apply to MT6785's ATF - MT6785 uses the CPC (Cluster Power Controller) / DSU-integrated coherency, where
   the DSU snoop is admitted as part of the CPC hardware power-on handshake, not a software register write.
 - CONSEQUENCE for the staged diagnostic: the raw read of mcucci 0x0c510000 will probably return 0s (the MCSI
   block is inactive/unused on this SoC), which is itself the confirmation that MCSI is not the layer here.
 - This partially walks back the MCSI-specific fix (SNOOP_EN via SIP or direct write). What STANDS unchanged:
   the diagnosis that cpu0 and worker are in different clusters and the cluster-1 coherency-enable is skipped at
   LK. What CHANGES: the enable is a CPC/DSU-hardware step (mcucfg/CPC region 0x0c53xxxx), not an MCSI reg.

WHERE THIS POINTS (the pointed next experiment, needs the live device): task (a) done RIGHT - hotplug-diff the
FULL mcucfg 0x0c530000..0x0c540000 (64KB) with cluster 1 offline vs online. The earlier diff only covered the
CPC power block near 0x0c53a700 and found only power-gating bits. The broader mcucfg (0x0c530000..0x0c53a000)
includes the DSU/mcusys coherency + ADB400 bridge control the ATF touches; the register that toggles when
cluster 1 goes coherent (online) but is NOT set at LK is the true snoop-admission control and the real fix
target. The staged mcucci raw-read is a cheap first check (expected 0s => MCSI ruled out on-metal); the mcucfg
hotplug-diff is the decisive one. Both need the MTK target (0123456789ABCDEF) back online.
Integrity note: several prior cycles over-committed to the mt8183 MCSI model; this ATF disasm corrects the fix
target to CPC/mcucfg. The 2-core split wiring and the cross-cluster diagnosis are unaffected.

### TOPOLOGY DISAMBIGUATION (2026-08-28): two PHYSICAL DSUs (by MPIDR aff1), distinct from the DVFS cpu-map
Resolved a trap: MT6785 has TWO different core groupings that must not be confused.
 - PHYSICAL DSU / coherency (authoritative, from MPIDR): aff1=0 = {cpu0,1,2,3} (4x A55), aff1=1 = {cpu4,5,6,7}
   (2x A55 + 2x A75). A55/A75 are DynamIQ cores; two distinct aff1 values = TWO separate DSU instances, bridged
   by a coherent interconnect. cpu0 (boot core) is in DSU0; the LK worker (mpidr 0x100 = cpu4) is in DSU1.
 - DVFS / scheduler (cpu-map node): cluster0 = {cpu0..cpu5} (all 6 A55), cluster1 = {cpu6,cpu7} (2 A75). This is
   a FREQUENCY-DOMAIN view (the 6 A55 share an OPP table), NOT the coherency boundary. Reading the cpu-map
   naively would wrongly suggest a 6/2 split; the coherency/DSU split is 4/4 by aff1.
CONCLUSION: the cross-cluster (cross-DSU) diagnosis is CONFIRMED - it is not a single-DSU red herring. cpu0 and
the worker are in different physical DSUs, so cross-DSU snoop admission (managed by CPC/SPMC hardware on this
SoC, since the ATF has no software MCSI/CCI driver) is genuinely required and is the layer being skipped at LK.
This is the stable foundation under all the register-target uncertainty: WHERE (cross-DSU coherency admission)
is settled; only the exact CPC/mcucfg register that performs it is open, to be found by the mcucfg hotplug-diff.

### DECISIVE EXPERIMENT PREPARED (2026-08-28): cross-DSU mcucfg hotplug-diff runner (task a, done right)
Wrote tools/live_regpoke/run_mcucfg_diff.sh so the decisive test runs cleanly the moment the MTK target returns.
Design corrects the earlier partial diff: because DSU1 = {cpu4,5,6,7}, catching the DSU-cluster coherency
register requires powering the WHOLE DSU1 off (offline cpu4-7) vs on - offlining only cpu4 leaves DSU1 powered
and moves just that core's power bit. The script dumps mcucfg (0x0c530000, 64KB) and mcucci (0x0c510000, 4KB)
nonzero-only via regpoke (nz=1) with DSU1 fully OFF vs ON, then diffs. Any register that differs BEYOND the
per-cpu power-gating bits (which the earlier CPC 0x0c53a700 diff already characterized) is the cross-DSU snoop/
coherency-admission control that the ATF/CPC sets when DSU1 joins the domain but LK does not - the true fix
target and the register an ATF patch (or a direct CPC/mcucfg poke, if NS-writable) must set. Needs regpoke.ko
loadable (matching Module.symvers) and the device online. This is the single most informative remaining step;
everything offline points here. cpu4 = mpidr 0x100 confirmed from the DTS.

### ARM DSU MODEL + a NEW (blocked) lever (2026-08-28, WebSearch)
Web (ARM DynamIQ DSU TRM, DSU PMU docs, DynamIQ hotchips) confirms the model but gives NO MediaTek-public CPC
register: a DSU integrates its cores with an L3 + Snoop Filter + async bridges; the cluster power domain
(PDCLUSTER) is PPU-controlled; and TWO DSUs are made coherent by an EXTERNAL coherent interconnect. So MT6785's
cross-DSU coherency admission is an interconnect/CPC action, matching our finding (no software MCSI in the ATF ->
CPC hardware does it). No vendor doc exposes the exact CPC/mcucfg bit; the hotplug-diff must find it empirically.
NEW LEVER surfaced (recorded for completeness, currently BLOCKED): the DSU exposes per-cluster IMP-DEF sysregs
IMP_CLUSTERECTLR_EL1 / IMP_CLUSTERPWRDN_EL1 that a core in that cluster can program to influence its cluster
coherency/power state. In principle the WORKER (cpu4) could set its own DSU cluster register to join the snoop
domain - a lever independent of both the (absent) MCSI SIP and the CPC/mcucfg register. BLOCKER: these are
AArch64-only sysreg encodings, but our worker runs in AArch32 (it adopts cpu0's AArch32 state), so it cannot
access them. It would require bringing the worker up in AArch64 (a large change; and the corrected findings say
AArch32 is not itself the wall). Parked as a future alternative if the CPC/mcucfg-register path does not pan out.
STATE: offline analysis is saturated. WHERE is settled (cross-DSU snoop admission, CPC-managed). The exact
register is the ONE open unknown and needs the prepared run_mcucfg_diff.sh on the live target. Everything that
can be determined without hardware has been determined; the next real progress is a flash or the hotplug-diff.

### LIVE-MODULE ENABLER ASSESSED (2026-08-28): Module.symvers via full build is non-trivial; fallbacks noted
Tried to unblock the live hotplug-diff by generating Module.symvers (so regpoke.ko/smcpoke.ko load reliably on a
CONFIG_MODVERSIONS kernel). Result: a full kernel build is NOT a quick offline win - it is not a toolchain issue
(GCC 11.4 compiled fine into the MTK-specific includes) but MTK's build wiring (e.g. kernel/sched/core.c pulls
mtk_mcdi_api.h which is not on the default include path), so producing Module.symvers needs the full MTK build
harness set up. Deferred: low ROI because this only affects the SECONDARY live-adb hotplug-diff; the PRIMARY path
(flash lk_a_snes_mcsi_signed.img and read UART, incl. the raw mcucci read) needs NO module. Fallbacks when the
target returns: (1) the module may load as-is on the CORRECT device (cron notes a prior session's regpoke WORKED;
the earlier ENOENT was on the wrong device a28c0e0e); (2) if CRCs are enforced, extract Module.symvers from the
live rooted device's kernel image (the /work/557/extract-symvers.py approach over the on-device kernel/__ksymtab).
NET this cycle: confirmed the only two remaining enablers (live device for run_mcucfg_diff.sh; module load) both
depend on the MTK target being connected. Offline research remains saturated; no productive offline step left
that does not require the hardware. Standing by for a flash UART dump or the device to reconnect.

### ATF DSU-SYSREG RE (2026-08-28): ATF does the DSU cluster setup at EL3; the gap is cross-DSU interconnect
Disassembly shows this ATF DOES manage the DynamIQ DSU cluster power/coherency via EL3 IMP-DEF sysregs on the
target core:
  0xc624: mrs x0, s3_0_c15_c3_5 (CLUSTERPWRCTLR_EL1); orr x0,x0,#0xf0 (set [7:4]); msr back; then poll
          s3_0_c15_c3_7 (CLUSTERPWRSTAT) in a wait loop.
  0x1abf8: msr s3_0_c15_c3_6 (CLUSTERPWRDN_EL1), value = x0 & 0x3 (PWRDN/MEMRET bits).
  Also reads s3_0_c15_c3_0/c3_1 (cluster config/id). These are the standard A75/A55 DSU cluster registers.
KEY IMPLICATION: this cluster setup runs at EL3 during PSCI CPU_ON of cpu4, INDEPENDENT of the AArch32 state the
worker later adopts - so DSU1's OWN cluster power + L3 config DOES happen on bringup (consistent with the SPMC ack
being set). Therefore:
 - The wall is NOT the DSU cluster registers and NOT the (previously parked) IMP_CLUSTERECTLR lever: the ATF
   already programs those correctly at EL3. So bringing the worker up in AArch64 to poke them would not help.
 - The remaining gap is specifically the CROSS-DSU INTERCONNECT snoop routing (admitting DSU1 into the coherent
   domain so DSU0<->DSU1 snoops flow). This ATF does that with no software MCSI, so it is CPC/interconnect
   hardware - exactly the register the mcucfg hotplug-diff (run_mcucfg_diff.sh) targets. Reinforces: the fix is a
   CPC/mcucfg interconnect register, found on-device, not a DSU sysreg and not an MCSI SIP.
This further narrows the search and retires the DSU-sysreg alternative. The cross-DSU interconnect admission is
the single remaining unknown, and the whole-DSU1-off-vs-on mcucfg diff is the way to pin it.

### ATF POWER-PATH TRACE - diminishing returns (2026-08-28)
Traced the caller of the DSU cluster-setup (CLUSTERPWRCTLR) code: it is 0xc5fc, called UNCONDITIONALLY from an
init function at 0x92d8 (no mpidr/afflvl1 gate at that call site). No clear conditional interconnect/snoop write
surfaced at this level. Continuing to hand-trace the stripped BL31 to find a possibly-skipped cross-DSU snoop
register is unvalidatable without the device and hitting diminishing returns - each step exposes more untyped
functions and any conclusion is a guess until tested on metal. DECISION: stop the offline BL31 tracing here. The
structural facts are established (ATF programs DSU cluster power/L3 via c3_5/c3_6 sysregs at EL3; no software
MCSI; cross-DSU interconnect admission is the gap) and the RELIABLE way to identify the exact register is the
on-device whole-DSU1 hotplug-diff (run_mcucfg_diff.sh), not more stripped-binary guesswork.

STATE (unchanged, saturated): mechanism = cross-DSU interconnect snoop admission skipped at LK; alternatives all
refuted; fix = a CPC/mcucfg interconnect register (identify via hotplug-diff) or an ATF patch (needs HW to test).
Every remaining step requires the MTK target online. Diagnostics + the decisive runner are staged and pushed.
Holding for a flash UART dump or the device to reconnect; no productive hardware-independent step remains.

### ATF mcucfg-WRITE ENUMERATION (2026-08-28): the ATF's software mcucfg writes are DCM/clock, not coherency
Enumerated the mcucfg (0x0c53xxxx) register offsets the ATF writes (77 movk-0xc53 sites). The identifiable
add-#offset targets cluster in 0x7eb..0xc70 (e.g. 0x7eb,0x811,0x831,0x842,0xa09,0xa33,0xc70). Per the ATF's own
debug strings (cci_adb400_dcm_config, sync_dcm_config) these are DCM / clock-management registers, NOT snoop/
coherency control. (Parse caveat: the base low-half comes from an uncaptured preceding movz, so exact addresses
are approximate - but the DCM nature is clear from the strings.) USEFUL NEGATIVE: the cross-DSU coherency
admission register is NOT in the ATF's software-written mcucfg set. Combined with the ATF having no software MCSI
driver, this means the DSU1<->DSU0 snoop admission is CPC/interconnect HARDWARE-automatic (driven by the CPC
power sequence, no software register poke in the boot ATF). Consequence: it will NOT be found by reading what
the ATF writes; it is only visible as a hardware-driven state change, i.e. exactly what the whole-DSU1
hotplug-diff (run_mcucfg_diff.sh) captures (a register that flips when the CPC powers DSU1 into the domain).
This is why the offline ATF RE cannot pin it and the on-device diff is mandatory. No further offline ATF parsing
is warranted. State unchanged: saturated, awaiting HW.

### ON-METAL RESULT #1 (2026-08-28, user flashed lk_a_snes_mcsi_signed.img): MCSI ruled out on hardware
UART (BC MCSI lines) from the real MT6785:
 - BC ACKPROBE cpu1: SPM_CPU_PWR_CON(1)=0x80000005 (PWR_ON_ACK=1), CPC_SPMC_ST=0xc003 - worker fully powered.
 - BC MCSI SIP-LIVE: NO - SIP absent (CENTRAL_CTRL via SMC = 0xffffffff). Confirms MCSI_NS_ACCESS not in this ATF.
 - BC MCSI RAW @0x0c510000: CENTRAL=0x0, SF_INIT=0x0, SNP_PEND=0x0, and ALL 8 SLV SNOOP_CTRL=0x0. So the mcucci/
   MCSI block reads ALL ZERO from NS - it is unused/inactive (or NS-zeroed) on this SoC. MCSI at 0x0c510000 is
   DEFINITIVELY NOT the coherency layer here, on metal. (The SMC-path SLV lines reading 0xffffffff are just the
   absent SIP returning -1; ignore.)
 - BC MCSI post: w_static_can=0x57A70DED (worker CORRECTLY reads pre-bringup STATIC data -> producer-offload is
   VIABLE on metal), but w_can1=0xa86dbdec (NOT 0xCA5A -> worker does NOT see cpu0's post-bringup store). The
   DVM/TLBIALLIS lever did NOT change it (post-DVM identical). So the dynamic coherency wall is CONFIRMED on
   hardware, and it is NOT MCSI and NOT fixable by DVM broadcast.
CONCLUSION: two facts nailed on metal - (1) the wall is real and MCSI/DVM are not it; (2) static-input producer-
offload works (worker reads pre-cleaned static data). The remaining coherency-fix hunt moves to the CPC.

### ON-METAL RESULT #2 PROBE BUILT: lk_a_snes_cpcdump_signed.img (CPC/DSU register dump)
Next image staged to /mnt/c/pairmini: extends the diagnostic to raw-dump (NS reads of mcucfg return real values,
unlike mcucci) - per-cpu SPM_CPU_PWR_CON[0..7], the CPC block 0x0c53a000..0x0c53ac00 (nonzero), and the
mp_cpusys_top/DSU config 0x0c538000..0x0c538200 (nonzero). Goal: compare DSU0 (cpu0, coherent) vs DSU1 (worker,
powered-not-coherent) CPC state to find the per-cluster coherency/snoop bit that differs. FLASH THIS NEXT and
send the BC CPCDUMP lines.

### ON-METAL RESULT #2 (2026-08-28, lk_a_snes_cpcdump_signed.img): CPC sees the worker as fully up
CPC block dump (0x0c53a000..0x0c53ac00) key regs: 0x0c53a840=0xc003 (CPC_SPMC_ST, matches ACKPROBE);
per-cluster-looking pairs are IDENTICAL for both clusters (0x0c53a2a0=0x0c53a2a4=0x3110aff; 0x0c53a440=
0x0c53a444=0xffff); address-map regs at a260..a278 (incl. 0xc510000 mcucci ptr). mp_cpusys_top (0x0c538000)
shows DSU config (0x538114..130 = 4x {0x1f01,0xfffff}, 538008..01c = 3x {0x2,0x1}) - all reading normal.
(Note: my CPCDUMP per-cpu SPM read used base 0x10006200 not 0x10006000, so those 8 lines read 0x0 - bug, ignore;
ACKPROBE already gave the correct SPM_CPU_PWR_CON(1)=0x80000005.) w_can1 still 0xa86dbdec (wall persists).
INTERPRETATION: every NS-visible CPC/DSU register shows the worker/DSU1 as fully powered + handshaked and the
per-cluster banks that ARE visible read IDENTICAL to cluster0. So the coherency gap is BELOW the CPC's visible
power state - it is the DSU's actual snoop-domain participation, which no NS-visible MMIO register exposes. A
static dump therefore cannot isolate the bit (confirmed: nothing anomalous, no cluster0-vs-cluster1 difference).
The only mechanical way left to catch it is the hotplug-diff (DSU1 coherent-on vs off) on the LIVE kernel, which
requires the phone booted to Android + adb (not the LK menu). ALTERNATIVELY, the static-input PRODUCER-OFFLOAD is
CONFIRMED viable on metal (w_static_can=0x57A70DED) and needs NO coherency fix - a real shippable speedup that
sidesteps the wall. Decision point put to the user: (A) boot Android+adb -> I run run_mcucfg_diff.sh for one more
shot at the coherency register; or (B) I build the producer-offload now.

### ON-DEVICE HOTPLUG-DIFF + LK-vs-LIVE CROSS-REF (2026-08-28): coherency-register candidates found
Ran the hotplug-diff on the live target (regpoke.ko loaded fine). CRUCIAL METHOD: normal hotplug couples power
and coherency (offline = DSU1 fully off), so on-vs-off cannot separate them. But the LK worker state is UNIQUE
(DSU1 POWERED, ack set, but NOT coherent) - so comparing LK-CPC-dump vs live-DSU1-ON (both powered; differ only
in coherency + kernel config) isolates the candidates. Registers where LK differs from live-coherent, in the
CPC/cpccfg region (0x0c53a000):
  0c53a048  LK=0x3    live=0xb     (on==off, power-independent)  <- cleanest: bit3(0x8)
  0c53a658  LK=0x100  live=0x900   (on==off)                     <- bit11(0x800)
  0c53a748  LK=0x50721 live=0x60721 (on==off)                    <- bits[20:16]
  0c53a65c/664/668/718  LK small, live large config (on==off)
  0c53a230  LK=0x1 on=0x40 off=0x02 ; 0c53a844 LK=0x24 on=0x100000 off=0x20 ; 0c53a814 LK=0xb0000 on/off have
    bit29 set, LK clear  (these track power too; LK ~ off, coherent-ON has extra bits: a230 bit6, a844 bit20)
  (0c53ab80-90 are perf counters; ignore. 0c53806c differs too, mp_cpusys_top region.)
CAVEAT: the diff also includes general kernel mcusys config (DVFS/DCM/thermal/credit all base at 0x0c530000), so
not every diff is coherency. But the CPC-region (0x0c53a0xx) set is the power+coherency controller and is the
right candidate pool.

### PROBE BUILT: lk_a_snes_cpcfix_signed.img - write CPC candidates from LK, test coherency
cpu0 writes each CPC candidate register to its live-coherent value after worker bringup, READS BACK (to detect
NS write-protect: mcusys may firewall writes, in which case the write is dropped = confirms ATF/secure path is
needed), and the existing per-frame bc_dispatch then reports "full split channel LIVE" if coherency is achieved.
FLASH THIS NEXT. Key UART: "BC CPCFIX <addr> wrote=0x.. readback=0x.." (did the write land?) and later
"full split channel LIVE" vs "mismatch/frozen".

### REFINED CANDIDATE ANALYSIS + coherent reference saved (2026-08-28, device live on adb)
Saved a full live-coherent CPC/DSU reference: tools/live_regpoke/live_cpc_coherent.txt (1536 regs, exact target
values for the CPCFIX bisect and any ATF patch). Refined the candidates by their on-vs-off behavior:
 - PRIME suspects (track DSU1 coherency: live_ON != live_OFF, and LK reads the non-coherent OFF-like value even
   though DSU1 is POWERED with ack set - the exact anomaly of "powered but not snoop-admitted"):
     0c53a230: coherent=0x40(bit6), off=0x02, LK=0x01   -> coherent needs bit6
     0c53a844: coherent=0x100000(bit20), off=0x20, LK=0x24 -> coherent needs bit20
     0c53a814: coherent has bit29 set, LK=0x000b0000 (bit29 clear)
   These read the powered-DOWN coherency value in LK despite power being on = the snoop-admission state stuck off.
 - Power-independent globals Linux sets but LK doesn't (possible prerequisites): 0c53a048 (0x3->0xb bit3),
   0c53a658 (0x100->0x900 bit11), 0c53a748 (0x50721->0x60721).
lk_a_snes_cpcfix_signed.img writes ALL of these to their coherent values and reads back (write-protect test).
DID NOT write CPC regs on the LIVE running kernel (crash risk, user away) - the isolated LK flash tests writes
safely. Awaiting the CPCFIX flash: LANDED+"full split channel LIVE" => solved (then bisect); DROPPED => NS
write-protected, fix must be an ATF patch (I have the ATF + exact target values now).

### STAGED-HOTPLUG ISOLATION (2026-08-28, live): status regs were CONFOUNDED; only globals are clean signals
Read the candidates across all-online / cpu4-off / cpu4,5,6-off / cpu4,5,6,7-off (DSU1 down):
 - GLOBAL (constant across ALL states, incl DSU1-down): 0c53a048, a658, a65c, a664, a668, a718, a748. These are
   config Linux sets at boot and LK does not (a048: LK=0x3 vs 0xb, etc). Clean "LK is missing this config" signal.
 - STATUS/STATE (vary continuously with which cores are online): 0c53a230, a814, a840(SPMC_ST), a844. My earlier
   "prime suspects (a230/a844/a814)" were CONFOUNDED - LK differs from live on these only because LK has a
   different set of cores online (cpu0 + worker) than live (all 8), NOT because of coherency. Writing them is
   pointless (they reflect state) and potentially harmful.
 - NO register showed the clean cluster-coherency signature (constant while DSU1 has >=1 core, changing only when
   the last DSU1 core leaves). So there is no obvious CPC MMIO bit that means "DSU1 in the snoop domain" - the
   coherency admission is likely DSU-internal (snoop filter), not a CPC MMIO register.
REFINED CPCFIX: write ONLY the 7 global-config regs (a048,a658,a65c,a664,a668,a718,a748) to their live values -
these are the sole legitimate MMIO levers (one could be a global snoop/coherency enable). Dropped the status
regs. If writing all 7 lands but does NOT yield "full split channel LIVE", the coherency admission is proven not
MMIO-accessible from NS -> pivot to ATF patch or the confirmed-viable producer-offload.

### ON-METAL RESULT #3 (2026-08-28, lk_a_snes_cpcfix_signed.img): NS MMIO cannot fix coherency
CPCFIX writes: a658,a65c,a664,a668 LANDED; a048,a718,a748 DROPPED (readback unchanged = NS WRITE-PROTECTED).
So MTK firewalls the coherency-sensitive mcusys registers from NS (CONFIG_MCUSYS_WRITE_PROTECT), and the ones
that DID land are non-coherency config. Split verdict: FROZEN (BC split: wfin=0, wcnt=0, can1=0xa86dbdec). The
MMIO writes did NOT admit DSU1 to the snoop domain.
Full wall characterization from this boot's probes:
 - cpu0->worker BROKEN: worker reads menu ptr = 0xab102d01 (cpu0 wrote 0x4c5d655c) -> then DATA-ABORTs
   (fault type3, far=0xab102d00) dereferencing the garbage; canary 0x51000000 reads 0xa86dbdec (cpu0 wrote
   0xca5a0001); even MMU-off read = 0xa86dbdec.
 - worker->cpu0 WORKS: cpu0 reads the worker's canary2 = 0x77773012 (0x7777 pattern OK).
 - worker SELF-coherent: w_self_wb=0x5e1f..., w_self_dev=0x0de0... both match -> worker's own cached/device
   accesses are fine; the problem is purely CROSS-DSU cpu0->worker visibility.
 - static pre-bringup data OK: 0x51000024=0x57a70ded (producer-offload viable).
CONCLUSION (hardware-proven): the dynamic cross-DSU coherency fix is NOT achievable from NS - the critical
registers are write-protected and the writable ones are not it; no clean coherency MMIO bit exists (staged
hotplug). Remaining options: (A) ATF patch (EL3 can write the protected regs, but we have NO confirmed coherency
target register - the staged hotplug found none - so this is a low-odds gamble on a464/718/748 being coherency),
or (B) the PRODUCER-OFFLOAD, which is fully consistent with the measured hardware behavior (worker reads static
pack data + worker->cpu0 writes both work) and needs NO coherency fix. Recommending B as the shippable win.

### PIVOT TO PRODUCER-OFFLOAD (2026-08-28): user chose it; design validated
Hardware verdict accepted: dynamic cross-DSU coherency is NS-unfixable. Building the producer-offload instead.
DESIGN (phase-validated):
 - build_cardcache renders the SETTLED strip (cont_shift=0). Card j position: cx = 640 + sel_world + CAR_HGAP*
   ring_delta(focus,j,n), CAR_HGAP=262 (int), ring_delta int. sel_world rests at CAR_SLOT_X=-393 and moves in
   262-steps -> sel_world is always INTEGER at settle -> every card sits at an INTEGER cx (sub-pixel phase 0).
   => per-card pre-rendered tiles blit EXACTLY (the earlier nearest-neighbor phase worry does NOT bite here).
 - WORKER (once at boot, static input only - which the HW confirms it can read): render each of the N game cards
   in NORMAL (non-focused) state - card frame + aspect-scaled boxart + player icon + resume dots (the draw_card
   body, minus focus/cursor) - into its own fixed tile buffer at canonical origin. Write tiles to DRAM, clean
   (worker->cpu0 direction WORKS on HW). Worker reads pack boxart from the static pre-bringup snapshot (proven:
   w_static_can=0x57A70DED).
 - cpu0 build_cardcache: for each non-focused card, BLIT its pre-rendered tile at (cx, CAR_CY) instead of calling
   draw_card (which does the expensive boxart min-filter scale + several blits). Focused card stays on L3
   (draw_focus_card) unchanged. Tiles are static (normal cards never change) -> rendered ONCE, reused on EVERY
   rebuild -> removes the per-card render cost from every nav/settle rebuild (~the 13-30ms build hitch).
 - SAFETY/FALLBACK: gate on a worker-tiles-ready flag; until ready (or if bringup is skipped on stock tee), cpu0
   uses the existing draw_card path -> shipping menu unchanged. Host-validate: pre-rendered-tile strip must equal
   the draw_card strip pixel-for-pixel (integer phase => should be exact).
NEXT: implement worker tile-render entry + cpu0 blit path + host validation, behind a flag. Multi-cycle build.

### PRODUCER-OFFLOAD PHASE 1 DONE (2026-08-28): tile decomposition host-validated PIXEL-EXACT
Implemented + host-validated the correctness-critical piece: snes_menu_render_card_tile (renders one normal card
into a straight-alpha tile) + snes_menu_build_cardcache_tiled (blits tiles instead of draw_card) + snes_blit_raw
(composite a raw RGBA tile via the cache premult path). host_render "cardtile" mode compares the tiled strip vs
the direct build_cardcache strip: PASS 0 px differ at settled + nav=1,3,5,10 (native, non-resume). One bug found
+ fixed: the tile is written framebuffer-order (0xAARRGGBB) but snes_blit_raw reads it as an RGBA-byte texture,
so R/B were swapped - fixed by swapping R<->B in the tile once after cache_unpremult. So the decomposition is a
proven correctness no-op: once the worker fills the tiles, cpu0's tiled build is bit-identical to today's menu.
NEXT (phase 2): wire the worker to render the 21 tiles at boot from the static pack (worker->cpu0 writes work on
HW), cpu0 uses build_cardcache_tiled gated on tiles-ready && native && !resume, fallback to draw_card otherwise.

### PRODUCER-OFFLOAD PHASE 2a (2026-08-28): cpu0-pre-render tiles wired, on-device image staged
Wired the tiled path into the live driver behind AYANEO_CARDTILES (snes_driver.c build_cardcache call site):
on a native, non-resume strip rebuild, cpu0 renders the N normal-card tiles ONCE (SNES_CARDTILES_PA=0x52500000,
~8.17MB in the free gap after the wallpaper) then uses snes_menu_build_cardcache_tiled (cheap tile blits) instead
of build_cardcache (per-card boxart min-filter render). Static tiles -> rendered once, reused on every later nav
=> removes the per-card render from the nav-rebuild hitch. 4:3/resume fall back to draw_card (exactness only holds
at integer positions). This is SINGLE-CORE (no worker yet) but already the real win, and it de-risks the whole
path on-device. Staged lk_a_snes_cardtiles_signed.img. NEXT phase 2b: move the one-time tile render to the WORKER
(cpu1) so cpu0 pays nothing (worker->cpu0 writes proven on HW); until then cpu0 pays the ~one-time render on the
first native nav. Host-validated pixel-exact (cardtile mode, 0 px differ at settle + nav 1/3/5/10).

### PRODUCER-OFFLOAD 4:3 GENERALIZED (2026-08-28): device runs 4:3, offload now covers it
CRITICAL: the panel is 1280x960 so the menu is ALWAYS in 4:3 (s_menu.aspect=1, want=(H>=960)) - the native-only
offload would have done NOTHING on-device. Generalized the tile path to 4:3: render_card_tile bakes the current
view scale (S=ASP_CONTENT_S) into the tile centred; draw_carousel_tiled composites at the VIEW_CONTENT-transformed
positions (S*cx+vdx, S*CAR_CY+vdy) into a vsx=1 target (tile carries the scale). Fixed a native regression (used
VIEW_CONTENT's vdy=480-S*360=120 for native, but set_view is identity when !aspect -> gate vdx/vdy on aspect).
Host results (cardtile mode, both aspects, nav 0/3/7): native aspect=0 = PASS 0 px differ (bit-exact); 4:3
aspect=1 = ~53k px differ but that is pixel-art boxart texels shifted <=1px - PPM dump (ref vs tiled, sent to
user) shows the two strips VISUALLY IDENTICAL. So enabled the offload in 4:3 (removed the !aspect gate; keep
non-resume; tiles invalidated on aspect change). Tile bumped to 320x360 to fit the 1.185x-scaled card; buffer
21*320*360*4 = 9.68MB at 0x52500000 (fits the gap). Restaged lk_a_snes_cardtiles_signed.img - now actually
active on the 4:3 device. NEXT: user flashes to confirm smoother nav + visual parity; then phase 2b (worker
renders the tiles) to move the one-time cost off cpu0.

### PHASE 2b PLAN (2026-08-28): worker renders the tiles (the literal goal - cpu1 works during LK)
Phase 2a is complete + on-device (cpu0 renders tiles once, tiled builds thereafter; 4:3-active, host-validated).
Phase 2b moves the one-time tile render to cpu1, fulfilling the original objective. It reuses the proven-on-HW
facts: worker reads STATIC pre-bringup data (w_static_can=0x57A70DED) + worker->cpu0 writes work (canary2 0x7777).
DESIGN (careful, fallback-safe):
 1. cpu0 BEFORE bringup: clean the whole render input to DRAM - s_menu struct, the pack (already in DRAM at
    0x50000000), and publish the menu pointer + tiles base + ngames at a STATIC comms location (line 0, cleaned
    pre-MMU) so the worker reads them as static data (not the dynamic menu_ptr that faults).
 2. worker (new TILE job in bc_worker_entry, gated): read the static menu ptr; for gi in ngames render
    snes_menu_render_card_tile(menu, gi, tiles+gi*tsz); clean each tile; publish a done counter + a checksum.
 3. cpu0: bounded-wait for done; verify the checksum (or tile non-zero); if good -> s_tiles_ready=1 (use worker's
    tiles); if timeout/bad -> cpu0 renders them itself (Phase 2a fallback). So a worker crash/hang just boots the
    normal menu.
DE-RISK FIRST (recommended next build): a NON-critical variant - cpu0 always renders its own tiles (2a), AND the
worker independently renders tile[0] into a scratch buffer + publishes its checksum; cpu0 logs WORKER-TILE
MATCH/MISMATCH vs its own. Proves the worker can execute the render tree (the real risk) WITHOUT the menu
depending on it. Needs the tee_patched_armcpc.img prereq (bringup). Gate: AYANEO_CARDTILES_WORKER (implies
BIGCORE_EXPT+CARDTILES). Perf value is modest (one-time ~20ms off cpu0) but it is the stated goal realized.

### ON-DEVICE RESULT: PRODUCER-OFFLOAD TILES ARE SLOWER (2026-08-28) - REVERT
User flashed lk_a_snes_cardtiles_signed.img: 60fps idle (OVL path unchanged, good) but 30/sub-30 + flicker/
tearing on MOVEMENT. Root cause (confirmed in snes_render.c): the fast blit NEON path (line 162) is RGB565-ONLY;
the pack card frames/boxart ARE rgb565, so the original draw_card renders cards via NEON (8px/iter). The
pre-rendered tiles carry an alpha channel (needed for cache_layer source-over) so they are RGBA8888 -> the blit
falls to the SCALAR path (~8x slower/pixel). So build_cardcache_tiled is ~slower than build_cardcache, and the
per-nav strip rebuild misses the vsync window -> 30fps + tearing on held movement.
DEEPER: even fixing the NEON format would only reach PARITY, not a win - the card render is memory-bandwidth-
bound (writing ~the same pixels either way); the only thing offloaded was the boxart scale-COMPUTE, which is
negligible vs the pixel writes. And Phase 2b (worker) cannot help the per-nav rebuild either: that rebuild is
SYNCHRONOUS (cpu0 needs the strip immediately on a nav), so an async cpu1 can only do the ONE-TIME initial build.
CONCLUSION: the producer-offload via RGBA tiles does NOT improve menu perf and regresses movement. REVERTED
(AYANEO_CARDTILES defaults off; ship image lk_a_snes_signed.img is clean). The movement hitch is the inherent
synchronous per-nav strip rebuild - a separate menu-perf problem, not a multicore one. Honest dead-end for the
tile offload. If movement smoothness matters, the real levers are: cheaper rebuild (clear only dirty region /
smaller L2), or a wider strip cache that avoids per-nav rebuilds (needs >2496px L2). Not multicore.

### REAL REGRESSION FOUND (2026-08-29): OVL 60fps present tears on MOVEMENT, not the tile offload
User clarified: the single-core baseline was solid 30fps with NO tearing; the tearing/stutter began with the
multicore/OVL 60fps work. Root cause (mt_disp_drv.c + snes_driver.c): the LAYERED (OVL) present skips the explicit
post-swap vsync wait when the frame is FAST (<15ms) to hit 60fps. But a smooth carousel PAN frame IS fast (the OVL
hardware does the panning), so it skipped vsync during slides, and the OVL src_x pan reconfig then latches
MID-SCANOUT -> visible tearing on movement. Idle didn't tear (static content). FIX: never skip vsync while the
strip is MOVING (l2_pan!=0 || rebuilt || cont_shift!=0); skip only when truly static (idle), where a mid-scanout
latch is invisible and 60fps is free. Tradeoff: pans sync to vblank (tear-free; may cap at 60/30 depending on the
double-barrier), idle stays 60fps. Staged lk_a_snes_notear_signed.img (clean, cardtiles OFF). This is the actual
fix the user needs; the producer-offload tile work is a closed dead-end (reverted).

### TEARING FIX - TWO STAGED VARIANTS (2026-08-29)
Awaiting user test. Two images for the OVL-present tearing regression:
 1. lk_a_snes_notear_signed.img (targeted): skip vsync ONLY at true idle; sync during movement (l2_pan/rebuilt/
    cont_shift). Keeps 60fps idle, tear-free movement (movement may cap 60/30 via the double barrier). Try first.
 2. lk_a_snes_notear_safe_signed.img (AYANEO_ALWAYS_VSYNC): ignore the skip hint, ALWAYS wait vblank after every
    swap. Guaranteed tear-free (exactly the old single-core no-tear behaviour); may cap movement at 30fps.
    Fallback if #1 still shows any tearing.
If #1 is clean -> ship it (best of both). If #1 still tears in some case -> #2 is the guaranteed hammer, then the
proper 60fps-tear-free path is a hardware vblank-latched OVL layer reconfig (config at vblank, single barrier).

### TEARING MECHANISM RE (2026-08-29): CMDQ disabled -> OVL config is DIRECT, timing-sensitive
Studied the present path. primary_display_use_cmdq = CMDQ_DISABLE in LK, so OVL layer config is written DIRECTLY
to hardware (not SOF-latched via CMDQ). config_input_multi waits DISP_PATH_EVENT_FRAME_DONE (current frame's
scanout end) then writes the OVL registers during the back-porch. Tear-free requires that write to finish before
the next SOF. The two staged variants pace the LOOP via a post-swap vsync wait (variant1: only when moving;
variant2 AYANEO_ALWAYS_VSYNC: every swap) - they reduce tearing by slowing the cadence but do not directly time
the config to vblank. THEORETICALLY-CORRECT variant 3 (if 1/2 are insufficient): wait for vsync/FRAME_DONE
BEFORE config_input_multi so the OVL reconfig lands right at the start of the back-porch with a full frame of
margin before the next SOF - tear-free AND 60fps (one barrier). Not built yet: holding for the user's test of
variants 1/2 to avoid flooding untested images. If both still tear -> build variant 3 (pre-config vblank wait in
ayaneo_canvas_present_layers). NOTE: this display-tearing work is the user's actual priority now, distinct from
the (closed) multicore coherency research.

### TEARING FIXED / TRANSITION FLICKER REMAINS (2026-08-29, user tested)
User: notear + safe variants BOTH fix carousel-movement tearing (idle 60fps, movement 30fps, no tear). Remaining:
slight FLICKER on the layered<->single-buffer transition - moving the cursor from the game list to the TOP
(menubar/Display) or BOTTOM (suspend-point list), enter AND exit. It survives always-vsync => NOT a timing issue;
it is a CONTENT/ORDERING glitch on the OVL-layered <-> single-buffer switch (present_layers disables L2/L3 vs the
single-buffer L0; the card strip and the cursor hand off between the OVL layers and the full L0 render during the
slide). Likely the L3-cursor <-> L0-cursor handoff or an L2-still-enabled 1-frame overlap. Built
lk_a_snes_notear_debug.img (no-tear fix + AYANEO_DEBUG_LOGGING) which emits, for 8 frames around each transition,
"SNESX lay=%d st=%d openy=%d rend=..us pres=..us tot=..us" + the present phase breakdown. Ask the user to trigger
the flicker and paste those lines - the per-frame rend/present timing + lay/state across the boundary pinpoints
whether a frame overruns or a layer toggles a frame late. Then fix the handoff directly.

### PRESENT FPS + FLICKER FIX v2 (2026-08-29): single-barrier vsync + symmetric transition handoff
User on the no-tear image: carousel tearing gone, BUT scrolling the games list dropped to 15fps (was 30) and the
suspend list to 20 - because the no-tear fix waited vsync AFTER the swap, stacking on config_input_multi's
FRAME_DONE wait = TWO barriers = half rate. FIX: single barrier. Added ayaneo_present_presync_vsync() - for
moving/pan frames, wait vblank BEFORE writing the OVL config (config latches at the frame boundary, tear-free)
and DO NOT post-wait; config_input_multi's FRAME_DONE is then a near-no-op. One barrier -> ~30fps tear-free scroll
(restores the original) while idle stays 60fps (skip both, static = invisible latch).
Also fixed the transition flicker symmetrically (CMDQ disabled -> layer writes non-atomic): ENTER - ovl_split now
gated on steady layered (layered && s_was_layered), so the entering frame draws cards+cursor in L0 too, matching
L2 (invisible overlap) - nothing flashes as L2 enables; LEAVE - keep the L2 card layer up ONE extra frame (L0
already carries the cards) then disable, so nothing vanishes. Staged lk_a_snes_notear2_signed.img. Awaiting test:
expect scroll ~30fps tear-free + no enter/exit flicker. (Suspend-list 20fps is the single-buffer resume render
cost - separate, revisit if still an issue.)

### PRESENT v2 HARDENED + FALLBACK PLAN (2026-08-29)
Hardened lk_a_snes_notear2_signed.img (clear s_leave_pending on re-enter). Awaiting user test.
RISK on the presync (single-barrier) FPS fix: it waits vblank before config, but if config_input_multi's own
FRAME_DONE wait still fires (path busy at that instant) it could still double-barrier (15fps), OR if the OVL
config write still crosses SOF it could still tear. It is a reasoned bet, not certain. FALLBACK if v2 is still
15fps or still tears on scroll: revert the SCROLL to the original single-buffer full-render path (make `layered`
require cont_shift==0 so only true idle uses the OVL; scrolling uses the pre-OVL single-buffer path that was
30fps tear-free originally), relying on the transition-handoff fix for the idle<->scroll layer toggles. That is
the most reliable route to the user's known-good 30fps-no-tear scroll, at the cost of dropping the OVL 60fps
scroll ambition. Decide based on the v2 result.

### TWO IMAGES STAGED FOR ONE TEST SESSION (2026-08-29)
To save a round-trip, staged BOTH:
 - lk_a_snes_notear2_signed.img: presync single-barrier (targets 30fps tear-free scroll) + transition handoff.
 - lk_a_snes_sbscroll_signed.img (AYANEO_SB_SCROLL): OVL only for true idle; SCROLL uses the original
   single-buffer full render (the known-good 30fps tear-free path before OVL). Idle 60fps. + transition handoff.
Test v2 first. If v2 scroll is 30fps tear-free -> ship v2 (keeps the OVL scroll path). If v2 is still 15fps or
tears -> flash sbscroll (guaranteed-original scroll behaviour). Both include the enter/exit flicker handoff fix.
Fallback risk: sbscroll toggles layered<->single on every scroll start/stop, so it leans harder on the handoff
fix; if the toggle itself flickers, we keep the OVL scroll and instead chase the presync tearing directly.

### PRESENT v3 - BACK TO TEAR-FREE (2026-08-29): presync + sb-scroll both tore, reverted
User: both notear2 (presync) and sbscroll TORE a lot on menu movement. Diagnosis: (1) presync (pre-config vblank
wait) does NOT prevent tearing on this HW - only the POST-swap vblank wait does; (2) my enter-overlap
(ovl_split = layered && s_was_layered) made the ENTER transition frame do a heavy full-L0 render + OVL layer
enable, but it was not forced to vsync -> tore on every carousel enter (= "lots of tearing on menu movements").
FIX (v3, lk_a_snes_notear3_signed.img): presync disabled; post-swap vblank sync on moving (pan/rebuild) AND on
every enter/leave TRANSITION frame; skip only steady idle. Keeps the enter/leave flicker handoff. Expected:
tear-free + no enter/exit flicker, 60fps idle, ~15fps scroll (the tear-free price of the OVL double-barrier).
The 15fps scroll is the honest ceiling of this approach; getting to 30fps tear-free requires cutting the moving-
frame CPU cost (the L3 cursor re-render is ~9ms/frame) so render+one-barrier fits 2 vsyncs - a later optimization,
NOT another vsync-timing change (those tore). notear2/sbscroll are superseded; use notear3.
