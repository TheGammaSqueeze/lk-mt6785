# GBA panel refresh experiment (autonomous cron)

GOAL: drive the k85v1_64 panel refresh to the GBA's EXACT rate and stop.
TARGET = 16777216 / 280896 = **59.7275006 Hz** -> oem diag `hz1000` = **59727 or 59728**.
Constraint: prefer AT-OR-ABOVE the GBA rate (never slower). This is a POC - modifying the
DSI driver (ddp_dsi.c PCW / data rate) is explicitly allowed. DO NOT COMMIT until hz1000 is
59727/59728 (or provably the closest the hardware allows and confirmed with the user).

## How to measure
`fastboot -s 0123456789ABCDEF oem diag` -> `... hz1000=NNNNN` (128-frame vsync-locked
average, Hz*1000; menu + game loops). Reflash: `reboot bootloader; flash lk_a
out/lk_a_gba_sd_signed.img; reboot`; wait ~24s; read 2-3 times (stable to +-3).
Panel timing lives in dev/lcm/st7703_hd720_dsi_vdo/st7703_hd720_dsi_vdo.c under
`#if defined(AYANEO_GBA)` (vertical_frontporch, horizontal_frontporch, PLL_CLOCK).

## Measured so far (PLL_CLOCK=266, data_rate=532 MHz)
| vfp | hfp | vtotal | htotal | hz1000 | vs 59727.5 |
|-----|-----|--------|--------|--------|------------|
| 23  | 60  | 999    | 1430   | 59751  | +23.5 (above) |
| 23  | 61  | 999    | 1431   | 59697  | -30.5 |
| 22  | 62  | 998    | 1432   | 59701  | -26.5 |
| 24  | 59  | 1000   | 1429   | 59690  | -37.5 |
PLL=267 vfp=27: hz1000=59683 (below; pixel-clock scaling with PLL is NOT the naive 267/266).

CONCLUSION: with integer data_rate, the timing grid steps ~0.05 Hz and BRACKETS 59.7275 at
59.751 (above) / ~59.70 (below) - no integer combo lands on it. The pixel clock is
data_Rate(MHz)*derived; to hit exactly 59.7275 needs a FRACTIONAL data rate.

## Plan (fractional data-rate POC)
The MIPITX PLL is fractional-N: ddp_dsi.c ~1824 (DPHY path) computes
`pcw = data_Rate * pcw_ratio / 26` plus 3 fractional bytes = a precise Q8.24 PLL word for
`data_Rate` MHz. `data_Rate = dsi_params->PLL_CLOCK*2` is integer/even. POC: add a fine
data-rate override in kHz and compute the PCW from it, so the pixel clock (hence refresh)
can be tuned in ~0.0002% steps. For vfp=23/hfp=60 (measured 59.751 at 532 MHz), pixel clock
scales with data rate, so target data_rate ~= 532 * 59.7275/59.751 = 531.79 MHz = 531790
kHz (refine by measurement; expect a few binary-search steps).

## Log (append each iteration: config -> hz1000 -> next step)
- (start) baseline vfp23/hfp60 = 59751; audio clamp widened to +-512 (committed 9cd6b1c,
  panel-vs-GBA mismatch no longer snaps). Next: implement the fractional data-rate POC.
- *** POC WORKS *** implemented fractional data-rate override in ddp_dsi.c (both PHY PCW
  blocks, #define AYANEO_GBA_DR_KHZ, /26000 kHz form of the Q7.24 PCW; DR=532000 reproduces
  stock). vfp23/hfp60 + DR=531791 kHz -> hz1000=59726 (STABLE x3). That is 59.726 Hz = essentially
  the GBA 59.7275, but ~1.5 mHz UNDER (want at-or-above). refresh scales linearly with DR, so
  nudge up: DR = 531791 * 59728/59726 = 531809 kHz for hz1000=59728 (at-or-above). Next: DR=531810.
- DR=531810 -> hz1000=59730 (+2.5 above). 2 pts: slope 0.21 mHz/kHz. Target 59728 -> DR=531801. Next: 531801.
- DR=531801 -> hz1000=59730 (my reads) but USER sees Pico oscillating 59.726-59.730 = SSC ON
  (LCM memset leaves ssc_disable=0). Disabling SSC (ssc_disable=1): removes modulation +
  raises avg (SSC pulls down). Re-measure + re-tune DR. Next: measure SSC-off @ DR=531801.
- SSC OFF: DR=531801 -> hz1000=60331 ROCK STABLE x4 (SSC was pulling avg down ~1% + causing
  the oscillation). USER wants rock-solid EXACT. Re-tune DR down: 531801*59728/60331=526486 kHz
  for stable 59728. Next: DR=526486 (SSC off).
- PRECISE MEASURE (tick-based 8-frame window + 64-bit div) landed: SSC off, DR=526486 ->
  menu mean 59724.5 (rock solid; the earlier 59730 was coarse-measure error). DR=526520 ->
  menu mean 59728.16 (+0.66 mHz). User: "lower ever so slightly" -> DR=526512 -> menu mean
  59727.2, mode 59727 (range 59724-59730 = pure measure quantization; SSC off so PLL is fixed).
- IN-GAME rock solid: game-loop readout swung 59690-59757 = the 8-frame window catching per-frame
  emulation CPU jitter (NOT panel oscillation; PLL physically fixed). Widened game window 8->64
  frames (~1.07s), recal at 2 windows. Result: in-game window-means 59723-59731, mean 59727.0 =
  matches the menu. DONE. FINAL: DR=526512, SSC off, vfp23/hfp60/PLL266. hz1000 ~= 59727 both
  loops, at/just-below the GBA 59.7275 (user asked for ever-so-slightly-lower). STOP: committing.
