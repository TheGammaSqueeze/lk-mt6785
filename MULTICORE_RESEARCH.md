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

### HOLDING STATE
The decisive gate remains one HW flash of the two staged images. Remaining readiness tasks are
genuinely blocked: Lever 4 producer wiring and Lever 3 0x600 bringup both need the probe result
(coherency mechanism + CPC_SPMC_ST) before they can be built correctly; the non-shareable-walk
refinement is low-value (the worker PTW already works). Further no-HW cycles will hold rather than
churn. On HW input, execute the OPERATOR NOTES decision tree.
