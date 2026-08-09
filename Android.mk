LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := libhwc_tegra
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true

LOCAL_CFLAGS := \
    -Wall \
    -Wextra \
    -Werror \
    -std=c++17

LOCAL_C_INCLUDES := $(LOCAL_PATH)

# The display controller interface is declared in the kernel's own
# include/video/tegra_dc_ext.h. That header is GPL-2.0 and is not exported by
# headers_install, so it is reached where it lives instead of being copied
# here: including it leaves the licence with the kernel, while a copy in this
# tree would drag it into an Apache-2.0 project.
LOCAL_C_INCLUDES += $(TARGET_KERNEL_SOURCE)/include

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libcutils

LOCAL_SRC_FILES := \
    tegra/DcControl.cpp

include $(BUILD_STATIC_LIBRARY)
