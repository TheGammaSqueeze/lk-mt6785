LOCAL_DIR := $(GET_LOCAL_DIR)

INCLUDES += -I$(LOCAL_DIR)/include
INCLUDES += -Iplatform/$(PLATFORM)/include/platform

OBJS += $(LOCAL_DIR)/boot_info.o
OBJS += $(LOCAL_DIR)/boot_opt.o
OBJS += $(LOCAL_DIR)/vboot_state.o

ifeq ($(MTK_SECURITY_ANTI_ROLLBACK),yes)
OBJS += $(LOCAL_DIR)/anti_rollback_util.o
endif

ifeq ($(MTK_AVB20_SUPPORT),yes)
MODULES += $(LOCAL_DIR)/avb20
else
MODULES += $(LOCAL_DIR)/legacy
endif

ifeq ($(MTK_BUILD_DEFAULT_UNLOCK),yes)
DEFINES += MTK_BUILD_DEFAULT_UNLOCK
endif

# Boot header versions 3 and 4 with a vendor_boot partition and the bootconfig block (Android 11
# and later kernels); a board whose firmware boots this way sets VENDOR_BOOT_SUPPORT := yes.
ifeq ($(VENDOR_BOOT_SUPPORT),yes)
DEFINES += VENDOR_BOOT_SUPPORT
OBJS += $(LOCAL_DIR)/bootimg_v3.o
OBJS += $(LOCAL_DIR)/kinfo_dt.o
endif
