# Konkr Pocket Block: MT8786V/CU on the mt6768 platform (board m8695_bw01), eMMC, A/B, MT6358
# PMIC with an RT9467 charger on I2C7, a 960x720 JD9168S panel scanned landscape, all-GPIO
# buttons. The stock firmware is Android 12 on a GKI 5.10 kernel (boot header v4, vendor_boot,
# bootconfig) and the console ships locked with a green verified boot state, which this LK keeps:
# it reports the real lock and boot state to Android and to the TEE. Build with
# `make k68v1_64_block` (build_konkr_block.sh builds and signs).
LOCAL_DIR := $(GET_LOCAL_DIR)
TARGET := k68v1_64_block
MODULES += app/mt_boot \
           dev/lcm

# Board switch for the C code.
KONKR_BLOCK := yes
DEFINES += KONKR_BLOCK

# Android knows the SoC as mt8786 (ro.boot.hardware, /firmware/android hardware), as the stock LK
# reports it.
MTK_PRODUCT_LINE_PLATFORM = MT8786

MTK_EMMC_SUPPORT = yes
MTK_MMC_COMBO_DRV = yes
MTK_KERNEL_POWER_OFF_CHARGING = yes
MTK_SMI_SUPPORT = yes
DEFINES += MTK_NEW_COMBO_EMMC_SUPPORT
DEFINES += MTK_GPT_SCHEME_SUPPORT

# Charging: the new charger framework with the RT9467 (I2C7 0x5b) and Pump Express+, as the
# stock LK has it. There is no MT6370 on this board (its device tree nodes are unbound templates).
MTK_CHARGER_NEW_ARCH := yes
MTK_CHARGER_INTERFACE := yes
MTK_PUMP_EXPRESS_PLUS_SUPPORT := yes
MTK_RT9467_SUPPORT := yes
DEFINES += RT9467_I2C_ID=I2C7

# Jadard JD9168S, 960x720 native landscape, no rotation; the logo set is the stock one rebuilt
# image for image (tools/block/logo_from_stock.py).
MTK_LCM_PHYSICAL_ROTATION = 0
CUSTOM_LK_LCM="jd9168s_960x720_dsi_vdo_m8695_bw01"
BOOT_LOGO := block

MTK_SECURITY_SW_SUPPORT = yes
MTK_VERIFIED_BOOT_SUPPORT = no
MTK_SEC_FASTBOOT_UNLOCK_SUPPORT = yes
MTK_DM_VERITY_OFF = no
MTK_AVB20_SUPPORT := yes
SPM_FW_USE_PARTITION = yes
MTK_AB_OTA_UPDATER = yes
MTK_SMC_ID_MGMT = yes

DEBUG := 2
DEFINES += WITH_DEBUG_UART=1
# The serial number stays the stock one from proinfo (Android's adb and keystore identity).
CUSTOM_LK_USB_UNIQUE_SERIAL = no

MTK_TINYSYS_SCP_SUPPORT = no
MTK_TINYSYS_SSPM_SUPPORT = yes
MTK_GOOGLE_TRUSTY_SUPPORT = no
MTK_VPU_SUPPORT = no
MTK_PROTOCOL1_RAT_CONFIG = C/Lf/Lt/W/T/G
MTK_DYNAMIC_CCB_BUFFER_GEAR_ID =
DEVELOP_STAGE = SB

# Android 12: boot header v4, vendor_boot (vendor ramdisk, device tree, vendor command line,
# verified by vbmeta with boot) and the androidboot.* settings as a bootconfig block.
VENDOR_BOOT_SUPPORT := yes

# The console stays locked with its stock Android: the lock state, the verified boot state and the
# root of trust reach Android and the TEE as the stock LK reports them, so the stock /data keys
# stay valid (the AYANEO and KPA builds report an unlocked device instead).
DEFINES += KEEP_STOCK_BOOT_STATE

# Bring-up: the LK keeps its log (fastboot oem dump_pllk_log) at the stock level.
DEFINES += LK_LOG_INFO

# The Android 12 preloader's EMI boot tag and the kernel's /dramc, /emicen, /emimpu, /emiisu nodes.
EMI_TAG_V2 := yes

# Bring-up without a serial console: start in fastboot, `oem tryboot` boots once under the
# watchdog, `oem trace` / `oem lastkmsg` read back what happened (platform/common/boot_trace.c).
BOOT_TRACE := yes
# Bring-up: checkpoints on the RGB LED, for a boot that stops before anything else can report.
BLOCK_LED_DIAG := no

# The prebuilt (Android 10 era) security library cannot handle the Android 12 preloader: seccfg is
# read by our own read-only code, the LK skips checking its own images and the library's set up.
SECCFG_READ_ONLY := yes
SKIP_LK_IMAGE_AUTH := yes

# The MT8786V/CU is a WiFi only part: md1img is a 4 KB stub and the stock LK loads no modem.
LK_LD_MD_SUPPORT := no

# Bring-up: the first 1 MB unmapped, so a null pointer read is a reported data abort.
NULL_PAGE_GUARD := yes

# The A/B record in misc is the AOSP bootloader_control ("BCAB") one, as the preloader keeps it.
MTK_BOOTCTRL_VERSION := bcab

# The serial number (fastboot, androidboot.serialno, adb) is the one written at the factory at the
# start of proinfo, as the stock LK reads it.
DEFINES += SERIAL_NUM_FROM_BARCODE
