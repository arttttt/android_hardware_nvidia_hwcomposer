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
 * of it changes afterwards: the panel is soldered to the board, runs one
 * timing, and is not going anywhere.
 *
 * Every settable attribute is left at the base class's answer, which is that
 * there is none -- see the note in display/Connector.h on why that is the
 * truth here rather than a gap.
 */
class TegraConnector : public drm_hwcomposer::Connector {
public:
    TegraConnector(uint32_t index, const PanelTiming &timing)
        : mIndex(index),
          mTiming(timing),
          mModes{drm_hwcomposer::DrmMode(&mTiming.mode)} {}

    uint32_t GetId() const override { return mIndex; }

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
    const uint32_t mIndex;

    /* Held rather than referenced: the modes below are built from it. Not
     * const, because a mode is built from a pointer to it. */
    PanelTiming mTiming;

    /* Exactly one. The panel has a single timing; the framework still wants
     * a list, so it gets one of length one. */
    std::vector<drm_hwcomposer::DrmMode> mModes;
};

}  // namespace hwc
}  // namespace android
