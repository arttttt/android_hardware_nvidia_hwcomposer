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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <drm/drm_mode.h>

#include "display/Connector.h"
#include "display/DrmMode.h"
#include "tegra/FbDevice.h"

namespace android {
namespace hwc {

/* The panel on the far end of a display head.
 *
 * Everything it knows came from the framebuffer device at start-up and none
 * of it changes afterwards: the panel is soldered to the board and is not
 * going anywhere.
 *
 * It runs one timing, at one or two rates. The second is the same timing
 * with the vertical front porch stretched until a frame takes twice as
 * long: the controller can do that between two frames with no flip and no
 * mode set, so the framework may ask for it as a seamless change within one
 * group -- and the framework, not the composer, decides when it is quiet
 * enough to ask. Offered only where the kernel can stretch the porch.
 *
 * Every settable attribute is left at the base class's answer, which is that
 * there is none -- see the note in display/Connector.h on why that is the
 * truth here rather than a gap.
 */
class TegraConnector : public drm_hwcomposer::Connector {
public:
    TegraConnector(drm_hwcomposer::Device &device, uint32_t index,
                   const PanelTiming &timing, bool stretchable)
        : mDevice(device),
          mIndex(index),
          mTiming(timing),
          mSlowMode(slowed(mTiming.mode)) {
        mModes.emplace_back(&mTiming.mode);
        if (stretchable)
            mModes.emplace_back(&mSlowMode);
    }

    /* The porch of the slow rate, in lines: measured by eye down the
     * calibration ladder of this panel, the longest at which its pixels
     * hold their charge between refreshes, and the one that makes an
     * effective thirty hertz of its mode. A property of the glass, not of
     * the system; a different panel means a different number. */
    static constexpr uint32_t kSlowVfp = 2086;

    /* The porch to ask the head for so that `mode` is what it runs: nought
     * for the panel's own timing, the mode's porch for any other. */
    static uint32_t porchFor(const drmModeModeInfo &mode,
                             const drmModeModeInfo &native) {
        const uint32_t vfp = mode.vsync_start - mode.vdisplay;
        return vfp == static_cast<uint32_t>(native.vsync_start -
                                            native.vdisplay)
                   ? 0
                   : vfp;
    }

    uint32_t GetId() const override { return mIndex; }

    drm_hwcomposer::Device &GetDev() const override { return mDevice; }

    /* The head number, which is also its position: the heads are found in
     * order and none of them goes away. */
    uint32_t GetIndexInResArray() const override { return mIndex; }

    std::string GetName() const override {
        return "DSI-" + std::to_string(mIndex);
    }

    /* A panel wired to the controller over a display serial interface. Not a
     * guess about this board so much as the only thing this controller
     * drives here: the tablet has no other output. */
    uint32_t GetConnectorType() const override {
        return DRM_MODE_CONNECTOR_DSI;
    }

    bool IsInternal() const override { return true; }
    bool IsExternal() const override { return false; }

    const std::vector<drm_hwcomposer::DrmMode> &GetModes() const override {
        return mModes;
    }

    uint32_t GetMmWidth() const override { return mTiming.mmWidth; }
    uint32_t GetMmHeight() const override { return mTiming.mmHeight; }

private:
    /* The panel's timing with the porch stretched to kSlowVfp. Everything
     * after the porch moves down with it; the clock stays, so the rate is
     * what the longer frame gives. Not preferred: the panel comes up at its
     * own rate. */
    static drmModeModeInfo slowed(const drmModeModeInfo &native) {
        drmModeModeInfo mode = native;
        const uint32_t pulse = native.vsync_end - native.vsync_start;
        const uint32_t back = native.vtotal - native.vsync_end;
        mode.vsync_start = static_cast<uint16_t>(mode.vdisplay + kSlowVfp);
        mode.vsync_end = static_cast<uint16_t>(mode.vsync_start + pulse);
        mode.vtotal = static_cast<uint16_t>(mode.vsync_end + back);
        mode.type = DRM_MODE_TYPE_DRIVER;

        const uint64_t pixels = static_cast<uint64_t>(mode.htotal) *
                                mode.vtotal;
        if (mode.clock != 0 && pixels != 0)
            mode.vrefresh = static_cast<uint32_t>(
                (static_cast<uint64_t>(mode.clock) * 1000 + pixels / 2) /
                pixels);
        else
            mode.vrefresh = native.vrefresh / 2;
        return mode;
    }

    drm_hwcomposer::Device &mDevice;
    const uint32_t mIndex;

    /* Held rather than referenced: the modes below are built from them. Not
     * const, because a mode is built from a pointer to its timing. */
    PanelTiming mTiming;
    drmModeModeInfo mSlowMode;

    /* The panel's own rate first, which is the one it comes up in, and the
     * slow rate after it where the kernel can stretch the porch. */
    std::vector<drm_hwcomposer::DrmMode> mModes;
};

}  // namespace hwc
}  // namespace android
