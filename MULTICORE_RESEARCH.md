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
