# Konkr Pocket Advance (KPA): MT6785 k85v1_64 board family, same preloader, partition
# map, eMMC, sub-PMIC and GPIO button wiring as the AYANEO Pocket Air Mini, with a
# different panel, no volume buttons (rotary encoder), no analog sticks and I2S smart
# amps for the speaker. Everything shared comes from k85v1_64.mk; this file only
# overrides what the KPA board changes. Build with `make k85v1_64_kpa`.

include project/k85v1_64.mk

# The GBA-from-SD flow is the only configuration this board ships.
AYANEO_GBA_SD := yes
AYANEO_GBA := yes
DEFINES += AYANEO_GBA_SD

# Board switch for the C code: encoder volume, no analog ADC, AW883xx speaker path,
# portrait frame buffer rotated 90 degrees.
KONKR_KPA := yes
DEFINES += KONKR_KPA

# Sitronix ST7123, 640x960 native portrait, mounted landscape. The LK frame buffer
# stays portrait and everything is rotated into it in software, exactly as the
# stock Konkr LK does (MTK_LCM_PHYSICAL_ROTATION "90").
MTK_LCM_PHYSICAL_ROTATION = 90
CUSTOM_LK_LCM="st7123p_hd640_boe_dsi_vdo"

# 960x640 logo set (landscape sources, rotated at draw time like the stock set).
BOOT_LOGO := kpa
