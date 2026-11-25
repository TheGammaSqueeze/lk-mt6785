# Enabling the extra CPU cores (secure-firmware CPC patch)

By default the GammaOS Pico LK runs on a single core. The other CPU cores can be
powered on from the LK, but only after a small, reversible patch to the device's
secure firmware (`tee.img`, which carries the ARM Trusted Firmware / BL31). This
directory has the tooling and the recipe so you can patch your own device.

**This is optional.** The stock, unpatched firmware runs GammaOS Pico perfectly
on one core. Only apply this if you want the multi-core capability.

## What the patch does

On these MediaTek parts BL31 does not arm the CPU power controller (the SPMC /
CPC) at cold boot. It arms it only when the non-secure kernel later calls the
MediaTek "kernel boot" SMC, and that SMC hands control to the kernel and never
returns to the bootloader. So a bootloader that tries `PSCI CPU_ON` for a second
core finds the controller unarmed and the call hangs.

The firmware already contains a one-shot routine that arms the controller (the
routine the kernel SMC calls). It is self-gated by a flag byte: the first caller
arms the controller and sets the flag; every later caller just returns. The
patch makes the cold-boot core call that routine once, by overwriting a single
droppable log-print branch in `bl31_main` with a branch to the routine.

Because the routine is one-shot, the later real kernel-boot SMC becomes a no-op,
so a normal **Android/Linux boot is unaffected** - the controller is simply
already armed by the time the kernel runs, which is exactly the state the kernel
expects.

It is a 4-byte code change plus a re-sign of the firmware's certificate.

## Safety first

Flashing `tee` is brick-risky. If a bad `tee` is written, the device will not
reach the LK and the **only** recovery is the SoC download tool (SP Flash Tool
on MediaTek), which needs the stock `tee.img` and a manual power/cable step.

- Keep the `*_stock.img` the tool writes. That is your rollback.
- Flash the `*_control.img` (re-signed, no code change) **first** and confirm the
  device still boots. That proves the preloader accepts our re-sign on your unit.
- Only then flash the `*_armcpc.img`.
- As long as a patched `tee` still boots to the LK, you can revert it over
  fastboot with the stock image; only a `tee` that fails to boot needs SP Flash
  Tool.

## Using the tool

You need the **stock `tee.img` for your exact device/build** (pull it from your
stock firmware package, or read the `tee_a` partition with the SoC download
tool), Python 3, and the `openssl` CLI.

```
python3 gammaos_cpc_patch.py \
    --profile profiles/konkr_kpa.json \
    --in /path/to/stock_tee.img \
    --out ./out
```

The tool refuses an input whose md5 is not the build the profile names (the addresses belong to
one build), and checks its own output: against the re-signed control image the patched one may
differ in that one instruction and in the certificate only.

This writes `out/tee_kpa_stock.img`, `out/tee_kpa_control.img`,
`out/tee_kpa_armcpc.img`. Flash with fastboot (device in bootloader mode):

```
fastboot flash tee out/tee_kpa_control.img   # reboot, confirm it still boots
fastboot flash tee out/tee_kpa_armcpc.img    # reboot, confirm it still boots
```

The tool re-signs with the MediaTek test key under `../../ayaneo/keys`, which is
what these test-key / unlocked units verify against. A unit fused to a
production key needs that key via `--key` / `--pub`.

## Profiles included

- `profiles/konkr_kpa.json`: Konkr Pocket Advance (MT6785). The redirect goes to the firmware's
  one-shot routine, so the later kernel boot call finds the controller already armed.
- `profiles/konkr_block.json`: Konkr Pocket Block (MT6768). The redirect goes to the plain arming
  routine instead. On this firmware the one-shot routine the kernel boot call uses does more than
  arming, and calling it at cold boot leaves the console unable to boot (recovered with SP Flash
  Tool). The kernel boot call still runs its full routine later, as on stock firmware.

When you make a profile for another build, prefer the plain arming routine (it only unlocks the
SPM, releases the other cores' resets and sets the enable bit) over a larger wrapper around it.

## Making a profile for a different device/build

The patch-site and routine addresses differ per firmware build. A profile is
four numbers (see `profiles/konkr_kpa.json`). To find them, disassemble the BL31
inside your `tee.img`:

1. The GFH `atf` partition starts at file 0, with a 512-byte partition header.
   The AArch64 BL31 reset vector begins a little after it (for the Konkr it is
   file `0x440`: `mov x20,x0; ...; msr vbar_el3,x0; isb; bl ...`). That offset is
   `payload_base`; every BL31 address `A` is at file `A + payload_base`.
   ```
   dd if=stock_tee.img of=bl31.bin bs=1 skip=$((0x440))
   aarch64-linux-gnu-objdump -D -b binary -maarch64 bl31.bin > bl31.asm
   ```
2. `wrapper`: find the one-shot CPC-arm routine. It reads a flag byte, returns if
   set, else sets it and writes the SPM unlock (`0x0b160001` to the SPM base) and
   sets `CPC_CTRL_ENABLE` (bit 16) in `MCUCFG_CPC_FLOW_CTRL` (`0x0C53A814`). On the
   Konkr it is at `0x9290`. It is the routine the kernel-boot SMC handler calls.
3. `patch_site`: a `bl` to a logging/print function in `bl31_main` (the cold-boot
   path, runs once on the boot core) whose result is unused. On the Konkr it is
   `0x4588` (`bl` to the log function `0x7190`). Confirm the next instructions
   overwrite the result register, so dropping the print is safe.
4. `old_insn`: the current 4 bytes at the patch site, as a 32-bit word (the tool
   refuses to patch if the firmware does not hold exactly this, so a wrong
   profile cannot corrupt a different build).

The replacement is `bl patch_site -> wrapper`, encoded
`0x94000000 | (((wrapper - patch_site) >> 2) & 0x03FFFFFF)`. The tool does this.

## After patching: using the cores

The LK still boots on one core and powers no other core by itself. With the patched
firmware, the in-game menu of a GBA game offers two rows on its Gameplay page:

- **Big Core** (off by default): the game's CPU runs on one of the big cores from the
  next game start. On unpatched firmware the row reads "Not available".
- **Big Core Clock** (Konkr Pocket Advance): the big core's clock, one of the console's
  stock levels up to 2050 MHz, each with its stock voltage.

The big core stays powered between games once it was used and is put back as the boot
left it at a reset or power off. A game that stalls or faults there is closed back to
the menu instead of hanging the console.

The runtime is `platform/common/pico/pico_smp*`; `pico_smp.h` has the API and the
`oem smp` commands cover manual bring-up, the on-device coherency self-test and the
load-test helpers.
