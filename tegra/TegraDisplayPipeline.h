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
#include <string>
#include <vector>

#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "display/Compositor.h"
#include "display/DisplayPipeline.h"
#include "tegra/DcHead.h"
#include "tegra/FbDevice.h"
#include "tegra/TegraAtomicStateManager.h"
#include "tegra/TegraPlane.h"
#include "tegra/TegraVSyncSource.h"

namespace android {
namespace hwc {

/* One Tegra display head, assembled.
 *
 * Three devices answer for one display and none of them answers for all of
 * it: the head node posts frames, the control node carries events, and the
 * framebuffer device knows the timing and the backlight. This is where that
 * is hidden, so that above it a display is one object with modes, power and
 * a planner.
 */
class TegraDisplayPipeline : public DisplayPipeline {
public:
    /* Opens head `index` and reads its panel timing. Returns null if either
     * fails, having logged which. */
    static std::unique_ptr<TegraDisplayPipeline> create(int index);

    ~TegraDisplayPipeline() override;

    drm_hwcomposer::UsablePlanes usablePlanes() const override;

    drm_hwcomposer::AtomicStateManager &atomicStateManager() override {
        return *mStateManager;
    }

    drm_hwcomposer::CompositionPlanner &planner() override {
        return *mPlanner;
    }

    /* Nothing to add beyond what the windows already report. The planner asks
     * each window whether it can take a layer, and this controller has no
     * further say that is not answered there. */
    const drm_hwcomposer::BackendDisplayCapabilities *capabilities()
        const override {
        return nullptr;
    }

    std::string name() const override;

    const std::vector<drm_hwcomposer::DrmMode> &modes() const override {
        return mModes;
    }

    uint32_t mmWidth() const override { return mTiming.mmWidth; }
    uint32_t mmHeight() const override { return mTiming.mmHeight; }

    /* Soldered to the board. */
    bool isExternal() const override { return false; }

    VSyncSource &vsyncSource() override { return *mVSync; }

    DcHead &head() { return *mHead; }

    /* The frame path this composer came up on, before plans and planes.
     *
     * Kept, and kept working, though nothing above asks for it any more: it
     * is the one path on this hardware that has been seen to put a correct
     * frame on the panel, and it costs a reference to hold on to. Where the
     * new path is in doubt, this is what it is compared against.
     */
    Compositor &compositor() { return *mCompositor; }
    void setCompositor(std::unique_ptr<Compositor> compositor);

private:
    TegraDisplayPipeline(int index, std::unique_ptr<DcHead> head,
                         std::unique_ptr<TegraVSyncSource> vsync,
                         const PanelTiming &timing);

    const int mIndex;

    std::unique_ptr<DcHead> mHead;
    std::unique_ptr<TegraVSyncSource> mVSync;
    std::unique_ptr<Compositor> mCompositor;

    /* What the panel reported, kept as it arrived. The modes list below is
     * built from it; the physical size is answered straight from here. */
    PanelTiming mTiming;

    /* Exactly one. The panel is fixed and has a single timing; the framework
     * still wants a list, so it gets one of length one. */
    std::vector<drm_hwcomposer::DrmMode> mModes;

    /* Declared after the modes it holds a reference to, so that it is built
     * second and destroyed first. */
    std::unique_ptr<drm_hwcomposer::TegraAtomicStateManager> mStateManager;
    std::unique_ptr<drm_hwcomposer::CompositionPlanner> mPlanner;

    /* One per window the head owns, made the first time anyone asks which
     * planes this display has. Held here because a plane belongs to the
     * display rather than to a frame, and what a planner is handed points at
     * these. Mutable because being asked is not a change worth calling one. */
    mutable std::vector<std::unique_ptr<drm_hwcomposer::TegraPlane>> mPlanes;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DISPLAY_PIPELINE_H
