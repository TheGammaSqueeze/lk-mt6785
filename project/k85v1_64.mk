#
LOCAL_DIR := $(GET_LOCAL_DIR)
TARGET := k85v1_64
MODULES += app/mt_boot \
           dev/lcm
MTK_UFS_SUPPORT = yes
MTK_UFS_OTP = yes
MTK_EMMC_SUPPORT = yes
MTK_EMMC_SUPPORT_OTP = yes
MTK_MMC_COMBO_DRV = yes
MTK_KERNEL_POWER_OFF_CHARGING = yes
MTK_SMI_SUPPORT = yes
DEFINES += MTK_NEW_COMBO_EMMC_SUPPORT
DEFINES += MTK_GPT_SCHEME_SUPPORT
MTK_CHARGER_NEW_ARCH := yes
MTK_PUMP_EXPRESS_PLUS_SUPPORT := no
MTK_CHARGER_INTERFACE := yes
MTK_MT6360_PMU_CHARGER_SUPPORT := yes
MTK_LCM_PHYSICAL_ROTATION = 0
CUSTOM_LK_LCM="st7703_hd720_dsi_vdo"
DEFINES += MTK_ROUND_CORNER_SUPPORT
#nt35595_fhd_dsi_cmd_truly_nt50358 = yes
MTK_SECURITY_SW_SUPPORT = yes
MTK_VERIFIED_BOOT_SUPPORT = no
MTK_SEC_FASTBOOT_UNLOCK_SUPPORT = yes
SPM_FW_USE_PARTITION = yes
BOOT_LOGO := hd720
DEBUG := 2
#DEFINES += WITH_DEBUG_DCC=1
DEFINES += WITH_DEBUG_UART=1
#DEFINES += WITH_DEBUG_FBCON=1
# AYANEO Pocket Air Mini: build-time toggle for verbose bring-up logging.
# Release / GammaOS builds ship SILENT to match stock (both the LK console and
# the kernel UART are quiet, and the Root-of-Trust debug trace is compiled out).
# Build with `AYANEO_DEBUG_LOGGING=yes` to restore full LK dprintf + kernel UART
# logging (kernel gets ignore_loglevel + disable_uart=0) for debugging.
AYANEO_DEBUG_LOGGING ?= no
ifeq ($(AYANEO_DEBUG_LOGGING),yes)
DEFINES += AYANEO_DEBUG_LOGGING
endif
# AYANEO experiment (animated-boot-logo branch): paint a scrolling rainbow
# gradient over the whole panel during LK instead of the static eMMC boot logo.
# Set to no to restore the normal boot logo.
AYANEO_RAINBOW_BOOT ?= yes
ifeq ($(AYANEO_RAINBOW_BOOT),yes)
DEFINES += AYANEO_RAINBOW_BOOT
endif
# Boot audio experiment: play a short PCM boot sound (stored in boot_b) out the
# loudspeaker in sync with the animation. Build with `AYANEO_BOOT_AUDIO=no` to
# disable.
AYANEO_BOOT_AUDIO ?= yes
ifeq ($(AYANEO_BOOT_AUDIO),yes)
DEFINES += AYANEO_BOOT_AUDIO
endif
# Boot sound playback volume, 0-100 percent (0 = silent). Applied in code by
# scaling the PCM, so the stored asset stays full-scale and the level is a
# simple, flexible knob for future per-user preferences.
AYANEO_AUDIO_VOLUME ?= 40
DEFINES += AYANEO_AUDIO_VOLUME=$(AYANEO_AUDIO_VOLUME)
# Diagnostic: always-on trace of the boot-chime -> emulator audio handoff, even
# in the release build (a handful of prints, does not meaningfully alter timing).
AYANEO_AUDIO_TRACE ?= no
ifeq ($(AYANEO_AUDIO_TRACE),yes)
DEFINES += AYANEO_AUDIO_TRACE
endif
# Experimental: after the boot animation, run the SNES/SFC Classic home menu in
# LK instead of booting the kernel (native C, assets in boot_b). Mutually
# exclusive with the GBC/GBA emulator builds; reuses the same display/boot hooks.
# Enable with `AYANEO_SNES=yes`.
AYANEO_SNES ?= no
ifeq ($(AYANEO_SNES),yes)
DEFINES += AYANEO_SNES
AYANEO_GBC := no
endif

# Experimental: after the boot animation, run a GBC emulator (gambatte) in LK
# instead of booting the kernel. The core archive (emu/gbc/libgbc.a) must be
# prebuilt via emu/gbc/build_core.sh.
AYANEO_GBC ?= yes
ifeq ($(AYANEO_GBC),yes)
DEFINES += AYANEO_GBC
GBC_LIB := $(LK_TOP_DIR)/emu/gbc/libgbc.a
endif
CUSTOM_LK_USB_UNIQUE_SERIAL=no
MTK_TINYSYS_SCP_SUPPORT = no
MTK_PROTOCOL1_RAT_CONFIG = C/Lf/Lt/W/T/G
MTK_GOOGLE_TRUSTY_SUPPORT=no
# This device's secure OS is GenieZone (GZ), not Google Trusty (confirmed by the
# preloader/ATF UART log printing GZ params). Enable GenieZone so LK parses the
# modem-MTEE shared-memory boot tag and passes its mblock memory layout to GZ via
# SMC before the kernel jump, matching stock lk behaviour.
MTK_ENABLE_GENIEZONE = no
MTK_AB_OTA_UPDATER = yes
DEFINES += MTK_MT6370_PMU
DEVELOP_STAGE = SB
MTK_TINYSYS_SSPM_SUPPORT = no
MTK_VPU_SUPPORT = no
MTK_AUDIODSP_SUPPORT = no
MTK_SMC_ID_MGMT = yes
TYPEC_MT6360 = yes

MTK_ILDO_SUPPORT = yes
MTK_AB_OTA_UPDATER=yes
MTK_DYNAMIC_CCB_BUFFER_GEAR_ID=
MTK_MINIMUM_SCP_DRAM_SIZE = yes
