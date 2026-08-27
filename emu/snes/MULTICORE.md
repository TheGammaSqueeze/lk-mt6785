# Bringing up a second CPU core inside LK (MT6785 / AYANEO Pocket Air Mini)

This documents how we got a **second CPU core running while the LK bootloader SNES
menu is live** on the MT6785 (Helio G95, 6× Cortex-A55 + 2× Cortex-A76/A75), and
the **exact changes** required. The goal is to later split the menu render across
cores so we can hit 60 fps at lower clocks (= lower power/heat).

TL;DR of the fix: **LK cannot power a core itself** (it runs non-secure and the
CPU-power registers are firewalled), and **PSCI `CPU_ON` from LK hangs** because the
SoC's ATF/BL31 only arms its CPU-power sequencer lazily, at kernel handoff. We patch
**one 4-byte instruction in `tee.img` (BL31)** to arm that sequencer at cold boot,
re-sign the TEE, and then a plain `PSCI CPU_ON` from LK works.

---

## 1. Why it did not work out of the box

Observed: `PSCI CPU_ON(0x84000003, mpidr=0x100)` from LK froze the device (un-timed
ack spin inside ATF). Reverse-engineering the device BL31 (`tee.img`) and the flashed
kernel/DTB established:

1. **LK runs NON-SECURE (BL33), below a resident EL3 ATF monitor.** Proof: LK reaches
   every privileged op via `smc #0` (`app/mt_boot/mt_secure_call.c`), consumes ATF SiP
   services (`0x82000101` DAPC, `0x82000103` RPMB, `0x82000115` KERNEL_BOOT…), and
   never loads/jumps `tee.img` itself. A thing at EL3 cannot SMC upward, so LK is below it.

2. **The CPU-power registers are secure-only; NS LK writes are silently dropped.**
   On-HW read-after-write map (from `bigcore.c`'s probe): all of MCUCFG
   (`0x0C53_xxxx` - boot-vector `0x0C53C900+cpu*8`, `INITARCH 0x0C53C8E4`,
   `CPC_FLOW 0x0C53A814`) and the SPM unlock (`0x10006000`) read back **unchanged**
   after writes (DROP), while reads work. So a from-LK raw-MMIO bring-up is impossible.

3. **The SPMC/CPC "arming" (`spmc_init`) is deferred and not done at cold boot.**
   In this device's BL31, `spmc_init` (the code that sets `CPC_CTRL_ENABLE`, unlocks
   the SPM CPU banks, and de-asserts `PWR_RST_B` on the off cores) runs **lazily, once,
   as the first action inside the `MTK_SIP_KERNEL_BOOT` (`0x82000115`) handler**, i.e.
   at the moment LK hands off to the kernel. The kernel therefore gets an armed CPC
   "for free"; LK-at-menu-time does not, so ATF's `spm_poweron_cpu` ack never fires
   and its un-timed poll spins forever.

We confirmed (3) on hardware: calling `MTK_SIP_KERNEL_BOOT` from LK printed
`SPM: enable CPC mode` (that log line lives inside `spmc_init`) - the arming *ran* -
but the handler then jumped to the (bogus) entry and hung, so we cannot "arm and
return" from LK via that SiP.

The kernel's device tree confirms there is nothing MTK-specific to copy: every CPU is
`enable-method = "psci"`, `method = "smc"`. MPIDRs: `cpu0=0x000, cpu1=0x100 … cpu6=0x600,
cpu7=0x700` (cpu6/7 are the big A76/A75). So `0x100` = cpu1, a little A55 sibling.

---

## 2. The fix - exact changes

### 2.1 ATF / `tee.img` (BL31): one 4-byte patch + re-sign

Arm the CPC at **cold boot** (before LK) by redirecting a dead debug-print call in
ATF's cold-boot init function into the one-shot arming wrapper.

- **Disasm addresses** (device BL31; `tee.img` file offset = disasm addr + `0x400`,
  since the code payload starts at file `0x400`):
  - `0x91e8` - cold-boot platform init, called exactly once from `bl31_main` at `0x4654`
    (CPU0, before the BL33/LK handoff, before any `spm_poweron_cpu`).
  - `0x9288` - a `bl 0x71f4` (a NOTICE varargs print) inside `0x91e8`. Its result is
    unused and only `w19` (callee-saved) is live afterwards.
  - `0x92d0` - the arming wrapper: one-shot-guarded by a BSS byte flag at `0x2e000+4080`;
    when unset it runs `bl 0x9770; 0x971c; 0x99d4; spmc_init(0x125f4)` then returns.

- **The edit** (single instruction):

  | tee.img offset | original 4 bytes | new 4 bytes | meaning |
  |---|---|---|---|
  | `0x9688` | `db f7 ff 97` (`bl 0x71f4`) | `12 00 00 94` (`bl 0x92d0`) | run the arming at cold boot |

  BL encoding check: from `0x9288`, `bl 0x92d0` → `word = 0x94000000 | (((0x92d0-0x9288)>>2) & 0x03FFFFFF) = 0x94000012` → little-endian `12 00 00 94`.

- **Why it's safe:** `0x92d0`'s one-shot flag means the later *real* `MTK_SIP_KERNEL_BOOT`
  → `0x92d0` call (at `0x323c`) sees the flag set and no-ops its body, then still performs
  its kernel jump - so **normal Linux boot is unaffected**. `spmc_init` only touches the
  off cores (cpu1..7 `PWR_RST_B` at `0x1000620c..0x10006224`), idempotent `RESETPWRON`
  clears, and `CPC_CTRL_ENABLE` - no hazard to the running cpu0. `INITARCH 0x0C53C8E4` is
  written by `0x9770` at cold boot (correct place); we deliberately did **not** inject
  inside `spm_poweron_cpu`, because that would rewrite `INITARCH` after `plat_power_domain_on`
  set the target core's arch-state.

- **Re-sign** (the TEE is verified by the preloader via an X.509 `cert2` chain that uses
  the **same MTK test key** as LK - `keys/img_prvk.pem`, RSA-PSS/SHA-256). Use
  `tools/ayaneo/sign_lk.py`'s `resign()` **directly** (NOT `main()`/`graft()`, which are
  LK-layout-specific and would pad/corrupt the 166512-byte image):

  ```python
  import sign_lk as S
  d = bytearray(open("tee-verified.img","rb").read())     # stock, sha256 e30a8a38…
  assert bytes(d[0x9688:0x968c]) == bytes.fromhex("dbf7ff97")
  d[0x9688:0x968c] = bytes.fromhex("12000094")            # bl 0x71f4 -> bl 0x92d0
  out = S.resign(bytes(d))                                # re-hash [0x200:0x27c00] into cert2 + re-sign TBS
  assert len(out) == 166512
  open("tee_patched_armcpc.img","wb").write(out)
  ```

  `resign()` on `tee.img` finds `atf`(off 0, isz `0x27a00`) / `cert1` / `cert2`(off `0x284b0`),
  hashes `header=[0:0x200]` + `data=[0x200:0x27c00]` (the patch at `0x9688` is inside that
  span, so it is re-covered), rewrites the two MTK-OID hashes in cert2, and re-signs the
  cert2 TBS. There is **no independent inner signature** to worry about: the 256-byte
  high-entropy blob near `0x33c` inside the `EET KTM` header is signed *payload* (it lies
  inside `[0x200:0x27c00]`), not a separate signature. Verified on HW: the re-signed TEE
  is accepted by the preloader (device is not fuse-locked; `AYANEO_ROT lock=0`).

### 2.2 LK code changes

| File | Change |
|---|---|
| `emu/snes/bigcore.c` (new) | The whole experiment: MMIO helpers, the firewall read-after-write probe, the register map (all verified vs BL31 disasm), and `bigcore_start()` which reads `MCUCFG_CPC_FLOW_CTRL` and - gated on `CPC_CTRL_ENABLE` being set by the patched ATF - issues `PSCI CPU_ON(0x84000003, 0x100, &bc_entry_arm)` and samples the shared comms. All of it is behind `AYANEO_BIGCORE_EXPT` and compiled out otherwise. |
| `emu/snes/bigcore_entry.S` (new) | `bc_entry_arm`: a forced-ARM, 16-byte-aligned proof-of-life stub the secondary core boots into (MMU-off). Writes magic `0xB16C0DE5` then an unbounded incrementing counter to `0x54000000`, each with a `dsb`. |
| `emu/snes/snes_driver.c` | Calls `bigcore_start()`; adds the debug-only blue OSD line `BC m<mpidr> t<cpc> r<psci_ret> p<spm_status> g<magic?> c<counter>` (behind `AYANEO_DEBUG_LOGGING`), plus the `u2h`/`i2s` helpers. |
| `platform/mt6785/rules.mk` | Adds `emu/snes/bigcore.o` and `emu/snes/bigcore_entry.o` to `OBJS`. |
| `project/k85v1_64.mk` | Adds the `AYANEO_BIGCORE_EXPT` build flag (implies `AYANEO_DEBUG_LOGGING`) that gates the experiment. Also the earlier `AYANEO_VERBOSE_LOG` gate for per-frame display spam. |
| `build_ayaneo_snes.sh` | While the experiment is active, the *debug* image is built with `AYANEO_BIGCORE_EXPT=yes`. The *release* (signed) image is unchanged and always compiles the experiment out. |

Comms scratch is at `0x54000000` (`BC_COMMS_PA`) - a free 16 MB slot above the SNES asset
regions (blob `0x50000000`, wallpaper `0x52000000`, chrome `0x53000000`), so it never
collides with menu data. Because the core is brought up by PSCI it returns **non-secure**
(matching LK), so the comms are visible without any secure/NS gymnastics.

### 2.3 Register map (MT6785, all verified against the device BL31 disasm)

```
MCUCFG_BASE          0x0C530000
  BOOTADDR(cpu)      0x0C53C900 + cpu*8      (per-CPU reset vector)
  INITARCH           0x0C53C8E4              bit(16+cpu): 1=AArch64
  CPC_FLOW_CTRL      0x0C53A814              CPC_CTRL_ENABLE=bit16, SSPM_ALL_PWR_CTRL_EN=bit13
SPM_BASE             0x10006000
  POWERON_CONFIG_EN  0x10006000              unlock key 0x0B160001
  CPU_RSTCON(cpu)    0x10006208 + cpu*4      PWR_RST_B=bit0, RESETPWRON=bit5
  CPU_PWR_CON(cpu)   0x10006768 + cpu*4      PWR_ON=bit2
  CPU_PWR_STATUS     0x10006160              ack bit = (cpu<=3 ? cpu+9 : cpu+11)  -> cpu1 = bit10
```

---

## 3. Result (confirmed on hardware)

Flashing `tee_patched_armcpc.img` + the `AYANEO_BIGCORE_EXPT` debug LK, the OSD showed:

```
BC m0x81000000 t720896 r0 p0x474 g1 c<incrementing>
```

- `t=0xB0000` - `CPC_FLOW` bit16 set (patched ATF armed the CPC at cold boot; was `0xA0000`).
- `r0` - `PSCI CPU_ON(cpu1)` returned SUCCESS.
- `p=0x474` - `SPM_CPU_PWR_STATUS` bit10 set → **cpu1 powered on**.
- `g1` + climbing `c` - cpu1 is executing `bc_entry_arm` and writing the live counter.

**A second CPU core is alive and executing during the LK menu.**

---

## 4. Reproduce / flash

1. Build: `./build_ayaneo_snes.sh` → `out/lk_a_snes_debug.img` (experiment) and
   `out/lk_a_snes_signed.img` (clean release).
2. Patch + re-sign `tee.img` per §2.1 → `tee_patched_armcpc.img`.
3. Flash (BROM / SP Flash Tool): `tee` ← `tee_patched_armcpc.img`, `lk_a` ← `lk_a_snes_debug.img`.
   Keep the stock `tee-verified.img` as a one-flash rollback (device is BROM-unbrickable).
   Tip: flash a re-sign-of-stock control image first to prove the signing pipeline before
   trusting the patched one.

---

## 5. Phase 2 - multi-core render for a low-power 60 fps (implemented)

All behind `AYANEO_BIGCORE_EXPT`; the release build stays single-core and
byte-identical (verified with the `host_render` split harness).

- Cached+coherent worker (`bc_entry_arm2`, `bigcore_entry.S`): the PSCI-entered A55
  snapshots cpu0's live LPAE MMU config (`TTBR0` via `mrrc`, `TTBCR/MAIR0/MAIR1/DACR/
  SCTLR`) from the comms block, invalidates TLB/I-cache/branch-predictor, programs
  the translation registers and enables MMU + I/D caches. A55 SMP coherency is
  hardware/DSU managed (there is no software `SMPEN`), so once caches are on it is a
  coherent snoop participant. The `CPUECTLR` write in the device BL31 is MIDR-gated
  to the A76 and skipped on A55.
- Per-core render context (`snes_render.c`): the global z-sort/draw-list scratch
  became a per-core `snes_render_ctx`, selected by MPIDR (`bc_render_ctx`), so both
  cores run `snes_menu_render` concurrently without sharing mutable state.
- Scanline band split: `snes_target` gained a `band_y0/band_y1` clip honoured in
  `blit()` and in the three direct composites (`draw_wp_43/draw_wp/draw_chrome`). The
  two cores render disjoint 16px/64B-aligned bands of the same frame. Proven
  output-correct: `host_render <pack> <ppm> 40 <nav> split` reports IDENTICAL vs the
  whole-frame render for every state in 16:9 and 4:3.
- Per-frame fork/join (`snes_driver.c`): cpu0 runs `snes_menu_update`, posts the
  bottom band to the worker (`comms.go` + `SEV`), renders the top band itself, then
  joins on `comms.done` with a bounded-spin fallback (renders the worker's band
  itself if it misses the deadline, so a wedged worker never hangs the menu). Sync is
  `WFE/SEV` + `dmb/dsb ish`.
- Clock drop: once both cores render in parallel the little cluster (one shared
  PLL/buck) drops to `BC_MHZ` (1200, tunable) for lower dynamic power while holding
  60 fps; single-core fallback keeps 2000 MHz. Two cores at f/2 do the same work as
  one at f, so the frequency (and, where firmware allows, voltage) can come down.

On-HW validation (needs the patched tee): OSD `g1 k1` with `c` climbing = the worker
is up and rendering cached; the FPS HUD should hold >=60 at `BC_MHZ`. Tune `BC_MHZ`
down toward ~1075 for minimum power.

## 6. Phase 2 debug - cross-core coherency is not working, and the workaround

On HW the worker comes up cached (`g1 k1`) but the render either produces garbage or
faults. The exception handler we point `VBAR` at (`bc_vectors` in `bigcore_entry.S`)
captured the exact fault: a data abort with the faulting address equal to the worker's
`menu_ptr` plus a struct offset, where `menu_ptr` was a fixed garbage value every boot.
In other words the worker read the comms/menu data cpu0 had written and got stale/
uninitialised DRAM back. Cross-core hardware coherency (which an A55 DSU with
Inner-Shareable Normal-WB mappings should give for free) is not actually functioning at
this pre-kernel stage, even after we set the Inner-Shareable bits on the boot mappings
(`mmu_flags_to_l1_arch_flags`, gated to EXPT) and invalidated the worker's D-cache by
set/way before enabling it.

Two changes to get a working (if not yet optimal) parallel render and to prove the
cause:

- Explicit cache maintenance instead of relying on HW coherency. Added a real
  invalidate-only primitive `arch_invalidate_cache_range` (DCIMVAC by MVA to PoC) in
  `arch/arm/cache-ops.S` - it was declared in `arch/ops.h` but never implemented for
  this core. It is invalidate-ONLY on purpose: a clean-invalidate could write this
  core's stale lines back over the peer's fresh DRAM. It is guarded to
  `AYANEO_BIGCORE_EXPT` so the release image is byte-identical (the function is absent
  from the release `cache-ops.o`; verified with `nm`). The discipline
  (`snes_driver.c`): before releasing a frame cpu0 CLEANs the comms job, the `snes_menu`
  struct, and the scene-node pool (`0x50C00000`, 4 MB) to DRAM; the worker INVALIDATEs
  the same regions before reading, renders, then CLEANs its output band; cpu0
  INVALIDATEs that band before present. The fork/join is a strict ping-pong so the
  whole-block clean-back is idempotent (no field the peer owns is clobbered).
- Coherency diagnostics (one flash answers everything, all shipped to the log, not just
  the OSD):
  - `AT` probe in `bc_entry_arm2`: after the MMU is on the worker AT-translates the
    comms VA (`ATS1CPR`) and stores `PAR` (`mrrc p15,0,...,c7`) to comms. `PAR[8:7]` is
    the shareability and `PAR[63:56]` the memory attributes actually in force for this
    core - this reveals whether the mapping is really Inner-Shareable Normal-WB (if not,
    the correct fix is the mapping, and all the explicit maintenance can be deleted).
  - The worker records the `menu_ptr` it actually read from comms (`w_menu`) and the
    first word it read THROUGH that pointer (`w_menuw0`), flushing line 6 immediately so
    the value survives even if the dereference faults. cpu0 logs `cpu0 menu` vs
    `worker-read menu` vs `worker-read *menu`: equal pointers + a non-garbage word means
    the explicit invalidate now delivers cpu0's writes; a mismatch means coherency is
    still broken and points at the mapping.
  - `bc_vectors`/`bc_undef`/`bc_pabt`/`bc_dabt` capture `{type, FAR, FSR, PC, SPSR}`; a
    one-time comprehensive dump prints the fault, the worker CPU state
    (`MPIDR/SCTLR/CPACR/FPEXC`), and the job it was handed. A light periodic line
    (every 240 frames) prints `wfin/fb/wait/stage/fault/wcnt/wmenu/w*menu`.
  - OSD readability: a dark semi-transparent backing box (`0xC8000000`) is filled behind
    the overlay text so it is legible over the light wallpaper.

Open item: the 4 MB scene-pool clean+invalidate per frame on both cores is the obvious
cost that could erase the multi-core win; it is acceptable only as a correctness crutch
while the `PAR`/probe data tells us whether a mapping fix can restore real HW coherency
(and let the explicit maintenance be dropped, or at least narrowed to the nodes actually
touched).

## 7. Root cause on HW: a clean/invalidate war, not a mapping bug

The on-device run answered the coherency question decisively:
- `PAR` of the comms VA on the worker = `0xff000000:0x54000b80`: F=0 (no fault),
  `SH[8:7]=0b11` (Inner-Shareable), `ATTR[63:56]=0xff` (Normal Write-Back), `PA=0x54000000`.
  So the worker's mapping is exactly right; shareability/attributes are NOT the problem.
- Kernel device tree `cpu-map`: cpu0 (`reg=0x000`) and the worker (`reg=0x100`) are BOTH in
  `cluster0` (the six A55). They share one inner-shareable domain, so Inner-Shareable is the
  correct shareability. Do NOT switch to Outer-Shareable, and A55 has no software `SMPEN` to
  add (DSU-managed).
- The coherency probe showed the worker read `menu_ptr = 0xab102d01` (fixed uninitialised-DRAM
  garbage) where cpu0 wrote `0x4c5c304c`, then data-aborted dereferencing it.

The actual bug was in the explicit maintenance, not the hardware: BOTH cores were cleaning the
WHOLE comms block. `arch_clean*_cache_range(g_bc, sizeof)` on a core writes that core's own
cache copy of EVERY comms line back to DRAM, including lines the OTHER core owns. So the worker's
`BC_CLEAN(g_bc, sizeof)` stamped its stale/uninitialised copy of the job line (`menu_ptr` @192)
back over cpu0's freshly written value, and cpu0's per-frame OSD accessors
(`bigcore_raw_magic/counter`, `bigcore_cached_ok`) did the same clean-invalidate of the whole
block over the worker's `done`/`counter`. A clean/invalidate war: whichever core cleaned last
won, and it was usually garbage. Two supporting scope bugs: the worker's `BC_INVAL(g_bc,64)`
invalidated line 0, never the `go` line at offset 64; and cpu0 set `go` AFTER its block clean, so
`go` was never actually flushed to DRAM.

### The fix (all EXPT-gated; release stays single-core, host split still IDENTICAL)
Make every 64 B comms line have exactly ONE owner that ever CLEANS it; the other core only
INVALIDATES before reading, and never cleans a peer-owned line. Ownership:
`stage@0`, `done@128`, `counter@256`, `fault@320`, `diag@384` = worker; `go@64`, `job@192` =
cpu0. Concretely (`emu/snes/snes_driver.c`, `emu/snes/bigcore.c`, `emu/snes/bigcore_entry.S`):
- cpu0 cleans ONLY `job@192` (after writing the payload) and `go@64` (after publishing the seq -
  this was missing entirely), plus the bulk menu + scene pool it produced. It invalidates the
  worker-owned lines (`done`, `stage`, `counter`, `fault`, `diag`) before reading them.
- The worker cleans ONLY its owned lines (`stage`, `done`, `counter`, `diag`) and its rendered
  framebuffer band. It invalidates the cpu0-owned `go@64` and `job@192` before reading them, and
  invalidates the bulk menu + scene pool before reading.
- The asm fault handlers (`bc_dabt` etc) now `DCCMVAC` the fault-capture line so cpu0 reliably
  reads the fault (previously they only wrote it and parked in WFE).
- The OSD accessors in `bigcore.c` switched from whole-block `arch_clean_invalidate_cache_range`
  to invalidate-only of just the worker-owned line they read (line0 for magic, line4 for
  counter/cached_ok) - no more cpu0-side clobber of the worker's heartbeat.
This handoff is correct whether or not HW snoop-coherency is active, because it moves data through
DRAM (the Point of Coherency): the owner cleans to PoC, the reader invalidates then reads PoC, and
no core ever writes back a line it does not own.

Also: the bounded-fallback that made a failing worker cost 4 fps (cpu0 spun a fixed 1,000,000
iterations ~= 11 ms every frame) is now a wall-clock deadline of ~1.5 ms using the 13 MHz gpt4
tick, so a worker that never signals `done` degrades the menu to ~30 fps single-core, not 4 fps.

## 8. The real wall: the worker is never admitted to the DSU snoop-read domain

Sections 6 and 7 documented intermediate theories (cache-maintenance war, a stray set/way
invalidate). Those were real bugs and were fixed, but they were NOT the root cause. After
about seventeen on-hardware flashes and three deep reverse-engineering passes on the device
ATF, the actual wall is now precisely located and thoroughly proven.

### The one-line symptom

Once the worker turns its MMU on, it cannot read ANYTHING cpu0 writes to shared DRAM, under
ANY memory attribute. The worker's own writes are always visible to cpu0. It is a purely
directional failure: worker to cpu0 works, cpu0 to worker read is blind.

### Everything that was tried and failed (all confirmed on HW)

- Normal-WB Inner-Shareable mapping: worker reads garbage.
- Explicit clean (cpu0) + invalidate (worker) to the Point of Coherency: garbage.
- Removing the DCISW set/way invalidate from the worker bring-up: no change.
- Worker with D-cache OFF (SCTLR.C=0): garbage.
- Device-nGnRE (non-cacheable) shared mapping, via a sibling-preserving L2 split: garbage.
- Strongly-ordered (Device-nGnRnE) + Outer-Shareable, i.e. matching the MMU-off default
  attribute EXACTLY: still garbage.
- An ATF patch that NOPs the `w21` gate in `pwr_domain_on_finish` so the worker runs the
  FULL per-core on-finish body (CCI/DSU admit + EMI setup + every call it normally skips):
  no change.

### The decisive diagnostics

Three probes, all on the same physical 2 MB Device page, nailed it:
- cpu0 writes `0x51000000`, DSB, reads it back: sees its own write (`0xca5a0001`).
- Worker's own PAR (ATS1CPR) of `0x51000000`: ATTR `0x04` = Device. cpu0's PAR agrees.
  So both cores genuinely map it non-cacheable, no caches involved.
- Worker reads `0x51000000`: the stale pre-write value (`0xa86dbdec`), every frame.
- Worker writes `0x51000040` (next word, same page): cpu0 reads it correctly.

Same page, both non-cacheable, cpu0's write lands, worker's write lands, but cpu0 to worker
read is invisible. With no caches in the path this cannot be a coherency-maintenance problem.

### Why the MMU is the toggle

At bring-up the worker reads cpu0's data CORRECTLY - it reads cpu0's LPAE MMU snapshot from
the comms block and successfully adopts it. But that read is done with the worker's MMU OFF.
With the MMU off, an A55 issues all data accesses endpoint-direct (they do not consult the
DSU/CCI snoop fabric). With the MMU on, the access is issued as a translated transaction
carrying the core's coherency identity and is routed through the DSU snoop fabric - and the
worker is not an admitted read-consumer there, so it gets a stale line. The attribute is
irrelevant because the routing decision is upstream of the attribute.

### Why we cannot fix it from LK

The DSU/CCI coherency-admit registers ATF writes (MCUCFG `0x0C533308/0x0C533B08` CCI,
`0x0C533240/0x0C533A40` DSU) are CLUSTER-level fixed addresses, already set for cluster0 when
cpu0 booted. There is no per-core "snoop enable" register. The per-core admission happens
inside the hardware power-on sequence, which is driven by the SSPM co-processor. SSPM is
loaded by the preloader but is not servicing CPU power at the LK stage (which is exactly why
a normal `PSCI CPU_ON` hangs pre-kernel and we had to arm the CPC by hand). LK is Non-Secure
and DEVAPC drops all NS writes to secure MCUCFG/SPM, and no SiP service exposes snoop
admission. Forcing ATF to run the full software `pwr_domain_on_finish` body (the `w21`
un-gate) proved the admission is NOT in that software path - it is in the SSPM-driven
hardware sequence our cold-boot CPC bypass shortcuts.

Net: at the LK stage the second core can execute and its writes are coherently visible, but
its reads can never observe cpu0's writes, so no per-frame data can be handed to it and the
render split is impossible. The kernel gets coherency for free only because the SSPM/SPM
power stack is fully live by kernel-handover time.

## 9. Next avenue: reach the kernel-handover state, then do not hand over

The kernel does nothing special to make its cores coherent - it just calls `PSCI CPU_ON`.
The magic is entirely in the firmware/power state that is live by kernel-handover time. So
the promising direction is to make LK reach that state and then run our own menu instead of
jumping to Linux (a "fake kernel jump"):

- LK already calls `MTK_SIP_KERNEL_BOOT` (0x82000115) with a BOGUS entry, which arms the
  SPMC and returns. The real kernel handover calls it with a VALID entry, at which point ATF
  performs its COMPLETE final EL3 setup and ERETs to the entry. The idea is to point that
  entry at our own stub, so ATF does the full handover and lands in our code with the
  handover-complete system state, where a subsequent `PSCI CPU_ON` may admit coherency.
- Open questions to resolve before trying it: does the full handover reconfigure the world
  in a way our menu cannot survive (EL/MMU/console)? Does SSPM only start servicing CPU
  power after a kernel-driver mailbox handshake (in which case the fake jump alone is not
  enough and we must replicate that handshake)? Is there an NS-reachable SSPM mailbox/SiP to
  request the CPU-power service?

This is the active line of investigation. All of the Phase-2 machinery (cached worker
bring-up, per-frame fork/join, the output-correct scanline split) is preserved behind
`AYANEO_BIGCORE_EXPT` and is ready the moment the worker becomes a coherent read participant.
