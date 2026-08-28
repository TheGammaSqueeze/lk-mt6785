# Operator flash guide (multicore-at-LK research)

The autonomous non-flash investigation is complete. The coherent-bringup path is an exhaustively
characterized DSU hardware wall; the remaining questions need ONE flash. All older diagnostic images are
superseded by a single comprehensive one.

## Flash THIS (one image, decides everything)

Prereq (once): `tee_patched_armcpc.img` on the `tee` partition (arms the CPC so PSCI CPU_ON completes).

Then flash `lk_a_snes_bigcore_cpcbit29.img` to `lk_a`, boot into the SNES menu, capture UART, and paste
these lines back:

1. `BC CANARY (non-comms 0x51000000): cpu0->worker worker-read=0x...`
   - `0xCA5Axxxx` => CPC_FLOW bit29 fixed coherency! (unlikely, but if so the whole 2-core split unlocks)
   - `0xa86dbdec` => still frozen (expected); go to the CPCDUMP diff.

2. `BC CPCDUMP 0x0c53a??? = 0x...` (several lines)
   - I diff these against the live coherent reference (tools/live_regpoke/live_cpc_reference.txt). Any CPC
     register that differs coherent-vs-LK beyond bit29 is a fresh coherency-fix candidate.

3. `BC STATICPROBE: worker read of PRE-bringup static 0x51000024 = 0x...`
   - `0x57A70DED` => the worker CAN read static pre-cleaned data => the producer-offload fallback is
     VIABLE (worker builds card tiles from static assets, cpu0 reads them via the proven worker->cpu0
     direction; no cross-core coherency needed).
   - garbage => the offload is blocked too; the worker can only use data it wrote itself.

## What I do with the results

- CANARY flips or CPCDUMP shows a real coherency-control difference -> wire the already-host-validated
  2-core render + cardcache splits (the real "60fps at lower power" win).
- Only STATICPROBE is green -> if you want it, I implement the low-risk producer-offload (per-system
  per-focus card tiles; removes the ~30ms scroll-rebuild hitch). Note: this is a real feature build with
  some visual-correctness care (the L2 card cache is focus-specific), so it is your call, not automatic.
- Nothing green -> multicore-at-LK is a hardware wall for this workload; restore the clean single-core
  build (lk_a_snes_signed.img) and close the effort. The whole investigation is in MULTICORE_RESEARCH.md.

## Notes
- The EXPT builds intentionally run the menu slower (fork/join + fallback spin every frame). The clean
  release build lk_a_snes_signed.img is unaffected; reflash it to get the normal menu back.
- Live register-poke module + build steps: tools/live_regpoke/README.md.
- Other staged images (probe/nonshare/mmuoff/ackprobe/dvm) are superseded; ignore them. The dvm image is
  the only separate experiment worth a second flash IF you want to try the DVM-broadcast long shot.
