LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# Named after the board rather than the device: the loader asks for
# hwcomposer.<ro.board.platform>.so, and that is `tegra` here.
LOCAL_MODULE := hwcomposer.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE_TAGS := optional

# A hardware module belongs on the vendor side, next to the service that
# loads it.
LOCAL_PROPRIETARY_MODULE := true

LOCAL_CFLAGS := \
    -Wall \
    -Wextra \
    -Werror \
    -std=c++17

# Per-frame tracing: what a plan contained, which descriptors went where,
# what the hardware answered. On while the composer is being brought up; set
# to 0 for a quiet log without touching a line of code. The calls stay
# compiled either way, so they cannot rot into something that no longer
# builds.
LOCAL_CFLAGS += -DHWC_TRACE_ENABLED=1

LOCAL_C_INCLUDES := $(LOCAL_PATH)

# The display controller interface is declared in the kernel's own
# include/video/tegra_dc_ext.h. That header is GPL-2.0 and is not exported by
# headers_install, so it is reached where it lives instead of being copied
# here: including it leaves the licence with the kernel, while a copy in this
# tree would drag it into an Apache-2.0 project.
#
# Only include/video is added, never include/ itself. The kernel's own
# linux/ headers sit beside it and are not the ones libc means: putting the
# parent directory on the search path shadows bionic's uapi copies, and the
# internal versions are not even valid C++ (linux/list.h names a parameter
# `new`). Nothing in include/video collides with a libc header name.
LOCAL_C_INCLUDES += $(TARGET_KERNEL_SOURCE)/include/video

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libcutils \
    libhardware

LOCAL_SRC_FILES := \
    hwc/HwcDevice.cpp \
    hwc/HwcDisplay.cpp \
    hwc/HwcLayer.cpp \
    hwc/HwcModule.cpp \
    tegra/DcControl.cpp \
    tegra/DcHead.cpp \
    tegra/FbDevice.cpp \
    tegra/TegraDisplayPipeline.cpp \
    tegra/TegraVSyncSource.cpp

include $(BUILD_SHARED_LIBRARY)
