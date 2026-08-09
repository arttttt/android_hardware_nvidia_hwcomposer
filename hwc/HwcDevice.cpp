/*
 * Copyright (C) 2026 Artem Bambalov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "HwcDevice.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <utility>

#include <utils/Log.h>

#include "tegra/TegraDisplayPipeline.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-device"

namespace android {
namespace hwc {

namespace {

/* The panel is the only display, and the head that drives it is the first. */
constexpr int kPrimaryHeadIndex = 0;

HwcLayer::Composition toComposition(int32_t type) {
    switch (type) {
    case HWC2_COMPOSITION_CLIENT:
        return HwcLayer::Composition::Client;
    case HWC2_COMPOSITION_DEVICE:
        return HwcLayer::Composition::Device;
    default:
        /* Solid colour, cursor and sideband are all things the framework can
         * draw itself, so an unsupported request becomes a request for
         * client composition rather than a refusal. */
        return HwcLayer::Composition::Client;
    }
}

int32_t fromComposition(HwcLayer::Composition type) {
    return type == HwcLayer::Composition::Device ? HWC2_COMPOSITION_DEVICE
                                                 : HWC2_COMPOSITION_CLIENT;
}

BlendMode toBlendMode(int32_t mode) {
    switch (mode) {
    case HWC2_BLEND_MODE_PREMULTIPLIED:
        return BlendMode::Premultiplied;
    case HWC2_BLEND_MODE_COVERAGE:
        return BlendMode::Coverage;
    default:
        return BlendMode::None;
    }
}

}  // namespace

HwcDevice::HwcDevice() {
    common.tag = HARDWARE_DEVICE_TAG;
    common.version = HWC_DEVICE_API_VERSION_2_0;
    common.module = nullptr;
    common.close = nullptr;

    getCapabilities = getCapabilitiesHook;
    getFunction = getFunctionHook;
}

HwcDevice::~HwcDevice() = default;

int HwcDevice::init() {
    std::unique_ptr<TegraDisplayPipeline> pipeline =
        TegraDisplayPipeline::create(kPrimaryHeadIndex);
    if (!pipeline) {
        ALOGE("no display on head %d", kPrimaryHeadIndex);
        return -ENODEV;
    }

    /* Kept before the pipeline is moved into the display, because the vsync
     * source belongs to the pipeline and is reached through it. */
    VSyncSource &vsync = pipeline->vsyncSource();

    mDisplays[kPrimaryDisplay] =
        std::unique_ptr<HwcDisplay>(new HwcDisplay(std::move(pipeline)));

    vsync.enable([this](int64_t timestampNs) { onVSync(timestampNs); });

    ALOGI("composer up, %zu display(s)", mDisplays.size());
    return 0;
}

HwcDisplay *HwcDevice::display(hwc2_display_t id) {
    auto it = mDisplays.find(id);
    return it == mDisplays.end() ? nullptr : it->second.get();
}

void HwcDevice::onVSync(int64_t timestampNs) {
    /* Copied under the lock, called outside it: the framework answers a
     * blank by calling straight back in, and holding the lock across that
     * would deadlock on the first frame. */
    HWC2_PFN_VSYNC callback = nullptr;
    hwc2_callback_data_t data = nullptr;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        callback = mVsync;
        data = mVsyncData;
    }

    if (callback)
        callback(data, kPrimaryDisplay, timestampNs);
}

/* ------------------------------------------------------------------ device */

int32_t HwcDevice::registerCallback(int32_t descriptor,
                                    hwc2_callback_data_t data,
                                    hwc2_function_pointer_t function) {
    switch (descriptor) {
    case HWC2_CALLBACK_HOTPLUG: {
        mHotplugData = data;
        mHotplug = reinterpret_cast<HWC2_PFN_HOTPLUG>(function);

        /* The framework learns about displays only from this callback, and
         * ours exists before it registers. So it is announced the moment
         * there is someone to tell.
         *
         * Called with the lock held, which is safe only because the
         * framework's hotplug handler does not call back into the composer
         * before this returns -- unlike vsync, which does. */
        if (mHotplug) {
            for (const auto &entry : mDisplays)
                mHotplug(mHotplugData, entry.first, HWC2_CONNECTION_CONNECTED);
        }
        return HWC2_ERROR_NONE;
    }
    case HWC2_CALLBACK_VSYNC:
        mVsyncData = data;
        mVsync = reinterpret_cast<HWC2_PFN_VSYNC>(function);
        return HWC2_ERROR_NONE;
    case HWC2_CALLBACK_REFRESH:
        /* Accepted and never used. It exists for composers that discover
         * they need the framework to redraw; this one always shows what it
         * was given. */
        return HWC2_ERROR_NONE;
    default:
        return HWC2_ERROR_BAD_PARAMETER;
    }
}

int32_t HwcDevice::createVirtualDisplay(uint32_t, uint32_t, int32_t *,
                                        hwc2_display_t *) {
    return HWC2_ERROR_NO_RESOURCES;
}

int32_t HwcDevice::destroyVirtualDisplay(hwc2_display_t) {
    return HWC2_ERROR_BAD_DISPLAY;
}

uint32_t HwcDevice::getMaxVirtualDisplayCount() {
    /* None. A virtual display is composition into a buffer rather than onto
     * a panel, which is the framework's own job here. */
    return 0;
}

void HwcDevice::dump(uint32_t *outSize, char *outBuffer) {
    if (outBuffer == nullptr) {
        *outSize = 0;
        return;
    }
    *outSize = 0;
}

/* ----------------------------------------------------------------- display */

int32_t HwcDevice::acceptDisplayChanges(hwc2_display_t displayId) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    return disp->acceptChanges() == 0 ? HWC2_ERROR_NONE
                                      : HWC2_ERROR_NOT_VALIDATED;
}

int32_t HwcDevice::createLayer(hwc2_display_t displayId,
                               hwc2_layer_t *outLayer) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    *outLayer = disp->createLayer();
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::destroyLayer(hwc2_display_t displayId, hwc2_layer_t layer) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    return disp->destroyLayer(layer) == 0 ? HWC2_ERROR_NONE
                                          : HWC2_ERROR_BAD_LAYER;
}

int32_t HwcDevice::getActiveConfig(hwc2_display_t displayId,
                                   hwc2_config_t *outConfig) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    *outConfig = static_cast<hwc2_config_t>(disp->activeModeIndex());
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getChangedCompositionTypes(hwc2_display_t displayId,
                                              uint32_t *outNumElements,
                                              hwc2_layer_t *outLayers,
                                              int32_t *outTypes) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    /* Asked twice: once with null buffers for the count, once to fill them.
     * The answer has to be the same both times, so it is what validate
     * recorded rather than anything recomputed here. */
    if (outLayers == nullptr || outTypes == nullptr) {
        *outNumElements = static_cast<uint32_t>(mChangedLayers.size());
        return HWC2_ERROR_NONE;
    }

    const size_t count =
        std::min<size_t>(*outNumElements, mChangedLayers.size());
    for (size_t i = 0; i < count; ++i) {
        outLayers[i] = mChangedLayers[i];
        outTypes[i] = mChangedTypes[i];
    }
    *outNumElements = static_cast<uint32_t>(count);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getClientTargetSupport(hwc2_display_t displayId,
                                          uint32_t width, uint32_t height,
                                          int32_t /*format*/,
                                          int32_t /*dataspace*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    /* The client target is scanned out as it is, so it has to match the
     * panel exactly. Scaling it would mean a composition pass this composer
     * does not have. */
    const DisplayMode &mode = disp->modes()[disp->activeModeIndex()];
    if (static_cast<int32_t>(width) != mode.width ||
        static_cast<int32_t>(height) != mode.height)
        return HWC2_ERROR_UNSUPPORTED;

    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getColorModes(hwc2_display_t displayId,
                                 uint32_t *outNumModes, int32_t *outModes) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    if (outModes == nullptr) {
        *outNumModes = 1;
        return HWC2_ERROR_NONE;
    }

    if (*outNumModes >= 1)
        outModes[0] = HAL_COLOR_MODE_NATIVE;
    *outNumModes = 1;
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getDisplayAttribute(hwc2_display_t displayId,
                                       hwc2_config_t config, int32_t attribute,
                                       int32_t *outValue) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (config >= disp->modes().size())
        return HWC2_ERROR_BAD_CONFIG;

    const DisplayMode &mode = disp->modes()[config];

    switch (attribute) {
    case HWC2_ATTRIBUTE_WIDTH:
        *outValue = mode.width;
        return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_HEIGHT:
        *outValue = mode.height;
        return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_VSYNC_PERIOD:
        *outValue = mode.vsyncPeriodNs;
        return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_DPI_X:
        *outValue = mode.dpiX;
        return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_DPI_Y:
        *outValue = mode.dpiY;
        return HWC2_ERROR_NONE;
    default:
        return HWC2_ERROR_BAD_PARAMETER;
    }
}

int32_t HwcDevice::getDisplayConfigs(hwc2_display_t displayId,
                                     uint32_t *outNumConfigs,
                                     hwc2_config_t *outConfigs) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    const size_t available = disp->modes().size();

    if (outConfigs == nullptr) {
        *outNumConfigs = static_cast<uint32_t>(available);
        return HWC2_ERROR_NONE;
    }

    const size_t count = std::min<size_t>(*outNumConfigs, available);
    for (size_t i = 0; i < count; ++i)
        outConfigs[i] = static_cast<hwc2_config_t>(i);
    *outNumConfigs = static_cast<uint32_t>(count);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getDisplayName(hwc2_display_t displayId, uint32_t *outSize,
                                  char *outName) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    const std::string name = disp->pipeline().name();

    if (outName == nullptr) {
        *outSize = static_cast<uint32_t>(name.size());
        return HWC2_ERROR_NONE;
    }

    const size_t count = std::min<size_t>(*outSize, name.size());
    memcpy(outName, name.c_str(), count);
    *outSize = static_cast<uint32_t>(count);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getDisplayRequests(hwc2_display_t displayId,
                                      int32_t *outDisplayRequests,
                                      uint32_t *outNumElements,
                                      hwc2_layer_t * /*outLayers*/,
                                      int32_t * /*outLayerRequests*/) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    /* Requests are how a composer asks the framework to change what it drew,
     * such as clearing a region the hardware will cover. Nothing here does,
     * because everything is composed by the framework already. */
    *outDisplayRequests = 0;
    *outNumElements = 0;
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getDisplayType(hwc2_display_t displayId, int32_t *outType) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    *outType = HWC2_DISPLAY_TYPE_PHYSICAL;
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getDozeSupport(hwc2_display_t displayId,
                                  int32_t *outSupport) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    /* The panel has two states. Doze would mean keeping it lit at low power
     * with the controller mostly idle, which this hardware does not offer
     * through any interface reachable from here. */
    *outSupport = 0;
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getHdrCapabilities(hwc2_display_t displayId,
                                      uint32_t *outNumTypes, int32_t *outTypes,
                                      float *outMaxLuminance,
                                      float *outMaxAverageLuminance,
                                      float *outMinLuminance) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    *outNumTypes = 0;
    if (outTypes == nullptr) {
        /* The luminance figures are returned on the counting call too, and
         * the framework reads them even when there are no types. */
        if (outMaxLuminance)
            *outMaxLuminance = 0.f;
        if (outMaxAverageLuminance)
            *outMaxAverageLuminance = 0.f;
        if (outMinLuminance)
            *outMinLuminance = 0.f;
    }
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::getReleaseFences(hwc2_display_t displayId,
                                    uint32_t *outNumElements,
                                    hwc2_layer_t *outLayers, int32_t *outFences) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    /* Taken once, on the counting call, and held until the filling call
     * arrives. Asking the display twice would hand back the fences on the
     * first ask and nothing on the second. */
    if (outLayers == nullptr || outFences == nullptr) {
        mPendingReleaseFences = disp->takeReleaseFences();
        *outNumElements = static_cast<uint32_t>(mPendingReleaseFences.size());
        return HWC2_ERROR_NONE;
    }

    size_t index = 0;
    for (auto &entry : mPendingReleaseFences) {
        if (index >= *outNumElements)
            break;
        outLayers[index] = entry.first;
        /* Ownership passes to the framework, which closes them. */
        outFences[index] = entry.second.release();
        ++index;
    }

    *outNumElements = static_cast<uint32_t>(index);
    mPendingReleaseFences.clear();
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::presentDisplay(hwc2_display_t displayId,
                                  int32_t *outPresentFence) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    UniqueFd fence;
    int err = disp->present(&fence);
    if (err == -EINVAL)
        return HWC2_ERROR_NOT_VALIDATED;
    if (err)
        return HWC2_ERROR_NO_RESOURCES;

    *outPresentFence = fence.release();
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setActiveConfig(hwc2_display_t displayId,
                                   hwc2_config_t config) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    return disp->setActiveMode(config) == 0 ? HWC2_ERROR_NONE
                                            : HWC2_ERROR_BAD_CONFIG;
}

int32_t HwcDevice::setClientTarget(hwc2_display_t displayId,
                                   buffer_handle_t target, int32_t acquireFence,
                                   int32_t /*dataspace*/,
                                   hwc_region_t /*damage*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp) {
        /* The descriptor was handed over, so it is ours to close even on the
         * path where nothing else happens. */
        if (acquireFence >= 0)
            close(acquireFence);
        return HWC2_ERROR_BAD_DISPLAY;
    }

    disp->setClientTarget(target, acquireFence);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setColorMode(hwc2_display_t displayId, int32_t mode) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    return mode == HAL_COLOR_MODE_NATIVE ? HWC2_ERROR_NONE
                                         : HWC2_ERROR_UNSUPPORTED;
}

int32_t HwcDevice::setColorTransform(hwc2_display_t displayId,
                                     const float * /*matrix*/, int32_t hint) {
    if (!display(displayId))
        return HWC2_ERROR_BAD_DISPLAY;

    /* Anything but the identity would have to be applied while compositing,
     * and nothing here composites. Refusing it makes the framework apply the
     * transform itself when it draws the client target. */
    return hint == HAL_COLOR_TRANSFORM_IDENTITY ? HWC2_ERROR_NONE
                                                : HWC2_ERROR_UNSUPPORTED;
}

int32_t HwcDevice::setOutputBuffer(hwc2_display_t /*display*/,
                                   buffer_handle_t /*buffer*/,
                                   int32_t releaseFence) {
    if (releaseFence >= 0)
        close(releaseFence);

    /* Only virtual displays have an output buffer, and there are none. */
    return HWC2_ERROR_BAD_DISPLAY;
}

int32_t HwcDevice::setPowerMode(hwc2_display_t displayId, int32_t mode) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    PowerMode target;
    switch (mode) {
    case HWC2_POWER_MODE_OFF:
        target = PowerMode::Off;
        break;
    case HWC2_POWER_MODE_ON:
        target = PowerMode::On;
        break;
    case HWC2_POWER_MODE_DOZE:
    case HWC2_POWER_MODE_DOZE_SUSPEND:
        /* Consistent with getDozeSupport saying no. Accepting it and lighting
         * the panel fully would be worse than a clear refusal. */
        return HWC2_ERROR_UNSUPPORTED;
    default:
        return HWC2_ERROR_BAD_PARAMETER;
    }

    return disp->setPowerMode(target) == 0 ? HWC2_ERROR_NONE
                                           : HWC2_ERROR_UNSUPPORTED;
}

int32_t HwcDevice::setVsyncEnabled(hwc2_display_t displayId, int32_t enabled) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    /* The reader runs from init to teardown; this only gates delivery.
     * Starting and stopping a thread at the framework's rate would cost more
     * than the events it would save, and the controller keeps producing
     * blanks either way. */
    switch (enabled) {
    case HWC2_VSYNC_ENABLE:
        mVsyncEnabled = true;
        return HWC2_ERROR_NONE;
    case HWC2_VSYNC_DISABLE:
        mVsyncEnabled = false;
        return HWC2_ERROR_NONE;
    default:
        return HWC2_ERROR_BAD_PARAMETER;
    }
}

int32_t HwcDevice::validateDisplay(hwc2_display_t displayId,
                                   uint32_t *outNumTypes,
                                   uint32_t *outNumRequests) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;

    std::vector<HwcDisplay::CompositionChange> changes;
    int err = disp->validate(&changes);
    if (err)
        return HWC2_ERROR_BAD_DISPLAY;

    /* Recorded here so that the two calls that follow, counting then
     * filling, answer from one decision rather than two. */
    mChangedLayers.clear();
    mChangedTypes.clear();
    for (const auto &change : changes) {
        mChangedLayers.push_back(change.layer);
        mChangedTypes.push_back(fromComposition(change.composition));
    }

    *outNumTypes = static_cast<uint32_t>(mChangedLayers.size());

    /* No display requests are ever made, so nothing follows from them. */
    *outNumRequests = 0;

    return HWC2_ERROR_NONE;
}

/* ------------------------------------------------------------------- layer */

int32_t HwcDevice::setCursorPosition(hwc2_display_t displayId,
                                     hwc2_layer_t layer, int32_t /*x*/,
                                     int32_t /*y*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!disp->layer(layer))
        return HWC2_ERROR_BAD_LAYER;

    /* No cursor layer is ever accepted, so there is no position to move. */
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerBlendMode(hwc2_display_t displayId,
                                     hwc2_layer_t layerId, int32_t mode) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setBlendMode(toBlendMode(mode));
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerBuffer(hwc2_display_t displayId,
                                  hwc2_layer_t layerId, buffer_handle_t buffer,
                                  int32_t acquireFence) {
    HwcDisplay *disp = display(displayId);
    HwcLayer *layer = disp ? disp->layer(layerId) : nullptr;
    if (!layer) {
        if (acquireFence >= 0)
            close(acquireFence);
        return disp ? HWC2_ERROR_BAD_LAYER : HWC2_ERROR_BAD_DISPLAY;
    }

    layer->setBuffer(buffer, acquireFence);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerColor(hwc2_display_t displayId, hwc2_layer_t layerId,
                                 hwc_color_t /*color*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!disp->layer(layerId))
        return HWC2_ERROR_BAD_LAYER;

    /* Only meaningful for a solid colour layer, which validate never
     * accepts; the framework draws those itself. */
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerCompositionType(hwc2_display_t displayId,
                                           hwc2_layer_t layerId, int32_t type) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setRequestedComposition(toComposition(type));
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerDataspace(hwc2_display_t displayId,
                                     hwc2_layer_t layerId,
                                     int32_t /*dataspace*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!disp->layer(layerId))
        return HWC2_ERROR_BAD_LAYER;

    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerDisplayFrame(hwc2_display_t displayId,
                                        hwc2_layer_t layerId,
                                        hwc_rect_t frame) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setDisplayFrame(Rect{frame.left, frame.top, frame.right,
                                frame.bottom});
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerPlaneAlpha(hwc2_display_t displayId,
                                      hwc2_layer_t layerId, float alpha) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setPlaneAlpha(alpha);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerSidebandStream(hwc2_display_t displayId,
                                          hwc2_layer_t layerId,
                                          const native_handle_t * /*stream*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!disp->layer(layerId))
        return HWC2_ERROR_BAD_LAYER;

    /* A sideband stream is video reaching the display without passing
     * through the framework at all. That needs the hardware path this
     * composer has not built yet. */
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t HwcDevice::setLayerSourceCrop(hwc2_display_t displayId,
                                      hwc2_layer_t layerId, hwc_frect_t crop) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setSourceCrop(FRect{crop.left, crop.top, crop.right, crop.bottom});
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerSurfaceDamage(hwc2_display_t displayId,
                                         hwc2_layer_t layerId,
                                         hwc_region_t /*damage*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!disp->layer(layerId))
        return HWC2_ERROR_BAD_LAYER;

    /* Damage says which part of a layer changed, so a composer can repaint
     * less. Every frame here is a whole buffer scanned out, so there is
     * nothing to narrow. */
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerTransform(hwc2_display_t displayId,
                                     hwc2_layer_t layerId, int32_t transform) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setTransform(transform);
    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerVisibleRegion(hwc2_display_t displayId,
                                         hwc2_layer_t layerId,
                                         hwc_region_t /*visible*/) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!disp->layer(layerId))
        return HWC2_ERROR_BAD_LAYER;

    return HWC2_ERROR_NONE;
}

int32_t HwcDevice::setLayerZOrder(hwc2_display_t displayId,
                                  hwc2_layer_t layerId, uint32_t z) {
    HwcDisplay *disp = display(displayId);
    if (!disp)
        return HWC2_ERROR_BAD_DISPLAY;
    HwcLayer *layer = disp->layer(layerId);
    if (!layer)
        return HWC2_ERROR_BAD_LAYER;

    layer->setZOrder(z);
    return HWC2_ERROR_NONE;
}

/* ------------------------------------------------------------------- table */

void HwcDevice::getCapabilitiesHook(struct hwc2_device * /*device*/,
                                    uint32_t *count,
                                    int32_t * /*capabilities*/) {
    /* None claimed. The two that exist announce that the composer can skip
     * validate, or that it handles a sideband stream; neither is true. */
    *count = 0;
}

hwc2_function_pointer_t HwcDevice::getFunctionHook(struct hwc2_device *device,
                                                   int32_t descriptor) {
    (void)device;

    switch (static_cast<hwc2_function_descriptor_t>(descriptor)) {
    /* Device. */
    case HWC2_FUNCTION_REGISTER_CALLBACK:
        return asHook<HWC2_PFN_REGISTER_CALLBACK>(
            deviceHook<&HwcDevice::registerCallback, int32_t,
                       hwc2_callback_data_t, hwc2_function_pointer_t>);
    case HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY:
        return asHook<HWC2_PFN_CREATE_VIRTUAL_DISPLAY>(
            deviceHook<&HwcDevice::createVirtualDisplay, uint32_t, uint32_t,
                       int32_t *, hwc2_display_t *>);
    case HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY:
        return asHook<HWC2_PFN_DESTROY_VIRTUAL_DISPLAY>(
            deviceHook<&HwcDevice::destroyVirtualDisplay, hwc2_display_t>);
    case HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT:
        return asHook<HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT>(
            deviceHook<&HwcDevice::getMaxVirtualDisplayCount>);
    case HWC2_FUNCTION_DUMP:
        return asHook<HWC2_PFN_DUMP>(
            deviceHook<&HwcDevice::dump, uint32_t *, char *>);

    /* Display. */
    case HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES:
        return asHook<HWC2_PFN_ACCEPT_DISPLAY_CHANGES>(
            displayHook<&HwcDevice::acceptDisplayChanges>);
    case HWC2_FUNCTION_CREATE_LAYER:
        return asHook<HWC2_PFN_CREATE_LAYER>(
            displayHook<&HwcDevice::createLayer, hwc2_layer_t *>);
    case HWC2_FUNCTION_DESTROY_LAYER:
        return asHook<HWC2_PFN_DESTROY_LAYER>(
            displayHook<&HwcDevice::destroyLayer, hwc2_layer_t>);
    case HWC2_FUNCTION_GET_ACTIVE_CONFIG:
        return asHook<HWC2_PFN_GET_ACTIVE_CONFIG>(
            displayHook<&HwcDevice::getActiveConfig, hwc2_config_t *>);
    case HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES:
        return asHook<HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES>(
            displayHook<&HwcDevice::getChangedCompositionTypes, uint32_t *,
                        hwc2_layer_t *, int32_t *>);
    case HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT:
        return asHook<HWC2_PFN_GET_CLIENT_TARGET_SUPPORT>(
            displayHook<&HwcDevice::getClientTargetSupport, uint32_t, uint32_t,
                        int32_t, int32_t>);
    case HWC2_FUNCTION_GET_COLOR_MODES:
        return asHook<HWC2_PFN_GET_COLOR_MODES>(
            displayHook<&HwcDevice::getColorModes, uint32_t *, int32_t *>);
    case HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE:
        return asHook<HWC2_PFN_GET_DISPLAY_ATTRIBUTE>(
            displayHook<&HwcDevice::getDisplayAttribute, hwc2_config_t, int32_t,
                        int32_t *>);
    case HWC2_FUNCTION_GET_DISPLAY_CONFIGS:
        return asHook<HWC2_PFN_GET_DISPLAY_CONFIGS>(
            displayHook<&HwcDevice::getDisplayConfigs, uint32_t *,
                        hwc2_config_t *>);
    case HWC2_FUNCTION_GET_DISPLAY_NAME:
        return asHook<HWC2_PFN_GET_DISPLAY_NAME>(
            displayHook<&HwcDevice::getDisplayName, uint32_t *, char *>);
    case HWC2_FUNCTION_GET_DISPLAY_REQUESTS:
        return asHook<HWC2_PFN_GET_DISPLAY_REQUESTS>(
            displayHook<&HwcDevice::getDisplayRequests, int32_t *, uint32_t *,
                        hwc2_layer_t *, int32_t *>);
    case HWC2_FUNCTION_GET_DISPLAY_TYPE:
        return asHook<HWC2_PFN_GET_DISPLAY_TYPE>(
            displayHook<&HwcDevice::getDisplayType, int32_t *>);
    case HWC2_FUNCTION_GET_DOZE_SUPPORT:
        return asHook<HWC2_PFN_GET_DOZE_SUPPORT>(
            displayHook<&HwcDevice::getDozeSupport, int32_t *>);
    case HWC2_FUNCTION_GET_HDR_CAPABILITIES:
        return asHook<HWC2_PFN_GET_HDR_CAPABILITIES>(
            displayHook<&HwcDevice::getHdrCapabilities, uint32_t *, int32_t *,
                        float *, float *, float *>);
    case HWC2_FUNCTION_GET_RELEASE_FENCES:
        return asHook<HWC2_PFN_GET_RELEASE_FENCES>(
            displayHook<&HwcDevice::getReleaseFences, uint32_t *,
                        hwc2_layer_t *, int32_t *>);
    case HWC2_FUNCTION_PRESENT_DISPLAY:
        return asHook<HWC2_PFN_PRESENT_DISPLAY>(
            displayHook<&HwcDevice::presentDisplay, int32_t *>);
    case HWC2_FUNCTION_SET_ACTIVE_CONFIG:
        return asHook<HWC2_PFN_SET_ACTIVE_CONFIG>(
            displayHook<&HwcDevice::setActiveConfig, hwc2_config_t>);
    case HWC2_FUNCTION_SET_CLIENT_TARGET:
        return asHook<HWC2_PFN_SET_CLIENT_TARGET>(
            displayHook<&HwcDevice::setClientTarget, buffer_handle_t, int32_t,
                        int32_t, hwc_region_t>);
    case HWC2_FUNCTION_SET_COLOR_MODE:
        return asHook<HWC2_PFN_SET_COLOR_MODE>(
            displayHook<&HwcDevice::setColorMode, int32_t>);
    case HWC2_FUNCTION_SET_COLOR_TRANSFORM:
        return asHook<HWC2_PFN_SET_COLOR_TRANSFORM>(
            displayHook<&HwcDevice::setColorTransform, const float *, int32_t>);
    case HWC2_FUNCTION_SET_OUTPUT_BUFFER:
        return asHook<HWC2_PFN_SET_OUTPUT_BUFFER>(
            displayHook<&HwcDevice::setOutputBuffer, buffer_handle_t, int32_t>);
    case HWC2_FUNCTION_SET_POWER_MODE:
        return asHook<HWC2_PFN_SET_POWER_MODE>(
            displayHook<&HwcDevice::setPowerMode, int32_t>);
    case HWC2_FUNCTION_SET_VSYNC_ENABLED:
        return asHook<HWC2_PFN_SET_VSYNC_ENABLED>(
            displayHook<&HwcDevice::setVsyncEnabled, int32_t>);
    case HWC2_FUNCTION_VALIDATE_DISPLAY:
        return asHook<HWC2_PFN_VALIDATE_DISPLAY>(
            displayHook<&HwcDevice::validateDisplay, uint32_t *, uint32_t *>);

    /* Layer. */
    case HWC2_FUNCTION_SET_CURSOR_POSITION:
        return asHook<HWC2_PFN_SET_CURSOR_POSITION>(
            layerHook<&HwcDevice::setCursorPosition, int32_t, int32_t>);
    case HWC2_FUNCTION_SET_LAYER_BLEND_MODE:
        return asHook<HWC2_PFN_SET_LAYER_BLEND_MODE>(
            layerHook<&HwcDevice::setLayerBlendMode, int32_t>);
    case HWC2_FUNCTION_SET_LAYER_BUFFER:
        return asHook<HWC2_PFN_SET_LAYER_BUFFER>(
            layerHook<&HwcDevice::setLayerBuffer, buffer_handle_t, int32_t>);
    case HWC2_FUNCTION_SET_LAYER_COLOR:
        return asHook<HWC2_PFN_SET_LAYER_COLOR>(
            layerHook<&HwcDevice::setLayerColor, hwc_color_t>);
    case HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE:
        return asHook<HWC2_PFN_SET_LAYER_COMPOSITION_TYPE>(
            layerHook<&HwcDevice::setLayerCompositionType, int32_t>);
    case HWC2_FUNCTION_SET_LAYER_DATASPACE:
        return asHook<HWC2_PFN_SET_LAYER_DATASPACE>(
            layerHook<&HwcDevice::setLayerDataspace, int32_t>);
    case HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME:
        return asHook<HWC2_PFN_SET_LAYER_DISPLAY_FRAME>(
            layerHook<&HwcDevice::setLayerDisplayFrame, hwc_rect_t>);
    case HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA:
        return asHook<HWC2_PFN_SET_LAYER_PLANE_ALPHA>(
            layerHook<&HwcDevice::setLayerPlaneAlpha, float>);
    case HWC2_FUNCTION_SET_LAYER_SIDEBAND_STREAM:
        return asHook<HWC2_PFN_SET_LAYER_SIDEBAND_STREAM>(
            layerHook<&HwcDevice::setLayerSidebandStream,
                      const native_handle_t *>);
    case HWC2_FUNCTION_SET_LAYER_SOURCE_CROP:
        return asHook<HWC2_PFN_SET_LAYER_SOURCE_CROP>(
            layerHook<&HwcDevice::setLayerSourceCrop, hwc_frect_t>);
    case HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE:
        return asHook<HWC2_PFN_SET_LAYER_SURFACE_DAMAGE>(
            layerHook<&HwcDevice::setLayerSurfaceDamage, hwc_region_t>);
    case HWC2_FUNCTION_SET_LAYER_TRANSFORM:
        return asHook<HWC2_PFN_SET_LAYER_TRANSFORM>(
            layerHook<&HwcDevice::setLayerTransform, int32_t>);
    case HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION:
        return asHook<HWC2_PFN_SET_LAYER_VISIBLE_REGION>(
            layerHook<&HwcDevice::setLayerVisibleRegion, hwc_region_t>);
    case HWC2_FUNCTION_SET_LAYER_Z_ORDER:
        return asHook<HWC2_PFN_SET_LAYER_Z_ORDER>(
            layerHook<&HwcDevice::setLayerZOrder, uint32_t>);

    default:
        /* Anything the framework asks for that is not here is answered with
         * nothing, which it reads as unsupported. */
        return nullptr;
    }
}

}  // namespace hwc
}  // namespace android
