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

#ifndef HWC_HWC_DEVICE_H
#define HWC_HWC_DEVICE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include <hardware/hwcomposer2.h>

#include "hwc/HwcDisplay.h"

namespace android {
namespace hwc {

/* The composer as the framework sees it.
 *
 * hwc2_device_t is a C structure holding one entry point, getFunction, that
 * hands back the rest by name. This owns the displays and answers those
 * calls; the two callbacks the framework registers, hotplug and vertical
 * blank, are dispatched from here as well.
 *
 * Everything is serialised on one lock. The framework calls into a composer
 * from several threads -- its own composition thread, its event thread, and
 * whatever calls setPowerMode -- and the cost of a lock is nothing beside a
 * flip, so there is no case for finer locking. The exception is the vertical
 * blank callback, which is invoked without the lock held, since it runs on
 * the event reader's thread and the framework may call straight back in.
 */
class HwcDevice : public hwc2_device_t {
public:
    HwcDevice();
    ~HwcDevice();

    /* Builds the displays. Separate from the constructor because it opens
     * devices and can fail, and a half-built composer is worse than one that
     * refused to load. */
    int init();

private:
    /* The framework addresses displays by an opaque identifier. There is one
     * physical display on this board and it is always this. */
    static constexpr hwc2_display_t kPrimaryDisplay = 1;

    static void getCapabilitiesHook(struct hwc2_device *device, uint32_t *count,
                                    int32_t *capabilities);
    static hwc2_function_pointer_t getFunctionHook(struct hwc2_device *device,
                                                   int32_t descriptor);

    HwcDisplay *display(hwc2_display_t id);

    /* Device level. */
    int32_t registerCallback(int32_t descriptor, hwc2_callback_data_t data,
                             hwc2_function_pointer_t function);
    int32_t createVirtualDisplay(uint32_t width, uint32_t height,
                                 int32_t *format, hwc2_display_t *outDisplay);
    int32_t destroyVirtualDisplay(hwc2_display_t display);
    uint32_t getMaxVirtualDisplayCount();
    void dump(uint32_t *outSize, char *outBuffer);

    /* Display level. */
    int32_t acceptDisplayChanges(hwc2_display_t display);
    int32_t createLayer(hwc2_display_t display, hwc2_layer_t *outLayer);
    int32_t destroyLayer(hwc2_display_t display, hwc2_layer_t layer);
    int32_t getActiveConfig(hwc2_display_t display, hwc2_config_t *outConfig);
    int32_t getChangedCompositionTypes(hwc2_display_t display,
                                       uint32_t *outNumElements,
                                       hwc2_layer_t *outLayers,
                                       int32_t *outTypes);
    int32_t getClientTargetSupport(hwc2_display_t display, uint32_t width,
                                   uint32_t height, int32_t format,
                                   int32_t dataspace);
    int32_t getColorModes(hwc2_display_t display, uint32_t *outNumModes,
                          int32_t *outModes);
    int32_t getDisplayAttribute(hwc2_display_t display, hwc2_config_t config,
                                int32_t attribute, int32_t *outValue);
    int32_t getDisplayConfigs(hwc2_display_t display, uint32_t *outNumConfigs,
                              hwc2_config_t *outConfigs);
    int32_t getDisplayName(hwc2_display_t display, uint32_t *outSize,
                           char *outName);
    int32_t getDisplayRequests(hwc2_display_t display,
                               int32_t *outDisplayRequests,
                               uint32_t *outNumElements, hwc2_layer_t *outLayers,
                               int32_t *outLayerRequests);
    int32_t getDisplayType(hwc2_display_t display, int32_t *outType);
    int32_t getDozeSupport(hwc2_display_t display, int32_t *outSupport);
    int32_t getHdrCapabilities(hwc2_display_t display, uint32_t *outNumTypes,
                               int32_t *outTypes, float *outMaxLuminance,
                               float *outMaxAverageLuminance,
                               float *outMinLuminance);
    int32_t getReleaseFences(hwc2_display_t display, uint32_t *outNumElements,
                             hwc2_layer_t *outLayers, int32_t *outFences);
    int32_t presentDisplay(hwc2_display_t display, int32_t *outPresentFence);
    int32_t setActiveConfig(hwc2_display_t display, hwc2_config_t config);
    int32_t setClientTarget(hwc2_display_t display, buffer_handle_t target,
                            int32_t acquireFence, int32_t dataspace,
                            hwc_region_t damage);
    int32_t setColorMode(hwc2_display_t display, int32_t mode);
    int32_t setColorTransform(hwc2_display_t display, const float *matrix,
                              int32_t hint);
    int32_t setOutputBuffer(hwc2_display_t display, buffer_handle_t buffer,
                            int32_t releaseFence);
    int32_t setPowerMode(hwc2_display_t display, int32_t mode);
    int32_t setVsyncEnabled(hwc2_display_t display, int32_t enabled);
    int32_t validateDisplay(hwc2_display_t display, uint32_t *outNumTypes,
                            uint32_t *outNumRequests);

    /* Layer level. */
    int32_t setCursorPosition(hwc2_display_t display, hwc2_layer_t layer,
                              int32_t x, int32_t y);
    int32_t setLayerBlendMode(hwc2_display_t display, hwc2_layer_t layer,
                              int32_t mode);
    int32_t setLayerBuffer(hwc2_display_t display, hwc2_layer_t layer,
                           buffer_handle_t buffer, int32_t acquireFence);
    int32_t setLayerColor(hwc2_display_t display, hwc2_layer_t layer,
                          hwc_color_t color);
    int32_t setLayerCompositionType(hwc2_display_t display, hwc2_layer_t layer,
                                    int32_t type);
    int32_t setLayerDataspace(hwc2_display_t display, hwc2_layer_t layer,
                              int32_t dataspace);
    int32_t setLayerDisplayFrame(hwc2_display_t display, hwc2_layer_t layer,
                                 hwc_rect_t frame);
    int32_t setLayerPlaneAlpha(hwc2_display_t display, hwc2_layer_t layer,
                               float alpha);
    int32_t setLayerSidebandStream(hwc2_display_t display, hwc2_layer_t layer,
                                   const native_handle_t *stream);
    int32_t setLayerSourceCrop(hwc2_display_t display, hwc2_layer_t layer,
                               hwc_frect_t crop);
    int32_t setLayerSurfaceDamage(hwc2_display_t display, hwc2_layer_t layer,
                                  hwc_region_t damage);
    int32_t setLayerTransform(hwc2_display_t display, hwc2_layer_t layer,
                              int32_t transform);
    int32_t setLayerVisibleRegion(hwc2_display_t display, hwc2_layer_t layer,
                                  hwc_region_t visible);
    int32_t setLayerZOrder(hwc2_display_t display, hwc2_layer_t layer,
                           uint32_t z);

    /* Tells the framework about the displays that already exist. Never
     * called with the lock held; see the definition. */
    void announceDisplays();

    /* The one entry point with its own trampoline, because it has work to do
     * after the lock is dropped. */
    static int32_t registerCallbackHook(hwc2_device_t *device,
                                        int32_t descriptor,
                                        hwc2_callback_data_t data,
                                        hwc2_function_pointer_t function);

    /* Called from the vertical blank reader's thread. */
    void onVSync(int64_t timestampNs);

    std::mutex mMutex;

    std::map<hwc2_display_t, std::unique_ptr<HwcDisplay>> mDisplays;

    hwc2_callback_data_t mHotplugData = nullptr;
    HWC2_PFN_HOTPLUG mHotplug = nullptr;

    hwc2_callback_data_t mVsyncData = nullptr;
    HWC2_PFN_VSYNC mVsync = nullptr;

    /* Layers already answered for in the last validate, kept so that the
     * framework's follow-up queries return the same set. */
    std::vector<hwc2_layer_t> mChangedLayers;
    std::vector<int32_t> mChangedTypes;

    /* Taken from the display on the counting call to getReleaseFences and
     * held until the filling call arrives. Asking the display twice would
     * hand the fences over on the first ask and nothing on the second. */
    std::map<uint64_t, UniqueFd> mPendingReleaseFences;

    /* Whether the framework wants blanks right now. The reader runs either
     * way; this only gates delivery. */
    bool mVsyncEnabled = false;

    /* Adapters from the C entry points to the member functions above.
     *
     * Forty-three functions written out by hand would be forty-three chances
     * to mistype a signature, and the compiler would catch none of them: the
     * table stores everything as one opaque pointer type. These three
     * templates plus the check in asHook turn that into a compile error
     * instead, and the lock is taken in one place rather than in every
     * function.
     */
    static HwcDevice *toDevice(hwc2_device_t *device) {
        return static_cast<HwcDevice *>(device);
    }

    template <auto method, typename... Args>
    static auto deviceHook(hwc2_device_t *device, Args... args) {
        HwcDevice *self = toDevice(device);
        std::lock_guard<std::mutex> lock(self->mMutex);
        return (self->*method)(args...);
    }

    template <auto method, typename... Args>
    static int32_t displayHook(hwc2_device_t *device, hwc2_display_t display,
                               Args... args) {
        HwcDevice *self = toDevice(device);
        std::lock_guard<std::mutex> lock(self->mMutex);
        return (self->*method)(display, args...);
    }

    template <auto method, typename... Args>
    static int32_t layerHook(hwc2_device_t *device, hwc2_display_t display,
                             hwc2_layer_t layer, Args... args) {
        HwcDevice *self = toDevice(device);
        std::lock_guard<std::mutex> lock(self->mMutex);
        return (self->*method)(display, layer, args...);
    }

    /* Checks the adapter against the signature the framework will call it
     * through, then erases the type as the table requires. */
    template <typename Pfn, typename Fn>
    static hwc2_function_pointer_t asHook(Fn function) {
        static_assert(std::is_same<Pfn, Fn>::value,
                      "hook does not match the function it is registered as");
        return reinterpret_cast<hwc2_function_pointer_t>(function);
    }
};

}  // namespace hwc
}  // namespace android

#endif  // HWC_HWC_DEVICE_H
