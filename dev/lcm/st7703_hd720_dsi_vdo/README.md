# st7703_hd720_dsi_vdo

DSI video-mode panel driver for the AYANEO Pocket Air Mini (MT6785 / k85v1_64).

## Provenance

Panel geometry and the register init sequence were recovered from the stock
signed `lk` image (`st7703_hd720_lcm_drv`). `get_params` was reconstructed by
disassembling the stock get_params function and mapping the struct stores to
named `LCM_PARAMS` fields; the init table is a faithful transcription of the
stock 171-entry DSI command table (`st7703_hd720_init_dump.txt` in the repo
root has the raw dump).

## Panel parameters

- 1280x960, DSI video mode (SYNC_PULSE_VDO_MODE), 4 lanes, RGB888
- VSA/VBP/VFP = 8 / 8 / 16, HSA/HBP/HFP = 30 / 60 / 60
- PLL_CLOCK = 266

## Drive voltage mods (over-driven by default)

The panel uses a page-based register scheme (CMD 0xEE selects the page). The
drive voltages are exposed as `ST7703_VGH`, `ST7703_VGL`, `ST7703_CPUMP`,
`ST7703_AVDD` and `ST7703_AVEE` at the top of the .c file.

They are set ABOVE stock by default (this panel is over-driven):

| Define        | Default (this build) | Stock  |
|---------------|----------------------|--------|
| ST7703_VGH    | 0x78 | 0x58 |
| ST7703_VGL    | 0x78 | 0x58 |
| ST7703_CPUMP  | 0x48 | 0x32 |
| ST7703_AVDD   | 0xFF | 0xE0 |
| ST7703_AVEE   | 0x60 | 0x20 |

To run stock voltages, reset the defines to the Stock column. Tune ONE register
at a time and verify on the panel. Over-driving does not fail safe (flicker,
image inversion, temporary retention/burn-in). Ceilings: AVEE 0x60, VGH/VGL
0x78, AVDD 0xFF. AVEE has the largest visible effect.
