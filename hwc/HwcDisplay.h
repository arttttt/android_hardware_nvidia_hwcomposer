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

#ifndef HWC_HWC_DISPLAY_H
#define HWC_HWC_DISPLAY_H

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <cutils/native_handle.h>

#include "display/DisplayPipeline.h"
#include "hwc/HwcLayer.h"
#include "utils/UniqueFd.h"

namespace android {
namespace hwc {

/* One display as the framework addresses it.
 *
 * Holds the layers, runs the frame through validate and present, and keeps
 * the small amount of state those two steps share. It knows nothing about
 * the hardware beyond the pipeline it was handed.
 *
 * The frame is a two-step handshake and the order is not advisory. The
 * framework validates, learns which layers it must draw itself, accepts
 * that answer, and only then presents. Presenting without a validate is an
 * error the framework relies on us to report, because the alternative is
 * showing a frame composed on assumptions nobody agreed to.
 */
class HwcDisplay {
public:
    explicit HwcDisplay(std::unique_ptr<DisplayPipeline> pipeline);
    ~HwcDisplay();

    HwcDisplay(const HwcDisplay &) = delete;
    HwcDisplay &operator=(const HwcDisplay &) = delete;

    /* Layers, addressed by the identifiers the framework was given. */
    uint64_t createLayer();
    int destroyLayer(uint64_t id);
    HwcLayer *layer(uint64_t id);

    /* The buffer the framework drew the client-composed layers into, and the
     * fence telling us when that drawing is finished. */
    void setClientTarget(buffer_handle_t buffer, int acquireFence);

    /* One layer whose composition was decided differently from the request. */
    struct CompositionChange {
        uint64_t layer;
        HwcLayer::Composition composition;
    };

    /* Decides where every layer will be composited.
     *
     * Fills `outChanges` with the layers given an answer differing from what
     * was asked for. The framework reads that list, accepts it, and only
     * then draws -- so a layer missing from it is a layer the framework
     * believes the display will show, and which nothing will.
     */
    int validate(std::vector<CompositionChange> *outChanges);

    /* Agrees to the answer validate gave. */
    int acceptChanges();

    /* Shows the frame. On success `outPresentFence` holds a fence that fires
     * when it has reached the panel, and ownership passes to the caller. */
    int present(UniqueFd *outPresentFence);

    /* Per-layer fences, valid only after a successful present. */
    std::map<uint64_t, UniqueFd> takeReleaseFences();

    const std::vector<DisplayMode> &modes() const { return mPipeline->modes(); }
    size_t activeModeIndex() const { return mPipeline->activeModeIndex(); }
    int setActiveMode(size_t index) { return mPipeline->setActiveMode(index); }

    int setPowerMode(PowerMode mode);
    PowerMode powerMode() const { return mPowerMode; }

    DisplayPipeline &pipeline() { return *mPipeline; }

private:
    std::unique_ptr<DisplayPipeline> mPipeline;

    std::map<uint64_t, HwcLayer> mLayers;
    uint64_t mNextLayerId = 1;

    /* The client target is a layer in every way that matters here, so it is
     * one: same buffer, same acquire fence, same ownership rules. */
    HwcLayer mClientTarget;

    /* Set by validate, cleared by present. What it guards is not a state
     * machine for its own sake: presenting on a stale validate would show a
     * frame composed against a layer set the framework has since changed. */
    bool mValidated = false;

    PowerMode mPowerMode = PowerMode::Off;
};

}  // namespace hwc
}  // namespace android

#endif  // HWC_HWC_DISPLAY_H
