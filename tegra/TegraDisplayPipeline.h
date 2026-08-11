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

#ifndef TEGRA_DISPLAY_PIPELINE_H
#define TEGRA_DISPLAY_PIPELINE_H

#include <memory>
#include <vector>

#include "display/DisplayPipeline.h"
#include "tegra/DcHead.h"
#include "tegra/FbDevice.h"
#include "tegra/TegraConnector.h"
#include "tegra/TegraCrtc.h"
#include "tegra/TegraPlane.h"
#include "tegra/ScratchPool.h"
#include "tegra/TegraVSyncSource.h"
#include "tegra/VicSession.h"

namespace android {
namespace hwc {

/* One Tegra display head, assembled into the chain a frame travels down.
 *
 * Three devices answer for one display and none of them answers for all of
 * it: the head node posts frames, the control node carries events, and the
 * framebuffer device knows the timing and the backlight. This is where that
 * is hidden, so that above it a display is a pipeline like any other.
 */
class TegraDisplayPipeline : public drm_hwcomposer::DisplayPipeline {
public:
    /* Opens the head `connector` is the panel of. Returns null on failure,
     * having logged what failed.
     *
     * The connector is not taken: it belongs to the device and outlives every
     * pipeline that binds to it. What is built here is the rest of the chain
     * and the things that hang off it. */
    static std::unique_ptr<TegraDisplayPipeline> create(
        TegraConnector &connector);

    ~TegraDisplayPipeline() override;

    drm_hwcomposer::UsablePlanes GetUsablePlanes() const override;

    VSyncSource &GetVSyncSource() override { return *mVSync; }

    DcHead &head() { return *mHead; }

private:
    TegraDisplayPipeline(TegraConnector &connector,
                         std::unique_ptr<DcHead> head,
                         std::unique_ptr<TegraVSyncSource> vsync);

    std::unique_ptr<DcHead> mHead;
    std::unique_ptr<TegraVSyncSource> mVSync;

    /* The engine that can merge layers the windows have no room for, or null
     * on a device that was not asked for it -- which is every device until
     * someone sets the property, and will stay that way until it has been
     * shown to put the right pixels on the panel.
     *
     * Belongs to the display rather than to a frame: a session is a channel
     * to the hardware, and opening one per frame would be paying for the
     * channel sixty times a second to use it once. */
    std::unique_ptr<VicSession> mVic;

    /* Three, which is what the display pipeline holds anyway: one being
     * shown, one waiting to be, one being written. Two would work and would
     * stall whenever a frame ran late; four would only cost another screen of
     * memory. */
    static constexpr size_t kScratchBuffers = 3;

    /* Where the engine writes. Made only alongside it, and the two live and
     * die together -- see the constructor. */
    std::unique_ptr<ScratchPool> mScratch;

    /* Owned here and bound in the constructor, unlike the connector, which
     * belongs to the device. A head is not shared between displays and has
     * nothing to say once its display is gone. */
    TegraCrtc mCrtc;

    /* One per window the head owns, made the first time anyone asks which
     * planes this display has. Held here because a plane belongs to the
     * display rather than to a frame, and what a planner is handed points at
     * these. Mutable because being asked is not a change worth calling one. */
    mutable std::vector<std::unique_ptr<drm_hwcomposer::TegraPlane>> mPlanes;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DISPLAY_PIPELINE_H
