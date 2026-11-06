LOCAL_DIR := $(GET_LOCAL_DIR)

INCLUDES += -I$(LOCAL_DIR)/../include
INCLUDES += -I$(LOCAL_DIR)/../../$(PLATFORM)/include

OBJS += \
	$(LOCAL_DIR)/systracker.o\
	$(LOCAL_DIR)/latch.o\
	$(LOCAL_DIR)/etb.o

# The Konkr build leaves out the DFD (cache dump after a kernel crash) decoder: its register
# tables (platform/lastpc.h: little_core, big_core, mcusys, about 270 KB of the 1.09 MB LK) are
# read by the preloader at 9.5 MB/s on every boot, ~28 ms. dfd_stub.c reports no DFD dump.
ifeq ($(KONKR_KPA),yes)
OBJS += $(LOCAL_DIR)/dfd_stub.o
else
OBJS += $(LOCAL_DIR)/dfd.o
endif
