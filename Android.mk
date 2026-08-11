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

# The composer header keeps its C++ half behind these, and every user of it
# turns them on -- the enumerations and the name-printing are what the code
# above is written against, not the bare C typedefs.
LOCAL_CFLAGS += \
    -DHWC2_USE_CPP11 \
    -DHWC2_INCLUDE_STRINGIFICATION

# Up to and including Android 9 the framework deals with a display appearing
# while it is still inside the call that registered its callbacks, on that
# same thread, and asks the composer about the display before returning.
# Later releases take the news away and deal with it afterwards. The composer
# has to know which, because it decides how far its own lock may extend around
# announcing a display; both behaviours are compiled either way. See
# utils/FrameworkTraits.h.
ifeq ($(shell test $(PLATFORM_SDK_VERSION) -le 28 && echo reentrant),reentrant)
LOCAL_CFLAGS += -DHWC_FRAMEWORK_HOTPLUG_IS_REENTRANT=1
else
LOCAL_CFLAGS += -DHWC_FRAMEWORK_HOTPLUG_IS_REENTRANT=0
endif

# Per-frame tracing: what a plan contained, which descriptors went where,
# what the hardware answered. Driven from the device tree with
#
#     TARGET_HWC_TRACE := true
#
# in BoardConfig.mk, so the decision sits with whoever is building the image
# rather than in this repository. The calls stay compiled either way, so
# turning tracing off cannot silently break it.
#
# This only decides whether the tracing is built. Whether it runs is decided
# on the device with `setprop vendor.hwc.trace 1`, and the answer is no unless
# asked -- a build that carries the tracing should not pay for it. See the
# note in utils/Logging.h for what it costs when it does run.
ifeq ($(TARGET_HWC_TRACE),true)
LOCAL_CFLAGS += -DHWC_TRACE_ENABLED=1
else
LOCAL_CFLAGS += -DHWC_TRACE_ENABLED=0
endif

LOCAL_C_INCLUDES := $(LOCAL_PATH)

# Before the platform's own, and deliberately: this release ships version 2.3
# of the composer interface and the code above expects 2.4. See compat/README
# for why the newer header is copied whole rather than added to.
LOCAL_C_INCLUDES += $(LOCAL_PATH)/compat

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

# Format codes, layout modifiers and the way a display timing is written down.
# Only headers are wanted -- nothing here talks to a display through DRM,
# because this kernel has no such driver for the display controller. It is the
# vocabulary that is shared, not the path, and no libdrm function is called.
#
# Three directories rather than one: `include` for <drm/drm_fourcc.h> as this
# tree writes it, the top level for <xf86drmMode.h>, and `include/drm` because
# that header includes <drm.h> and <drm_mode.h> unqualified.
LOCAL_C_INCLUDES += \
    external/libdrm \
    external/libdrm/include \
    external/libdrm/include/drm

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libbase \
    libcutils \
    libhardware \
    libsync \
    libui \
    libdl

# Statically, and that is the point rather than a preference. It is here for a
# handful of helper calls in the property model -- adding one to an atomic
# request, reading a blob -- and for nothing else: no display on this board is
# reached through DRM and no device node is opened. Nothing else on this device
# needs it either, so it is not in the image, and a shared dependency on it is
# a module that will not load.
LOCAL_STATIC_LIBRARIES := \
    libdrm

LOCAL_SRC_FILES := \
    backend/BackendManager.cpp \
    backend/ClientBackend.cpp \
    backend/GenericBackend.cpp \
    bufferinfo/BufferInfoGetter.cpp \
    bufferinfo/GrallocBufferHandle.cpp \
    bufferinfo/NvGralloc.cpp \
    bufferinfo/legacy/BufferInfoNvidia.cpp \
    compositor/CompositionPlanner.cpp \
    compositor/FlatteningController.cpp \
    compositor/FlatteningEventAtomReporter.cpp \
    compositor/FrameTimeHistory.cpp \
    compositor/GenericCompositionPlanner.cpp \
    compositor/GenericLayerMapperCompositionPlanner.cpp \
    compositor/HdcpController.cpp \
    compositor/LayerToPlaneJoiningPlan.cpp \
    compositor/PresentedCompositionCache.cpp \
    compositor/ShortCircuitor.cpp \
    compositor/mapper/CursorLayerMapper.cpp \
    compositor/mapper/ForceClientCompositionLayerMapper.cpp \
    compositor/mapper/LayerCachingMapper.cpp \
    compositor/mapper/LeftoverLayerMapper.cpp \
    compositor/mapper/MapperUtils.cpp \
    compositor/mapper/UnderlayMapper.cpp \
    display/DrmMode.cpp \
    display/DrmProperty.cpp \
    display/VSyncWorker.cpp \
    stats/CompositionStatsAtomReporter.cpp \
    stats/CountActiveDisplaysReporter.cpp \
    stats/DisplayConfigurationResultReporter.cpp \
    stats/DisplayHotplugConnectModeDetectedAtomReporter.cpp \
    stats/DisplayRefreshRatesChangedAtomReporter.cpp \
    stats/Stats.cpp \
    stats/StatsPoller.cpp \
    utils/BacklightController.cpp \
    utils/ColorUtil.cpp \
    utils/Logging.cpp \
    utils/SysfsBacklightController.cpp \
    utils/fd.cpp \
    utils/properties.cpp \
    hwc/Hwc.cpp \
    hwc/HwcDisplay.cpp \
    hwc/HwcDisplayConfigs.cpp \
    hwc/HwcLayer.cpp \
    hwc2_device/DrmHwcTwo.cpp \
    hwc2_device/hwc2_device.cpp \
    tegra/DcControl.cpp \
    tegra/DcHead.cpp \
    tegra/TegraAtomicCommitSink.cpp \
    tegra/TegraAtomicStateManager.cpp \
    tegra/TegraBackend.cpp \
    tegra/TegraDevice.cpp \
    tegra/TegraFormat.cpp \
    tegra/TegraPlane.cpp \
    tegra/FbDevice.cpp \
    tegra/TegraDisplayPipeline.cpp \
    tegra/TegraVSyncSource.cpp \
    tegra/VicSession.cpp

include $(BUILD_SHARED_LIBRARY)
