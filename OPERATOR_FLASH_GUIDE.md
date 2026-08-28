# Operator flash guide (multicore-at-LK research)

The offline investigation is complete and points to a SPECIFIC, FIXABLE root cause with a candidate fix
already built. This supersedes the older "DSU hardware wall / CPC bit29 / warmcycle" guide (that framing was
retired once the real mechanism was found). Full reasoning: MULTICORE_RESEARCH.md.

## The one-paragraph why

cpu0 (the LK boot core) is mpidr 0x000 = CLUSTER 0. The worker is brought up at mpidr 0x100 = cpu4 =
CLUSTER 1. MT6785 is a TWO-cluster DynamIQ SoC; the clusters are kept coherent by MCSI (Mediatek Cache Snoop
Interconnect). MCSI cross-cluster coherency is enabled in ATF's PSCI on_finish ONLY when it believes the
cluster transitioned from OFF, and is normally SSPM/MCUPM-driven. At LK there is no SSPM runtime and LK's PSCI
is minimal, so cluster 1's MCSI snoop-enable is SKIPPED: the worker is powered (SPM ack bit31 set) but NOT
admitted to the snoop domain, so its loads miss cpu0's dirty lines while its stores still drain. Every other
candidate (SPM/CPC power regs, CPUECTLR, AArch32, cache set/way, SSPM/MCUPM firmware, EMI MPU) has been ruled
out. The fix: from LK, cpu0 sets SNOOP_EN|DVM_EN on cluster 1's MCSI slave interface via the ATF SIP
MCSI_NS_ACCESS (SMC 0x8200028B). This is reachable, ABI-verified, and matches ATF's cci_enable_cluster_coherency.

## Prereq (once)

`tee_patched_armcpc.img` on the `tee` partition (arms the CPC so PSCI CPU_ON completes; without it LK skips the
worker bringup and is safe on stock too).

## Flash order (two images; both keep the shipping menu behind a flag)

FLASH 1 - DIAGNOSIS: `lk_a_snes_mcsi_signed.img`
  Read-only. Confirms the mechanism and that the ATF SIP layer exists. Boot into the SNES menu, capture UART.

FLASH 2 - FIX:       `lk_a_snes_mcsifix_signed.img`
  Same diagnosis, then ADMITS the worker cluster (SNOOP_EN|DVM_EN, with SNP_PENDING drain) + a DVM/TLBI lever,
  then re-checks coherency, then the per-frame 2-core split runs and self-reports.

RESTORE anytime: `lk_a_snes_signed.img` (clean single-core shipping menu).

## UART decision tree (paste these lines back)

1. SIP liveness (judged from MCSI_NS_ACCESS itself - binary analysis of tee_patched_armcpc.img confirms
   0x8200028B IS present in the ATF while MCUSYS_ACCESS_COUNT/FLUSH_BY_SF are NOT, so we probe the fix's own SIP):
   `BC MCSI SIP-LIVE: YES - MCSI_NS_ACCESS handled (CENTRAL_CTRL=0x...)`
     - "YES ..."  => the fix's SMC path is live; the MCSI reads/admit below are valid (expected on this tee.img).
     - "NO - SIP absent" (CENTRAL_CTRL=0xffffffff) => MCSI_NS_ACCESS not implemented; fall to the ATF-patch
       contingency (cci_enable at MCSI base 0x0c510000). (Note: the FLUSH_BY_SF line returning 0xffffffff is
       EXPECTED and harmless - that SIP is not in this ATF; the TLBIALLIS lever still runs.)

2. Snoop state (the smoking gun), from FLASH 1 or 2:
   `BC MCSI SLV<n> SNOOP_CTRL(0x1n00)=0x... SNOOP_EN=? DVM_EN=? SNP_SUP=? DVM_SUP=?`  (8 lines)
     - EXPECT: cpu0's cluster iface shows SNOOP_EN=1; the worker cluster iface shows SNP_SUP=1, SNOOP_EN=0.
       That difference CONFIRMS snoop-admission is the wall.
     - If ALL snoop-capable ifaces already show SNOOP_EN=1 => MCSI is NOT the wall; pivot (see log tail).

3. Fix took (FLASH 2 only):
   `BC MCSI FIX: SLV<n> after set SNOOP_CTRL=0x... SNOOP_EN=1 DVM_EN=1 (settled in N us)`

4. Coherency verdict (FLASH 2):
   `BC MCSI post: w_can1=0x... w_menuw0=0x... w_static_can=0x...`
     - flips to real values / 0xCA5A  => the worker now reads cpu0's data = COHERENT = wall broken.
   `BC MCSI post-DVM(TLBIALLIS): ...`
     - if it flips only HERE (not after the MCSI set) => the wall was DVM-sync, not admission.

5. The payoff, in the running menu (FLASH 2):
   `... full split channel LIVE`  => the 2-core render split is genuinely live (the 60fps-lower-power goal).
   `... mismatch/frozen`          => fix insufficient; capture all of the above and I will iterate.

## What to paste back

The MCUSYS_ACCESS_COUNT line, all 8 `BC MCSI SLV<n>` lines, the `BC MCSI FIX` lines, both `BC MCSI post` /
`post-DVM` lines, and whether the menu logged `full split channel LIVE`. That fully determines the next step.

## Notes
- The EXPT builds intentionally add the bringup + per-frame fork/join; the clean release `lk_a_snes_signed.img`
  is unaffected, reflash it for the normal menu.
- Live register-poke / secure-SMC tooling + build steps: tools/live_regpoke/ (regpoke.ko, smcpoke.ko,
  run_mcsi_diff.sh). MCSI reference source: tools/mcsi_ref/ (ARM ATF mt8183 mcsi.c/.h).
- Older staged images (probe/nonshare/mmuoff/ackprobe/cpcbit29/warmcycle/sgi) are superseded; ignore them.
