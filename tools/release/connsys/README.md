# Wi-Fi and Bluetooth firmware (CONNSYS)

The firmware the consoles' combined Wi-Fi and Bluetooth chip (MediaTek CONNSYS, part of the SoC)
runs, as their stock Android loads it from its `vendor/firmware` folder. GammaOS Pico's LK loads the
same files from the card's `bios/connsys/` folder when Wi-Fi or Bluetooth is turned on
(platform/common/pico/pico_bt.inc, pico_wifi.inc). They are the chip maker's, unchanged, taken from
each console's stock firmware, the same files the stock firmware packages for reflashing the
consoles carry.

| Console | Files |
| --- | --- |
| Konkr Pocket Advance (MT6785) | `*_2a_1*` |
| Konkr Pocket Block (MT8786 / MT6768) | `*_1a_1*` |

Each console uses only its own five; one card holds both sets, so it works in either console.
`SHA256SUMS` lists them. tools/ayaneo/gba/make_sdcard.sh copies them to the card template.
