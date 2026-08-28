# Operator flash guide (multicore-at-LK research)

The offline investigation is saturated: WHERE the wall is is settled; the exact register needs ONE on-device
test. This supersedes the older "DSU hardware wall / CPC bit29 / warmcycle" guide. Full reasoning:
MULTICORE_RESEARCH.md (read the LATEST entries - the early MCSI-specific cycles were later corrected, see below).

## The one-paragraph why (current understanding)

cpu0 (LK boot core) is mpidr 0x000 = physical DSU0. The worker is mpidr 0x100 = cpu4 = physical DSU1 (MT6785 is
TWO physical DSU clusters by MPIDR aff1: DSU0={cpu0-3}, DSU1={cpu4-7}; the 6/2 cpu-map split is a DVFS view, not
the coherency boundary). Cross-DSU coherency is admitted by the CPC/DSU hardware on cluster power-on. At LK that
admission does not happen for DSU1, so the worker is powered (SPM/SPMC ack bit31 set, verified) but NOT in the
snoop domain: its loads miss cpu0's data (which sits at the in-domain PoC = shared L3, not DRAM) while its stores
still drain. Every other candidate is ruled out: SPM/CPC power regs, CPUECTLR, AArch32, cache set/way,
SSPM/MCUPM firmware, EMI MPU. IMPORTANT CORRECTION: disassembly of our tee.img shows this ATF has NO software
MCSI driver and does NOT implement the MCSI SIP (the 0x28x SIP block is absent), so the earlier MCSI-SNOOP_EN-via
-SMC fix CANNOT work here - the admission is a CPC/DSU-hardware step. The real fix target is a CPC/mcucfg register
(or an ATF patch), to be pinned by the mcucfg hotplug-diff. The MCSI images below are now DIAGNOSTIC-ONLY.

## Prereq (once)

`tee_patched_armcpc.img` on the `tee` partition (arms the CPC so PSCI CPU_ON completes; without it LK skips the
worker bringup and is safe on stock too).

## THE decisive step (no flash needed - just adb): run_mcucfg_diff.sh

The single most informative test. On the live rooted device:
  cd tools/live_regpoke && ./run_mcucfg_diff.sh 0123456789ABCDEF
It powers DSU1 (cpu4-7) fully OFF then ON and nonzero-diffs mcucfg(0x0c530000,64KB)+mcucci(0x0c510000,4KB). Any
register that changes BEYOND the per-cpu power bits is the cross-DSU coherency-admission control = the fix target
(LK then pokes it, or an ATF patch sets it). Needs regpoke.ko to load (it built against /work/mt6785_kernel_source;
if insmod fails on CRC, that is the Module.symvers issue - extract symvers from the live kernel, see the log).

## Flash order (diagnostic value; the SMC "fix" will NOT take on this tee.img - see the why)

FLASH 1 - DIAGNOSIS: `lk_a_snes_mcsi_signed.img`
  Boot the SNES menu, capture UART. Read the "BC MCSI RAW ..." lines (direct mcucci read) and "BC MCSI SIP-LIVE".
  Expect SIP-LIVE=NO (SMC path absent) and the RAW mcucci read likely all-zero (MCSI block unused on this SoC) -
  both CONFIRM the fix is not MCSI/SMC but CPC/mcucfg. Use run_mcucfg_diff.sh above for the actual register.

FLASH 2 - `lk_a_snes_mcsifix_signed.img`
  Same diagnosis + attempts the MCSI SMC admit; on THIS tee.img the SMC returns -1 so SNOOP_EN will not change
  (expected). Kept for a future MCSI-enabled ATF. Not the working fix on the current tee.

RESTORE anytime: `lk_a_snes_signed.img` (clean single-core shipping menu).

## UART decision tree (paste these lines back)

1. SIP liveness (DISASM of tee_patched_armcpc.img shows the 0x28x SIP block is NOT compiled into this ATF):
   `BC MCSI SIP-LIVE: NO - SIP absent (expected on stock/CPC tee per disasm) (CENTRAL_CTRL=0xffffffff)`
     - "NO ..." is EXPECTED on the current tee.img. It means the SMC-based admit cannot work here; the real fix
       is an ATF binary patch (see below). Do NOT conclude the theory is wrong from this line - use the RAW read.
     - "YES ..." would only appear on a future MCSI-enabled ATF (then the SMC fix path is live too).

2. Snoop state (the smoking gun) - RAW direct read of mcucci @0x0c510000 (works without the SIP):
   `BC MCSI RAW SLV<n> SNOOP_CTRL=0x... SNOOP_EN=? DVM_EN=? SNP_SUP=? DVM_SUP=?`  (8 lines) + the RAW CENTRAL line
     - If the RAW reads return real values (not 0x00000000 / 0xffffffff): EXPECT cpu0's cluster iface SNOOP_EN=1
       and the worker cluster iface SNP_SUP=1, SNOOP_EN=0. That difference CONFIRMS snoop-admission is the wall,
       ON METAL, and tells us exactly which slave-iface index the ATF patch must enable.
     - If the RAW reads are all 0x00000000 => NS reads of mcucci are also firewalled; we then rely purely on the
       ATF patch + the coherency canary to judge success.
     - The `BC MCSI SLV<n>` (SIP) lines will read 0xffffffff on this tee.img - ignore them; use the RAW lines.

3. Fix took (FLASH 2 only) - only meaningful on an MCSI-enabled ATF:
   `BC MCSI FIX: SLV<n> after set SNOOP_CTRL=0x... SNOOP_EN=1 DVM_EN=1 (settled in N us)`  (on this tee.img the
   set returns -1 and this will not change SNOOP_EN; that is expected - the fix belongs in the ATF patch).

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
