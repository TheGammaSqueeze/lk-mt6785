# Display overdrive levers: reducing ghosting and smearing

Device: AYANEO Pocket Air Mini, MT6785 (Helio G95), st7703_hd720 DSI panel (1280x960, 4:3), MIPI SYNC_PULSE video mode. LK owns the whole display pipeline; libretro cores present one emulated frame per vsync-locked refresh. Research only, nothing was edited. Claims below were cross-checked against the source and an adversarial pass; line numbers are approximate (a few off by a line or two).

## 1. Two distinct causes need two different levers

"Ghosting/smearing" here has two independent physical origins and no single knob fixes both.

- **Cause A, LC response-time lag (true ghosting).** A pixel fails to finish its gray-to-gray (G2G) transition inside one frame. It lives in the liquid crystal and the drive electronics and is attacked with voltage: larger source swing, harder gate charging, per-frame overshoot (RTC), VCOM/gamma shaping.
- **Cause B, sample-and-hold motion blur (perceived smear).** A correct pixel is held static for the whole frame while the eye tracks motion across it. This is a persistence artifact, not a voltage one, and is attacked only by shortening the visible hold: higher refresh, black-frame insertion, backlight strobing.

The coupling that matters: the main cause-B lever (raising refresh) shortens the frame, giving the LC less time to settle, which worsens cause A. Because the cause-A voltage headroom on this device is already spent, pushing cause B is not free.

## 2. Panel-side levers (ST7703 registers)

Init table `dev/lcm/st7703_hd720_dsi_vdo/st7703_hd720_dsi_vdo.c`; guide `/work/pairmini/ST7703_HD720_Panel_Voltage_Modding_Guide.md`. The five drive rails (defines `:34-38`) are all at their empirically-found ceilings: banked, not levers anymore.

| Lever | Cause | Current / ceiling | Status | Note |
|---|---|---|---|---|
| AVEE neg source rail (`:38` def, page04 reg0x09) | A | 0x60 / 0x60 (stock 0x20) | EXHAUSTED | Guide's single largest response win; 0x70 flickers |
| AVDD pos source rail (`:37`, page04 reg0x02) | A | 0xFF / 0xFF (stock 0xE0) | EXHAUSTED | Register max |
| VGH gate-on (`:34`, page01 reg0x14) | A | 0x78 / 0x78 (stock 0x58) | EXHAUSTED | 0x84+ inverts the image |
| VGL gate-off (`:35`, page01 reg0x15) | A | 0x78 / 0x78 | EXHAUSTED | Same inversion limit |
| Charge-pump freq (`:36`, page01 reg0x17) | A enabler | 0x48 (stock 0x32), no known ceiling | PARTIAL | Rails sag less under transient load; encoding undecoded, "most dangerous" class |
| **VCOM** low/high/offset (page01 reg0x28/0x29/0x2A = 0x1F/0x29/0x63, all stock) | A | stock, un-retuned | **UNTAPPED, best value** | Rails were raised without re-centering VCOM, leaving a DC bias that causes retention/sticking and asymmetric G2G. Retuning recovers symmetric drive and kills retention |
| Gamma curves (page02 0x00-0x10 pos, 0x20-0x30 neg) | A | stock, undecoded | UNTAPPED | Steepening mid-gray is a static, per-target approximation of overdrive (not true RTC). Fiddly: bad ramps cause banding/color shift |
| References / analog bias (page08 0x12/0x13/0x61) | A | stock | UNTAPPED, highest-risk | 0x61 is the only plausible source-amp slew knob; VREF/VCM anchor the whole stack. Mistune corrupts everything |
| GIP / gate timing (pages 05/06/07) | A | stock, undecoded | UNTAPPED, impractical | Wider gate-ON window = more settle time, but dozens of interdependent values and largely redundant with maxed VGH |

**Panel bottom line:** every direct cause-A voltage rail is maxed. The only real headroom is companion tuning, and VCOM re-centering is the standout because it fixes the retention the maxed rails introduced. None of these is a big motion-smear win.

## 3. SoC-side levers (MT6785 DISP pipeline)

**Hardware overdrive / RTC does not exist on this SoC and cannot be added.** This is the load-bearing finding for cause A:

- The DISP OD engine is physically absent. `DISP_OD_BASE` is commented out (`platform/mt6785/include/platform/mt_reg_base.h:358`), while COLOR/CCORR/AAL/GAMMA/DITHER bases are live. No OD registers exist in the mt6785 headers, whereas mt6757's `ddp_reg.h` has a full OD block, so MTK ships it elsewhere and dropped it here. The `DISP_MODULE_OD` enum survives but is vestigial (NULL driver, not-connectable, no mutex bit).
- No previous-frame buffer to diff against. Scanout is single-pass `RDMA_MODE_DIRECT_LINK` (`ddp_rdma.c` ~891-907), OVL straight into RDMA into DSI0, one in-flight composition. A home-grown per-pixel RTC would need a retained frame plus a prev-vs-target LUT at 1280x960x60 on the A55 with no GPU compositor: not realistic in this bootloader, and the cores are already near the 16.7ms budget.

Weak or wrong-layer SoC levers:
- **PQ path (COLOR/CCORR/AAL/GAMMA)**: all in-path but bypass/relay stubs (`ddp_misc.c`). You could program a static steepened gamma/CCORR transfer curve, but it is content-global (crushes shadows / clips highlights) and cannot inject a one-frame overshoot. Marginal for ghosting, nothing for smear.
- **DITHER0**: the one active PQ engine, but spatial-only (temporal fields never written). Enabling temporal dither would add per-frame shimmer that reads as more smear. Not a lever.
- **DSI D-PHY drive strength / impedance / HS-TX voltage**: governs MIPI link integrity up to the panel's DSI RX, does not reach the source-driver swing (that is the panel's AVDD/AVEE, already maxed). Wrong layer.
- **MIPI data-rate + SSC** (`AYANEO_GBA_DR_KHZ=526512`): only matters as the substrate the vfp->refresh math divides. No smear reduction itself.

## 4. Sample-and-hold levers (cause B)

The backlight is the on-die **DISP_PWM0** block (not PMIC/I2C/GPIO): `ayaneo_apply_backlight` (`mt_disp_drv.c`) -> `mt65xx_leds_brightness_set` -> `disp_pwm_set_backlight`, writing a 10-bit duty to `DISP_PWM_CON_1` (`ddp_pwm.c` ~210), EN gated off at duty 0. The st7703 LCM driver has no `set_backlight` member, so there is no panel-internal CABC/0x51-0x55 dimming. No BFI or strobe code exists in the tree today.

**(a) Vsync-synced backlight strobing: UNTAPPED, best achievable cause-B lever here.** All PWM registers are runtime-writable, so it is mechanically possible, but it needs net-new timing code and has two hard problems: (1) the DISP_PWM carrier is a free-running CPU-written dimmer with no vsync trigger, so lowering duty just dims uniformly; a real strobe must fire off the FRAME_DONE IRQ and hold full duty through the LC-settle tail before blanking; (2) a second timed actor must toggle PWM_EN mid-frame while the present thread is blocked on FRAME_DONE, and CPU-timed jitter causes brightness beat/banding. Cost: brightness loss scales with the dark fraction, and the rails are maxed so there is no headroom to compensate; at ~59.7Hz a single strobe per frame is a ~60Hz square wave that flickers. Honest magnitude: roughly a 25-40% persistence cut before flicker/brightness become objectionable, best paired with a higher base refresh.

**(b) Black-frame insertion: UNTAPPED but least feasible here.** Present the real frame on even refreshes, black on odd. The logic is easy, but it needs real ~120Hz scanout this timing does not provide (see the refresh correction in section 5), halves brightness with no headroom, starves the slow LC of settle time (worsens A), adds black-to-image-to-black G2G transitions (more A), and breaks the one-frame-per-present pacing / audio clock recovery. Strobing is the better B lever: no 120Hz needed, no extra G2G penalty.

## 5. Interplay: refresh couples B against A, and the vfp knob is nearly maxed

Refresh is a live lever: `refresh_milliHz = LINE_CONST / (976 + vfp)`, changed via `DSI_VFP_NL` with no DSI reset. But the important correction: **the vfp knob cannot meaningfully raise refresh.** `DSI_VFP_NL` is 12-bit, software-clamped to vfp `[4,256]` (`ddp_dsi.c` ~1117), and vtotal floors at 976 lines, so the vfp-only range is about **48.4 Hz (vfp 256) to 60.9 Hz (vfp 4)**. The cores already sit at 60.088 Hz (SNES, vfp 17) and 59.727 Hz (GB/GBC/GBA/menu, vfp 23), so "vfp toward min" buys at most ~1 Hz. That is a judder-matching / slight-downshift tool, not a motion-clarity uplift.

A genuine >60Hz refresh for cause B requires **raising the pixel clock: PLL_CLOCK (currently 266) and the MIPI data rate**, which is the approach behind the standalone 70Hz image (`lk-verified-70hz.img`). Doubling PLL to ~532 gives a data rate ~1064 MHz, comfortably under the MIPITX assert ceiling (rejects >2500 MHz), so it is assert-legal but an untested st7703 regime that needs DSI-PHY and panel re-validation. Raising the base clock is also more A-friendly than shrinking the porch: for a given Hz the LC keeps more settle time. Because every cause-A rail is maxed, any refresh push leans on drive margin that no longer exists, so the A-vs-B crossover (how much ghosting returns per added Hz) is the key on-device measurement and cannot be answered from code.

Note: present is vsync-locked by default (the shipped path waits FRAME_DONE), but an `ayaneo_present_skip_framedone` bypass exists (`mt_disp_drv.c` ~776), so a future strobe/BFI implementer is not strictly bound to that cadence.

## 6. Recommended experiment order (highest impact / lowest risk first)

Do not chase the exhausted rails, and do not chase SoC OD/RTC (absent, unbuildable).

1. **VCOM re-centering (cause A, lowest risk, no new code).** Knob: page01 reg 0x28/0x29/0x2A (currently stock 0x1F/0x29/0x63). Expected: removes the DC-offset retention the maxed rails introduced and recovers symmetric G2G drive. Validate on a mid-gray checkerboard, one byte at a time, power-cycle, watch for the flicker/retention minimum. Same edit/re-sign/flash flow as the rail mod. Start here.
2. **Vsync-synced backlight strobing (cause B, best achievable smear win).** Knob: new LK timing code that holds full DISP_PWM duty through the LC-settle tail off the FRAME_DONE IRQ, then drops to 0 for the frame remainder. Expected: ~25-40% persistence cut. Net-new code; brightness/flicker-limited at 59.7Hz, so it pairs best with #3. First answer: what carrier freq `pwm_div=0` gives, and whether `GPIO_LCD_BL_EN` can hard-strobe cleaner than PWM_EN.
3. **Higher BASE refresh via PLL raise (cause B), coupled-aware.** Knob: raise PLL_CLOCK / data rate (the 70Hz-variant approach), NOT the vfp knob (which only moves ~1 Hz). Expected: shorter hold cuts smear and raises the strobe rate above the flicker band, unlocking a deeper strobe in #2. Validate on-device for tearing/stability and for cause-A regression (no voltage headroom left to absorb it). Assert-legal at ~1064 MHz but an untested panel regime.
4. **Charge-pump frequency (cause A enabler).** Knob: page01 reg 0x17 (0x48, no known ceiling). Low direct impact, but better current delivery when #3 shortens the row time. One step at a time, full white/black flash test.
5. **Static gamma steepening (cause A, weak).** Knob: page02 ramps, or un-bypass the SoC GAMMA/CCORR engine. The closest static substitute for the RTC this hardware cannot do. Needs a photodiode / high-speed-camera G2G measurement to justify the banding/color risk. Low priority.

Explicitly do NOT pursue: SoC OD/RTC (absent), home-grown per-pixel RTC LUT (no prev-frame, blows the A55 budget), D-PHY drive strength for ghosting (wrong layer), temporal dither (adds shimmer), BFI (needs non-existent 120Hz, halves brightness, worsens A), GIP timing edits (impractical, redundant with maxed VGH), and any further rail push (all at ceiling).

## 7. Hard limits on this hardware

- No true per-transition overdrive / RTC from either side: panel is a video-mode TCON with no full-frame store; SoC OD engine is physically absent and scanout is single-pass with no prev-frame buffer.
- Cause-A voltage headroom is fully spent (AVEE 0x60, AVDD 0xFF, VGH/VGL 0x78 are ceilings; overdriving does not fail safe, maxing rails together produced distortion + temporary retention).
- No 120Hz today: vfp can only lower Hz; classic BFI is out until the pixel clock/data rate is roughly doubled (assert-legal but untested).
- No panel-internal backlight/strobe path: any strobe must be CPU-driven DISP_PWM writes, and clean sub-frame strobing at ~59.7Hz flickers with a brightness cost the maxed rails cannot offset.
- Needs on-device measurement, not answerable from source: the ST7703 charge-pump/gamma field encodings, whether a true column-precharge register exists (none found), the correct VCOM after the rail raise, the DISP_PWM carrier frequency at pwm_div=0, the panel's true DSI-RX max line rate, and the A-vs-B crossover per added Hz.
