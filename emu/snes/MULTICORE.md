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
