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
# The AYANEO_DEBUG_LOGGING build keeps our own _dprintf traces (e.g. the "BC:"
# multicore lines) but leaves DEBUGLEVEL=0 so the per-frame display/render
# dprintf(INFO) spam stays out of the log. Build with `AYANEO_VERBOSE_LOG=yes`
# to raise DEBUGLEVEL and restore that full, noisy LK logging when needed.
AYANEO_VERBOSE_LOG ?= no
ifeq ($(AYANEO_VERBOSE_LOG),yes)
DEFINES += AYANEO_VERBOSE_LOG
endif
# Experimental 2nd-CPU-core bring-up (emu/snes/bigcore.c). OFF by default so the
# menu always boots; build with `AYANEO_BIGCORE_EXPT=yes` (implies debug logging)
# to activate the current bigcore experiment (arm the SPMC via the KERNEL_BOOT SiP,
# then PSCI CPU_ON). Requires AYANEO_DEBUG_LOGGING for the _dprintf traces.
AYANEO_BIGCORE_EXPT ?= no
ifeq ($(AYANEO_BIGCORE_EXPT),yes)
DEFINES += AYANEO_BIGCORE_EXPT
DEFINES += AYANEO_DEBUG_LOGGING
endif
# Sub-experiment of AYANEO_BIGCORE_EXPT: map the shared handoff region Normal-WB
# NON-shareable instead of Device, testing whether a non-snoop-admitted worker can
# use it CACHED with software clean/invalidate coherency (MULTICORE_RESEARCH.md
# candidate fix #1). Implies the bigcore experiment + debug logging.
AYANEO_BC_NONSHARE ?= no
ifeq ($(AYANEO_BC_NONSHARE),yes)
DEFINES += AYANEO_BC_NONSHARE
DEFINES += AYANEO_BIGCORE_EXPT
DEFINES += AYANEO_DEBUG_LOGGING
endif

# Sub-experiment: warm-cycle the worker (self PSCI CPU_OFF on first bringup, cpu0
# re-powers it) to test whether a warm DSU re-join establishes coherency the cold
# first-join did not (MULTICORE_RESEARCH.md warm-cycle experiment).
AYANEO_BC_WARMCYCLE ?= no
ifeq ($(AYANEO_BC_WARMCYCLE),yes)
DEFINES += AYANEO_BC_WARMCYCLE
DEFINES += AYANEO_BIGCORE_EXPT
DEFINES += AYANEO_DEBUG_LOGGING
endif

# Sub-experiment: GIC/SGI channel viability. cpu0 sets SGI#1 pending in cpu1's
# GICR via MMIO each frame; the worker polls GICR_ISPENDR0 via MMIO and counts.
# Tests whether MMIO (peripheral path) is a working cpu0->worker channel despite
# the dead DSU-snoop DRAM path (MULTICORE_RESEARCH.md GIC/SGI lead).
AYANEO_BC_SGI ?= no
ifeq ($(AYANEO_BC_SGI),yes)
DEFINES += AYANEO_BC_SGI
DEFINES += AYANEO_BIGCORE_EXPT
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
