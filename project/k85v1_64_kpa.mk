# Konkr Pocket Advance (KPA): MT6785 k85v1_64 board family, same preloader, partition
# map, eMMC, sub-PMIC and GPIO button wiring as the AYANEO Pocket Air Mini, with a
# different panel, no volume buttons (rotary encoder), no analog sticks and I2S smart
# amps for the speaker. Everything shared comes from k85v1_64.mk; this file only
# overrides what the KPA board changes. Build with `make k85v1_64_kpa`.

# The GBA-from-SD flow is the only configuration this board ships. Set BEFORE including the
# base project: its `?=` defaults and the ifeq blocks that add the DEFINES (AYANEO_GBA_SD,
# AYANEO_GBA, and AYANEO_GBC := no) are evaluated at include time, so a value set afterwards
# would leave the C code built as the GBC variant with the GBA SD flow bolted on.
AYANEO_GBA_SD := yes

include project/k85v1_64.mk

# Board switch for the C code: encoder volume, no analog ADC, AW883xx speaker path,
# portrait frame buffer rotated 90 degrees.
KONKR_KPA := yes
DEFINES += KONKR_KPA
# a unique fastboot / androidboot serial from the SoC efuse key (devinfo 12/13) instead of the
# shared default 0123456789ABCDEF (the stock LK shipped the default too)
DEFINES += CONFIG_MTK_USB_UNIQUE_SERIAL

# Sitronix ST7123, 640x960 native portrait, mounted landscape. The LK frame buffer
# stays portrait and everything is rotated into it in software, exactly as the
# stock Konkr LK does (MTK_LCM_PHYSICAL_ROTATION "90").
MTK_LCM_PHYSICAL_ROTATION = 90
CUSTOM_LK_LCM="st7123p_hd640_boe_dsi_vdo"

# 960x640 logo set (landscape sources, rotated at draw time like the stock set).
BOOT_LOGO := kpa
