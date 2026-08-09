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

#include "HwcDisplay.h"

#include <errno.h>
#include <inttypes.h>

#include <utility>

#include <utils/Log.h>

#undef  LOG_TAG
#define LOG_TAG "hwc-display"

namespace android {
namespace hwc {

HwcDisplay::HwcDisplay(std::unique_ptr<DisplayPipeline> pipeline)
    : mPipeline(std::move(pipeline)) {}

HwcDisplay::~HwcDisplay() = default;

uint64_t HwcDisplay::createLayer() {
    const uint64_t id = mNextLayerId++;
    mLayers[id];
    return id;
}

int HwcDisplay::destroyLayer(uint64_t id) {
    return mLayers.erase(id) == 1 ? 0 : -EINVAL;
}

HwcLayer *HwcDisplay::layer(uint64_t id) {
    auto it = mLayers.find(id);
    return it == mLayers.end() ? nullptr : &it->second;
}

void HwcDisplay::setClientTarget(buffer_handle_t buffer, int acquireFence) {
    mClientTarget.setBuffer(buffer, acquireFence);
}

int HwcDisplay::validate(std::vector<CompositionChange> *outChanges) {
    outChanges->clear();

    /* Every layer goes to client composition.
     *
     * This is the whole of the composition decision for now, and it is a
     * correct answer rather than a placeholder: the framework draws the
     * layers into the client target and the display shows that one buffer.
     * Assigning layers to hardware windows is an optimisation on top, and
     * one that cannot be judged until frames are actually reaching the
     * panel.
     */
    for (auto &entry : mLayers) {
        HwcLayer &layer = entry.second;
        layer.setActualComposition(HwcLayer::Composition::Client);

        /* Reported only where the answer differs from the request. A layer
         * left out of this list is one the framework believes the display
         * will show by itself, so it will not draw it -- and nothing else
         * would either. */
        if (layer.requestedComposition() != HwcLayer::Composition::Client)
            outChanges->push_back({entry.first, layer.actualComposition()});
    }

    mValidated = true;
    return 0;
}

int HwcDisplay::acceptChanges() {
    if (!mValidated)
        return -EINVAL;

    for (auto &entry : mLayers)
        entry.second.setRequestedComposition(entry.second.actualComposition());

    return 0;
}

int HwcDisplay::present(UniqueFd *outPresentFence) {
    if (!mValidated) {
        ALOGE("present without a validate");
        return -EINVAL;
    }

    /* Cleared here rather than at the end, so that a failed present still
     * costs the caller a fresh validate. Retrying a present against state
     * the framework has moved on from is worse than the failure. */
    mValidated = false;

    if (mPowerMode == PowerMode::Off) {
        /* Nothing reaches the panel while it is off, and saying so is not
         * an error: the framework keeps composing during a screen-off
         * animation and expects the frames to be quietly retired. */
        outPresentFence->reset();
        return 0;
    }

    FramePlan plan;

    /* One entry, the client target, covering the whole panel. Everything the
     * framework composed is already inside it. */
    if (mClientTarget.buffer()) {
        PlannedLayer entry;
        entry.buffer = mClientTarget.buffer();
        entry.acquireFence = mClientTarget.acquireFence();

        const DisplayMode &mode = modes()[activeModeIndex()];
        entry.sourceCrop = FRect{0.f, 0.f, static_cast<float>(mode.width),
                                 static_cast<float>(mode.height)};
        entry.displayFrame = Rect{0, 0, mode.width, mode.height};
        entry.blend = BlendMode::None;

        plan.addLayer(entry);
    }

    if (plan.isEmpty()) {
        /* No client target yet. Happens between a display appearing and the
         * framework's first composition. */
        outPresentFence->reset();
        return 0;
    }

    UniqueFd presentFence;
    int err = mPipeline->compositor().present(plan, &presentFence);
    if (err)
        return err;

    /* Every layer was composed into the client target, so the framework may
     * reuse all of their buffers as soon as the frame it drew is on screen.
     * That is the same moment for all of them, and it is what the present
     * fence signals -- hence one fence duplicated per layer rather than a
     * fence each. A layer whose duplicate fails is given none, which the
     * framework reads as already free; that risks an early reuse, so the
     * failure is logged rather than passed over.
     */
    for (auto &entry : mLayers) {
        UniqueFd copy = presentFence.dup();
        if (!copy && presentFence)
            ALOGE("layer %" PRIu64 ": cannot duplicate the present fence",
                  entry.first);
        entry.second.setReleaseFence(std::move(copy));
    }

    *outPresentFence = std::move(presentFence);
    return 0;
}

std::map<uint64_t, UniqueFd> HwcDisplay::takeReleaseFences() {
    std::map<uint64_t, UniqueFd> fences;

    for (auto &entry : mLayers) {
        UniqueFd fence = entry.second.takeReleaseFence();
        if (fence)
            fences.emplace(entry.first, std::move(fence));
    }

    return fences;
}

int HwcDisplay::setPowerMode(PowerMode mode) {
    if (mode == mPowerMode)
        return 0;

    int err = mPipeline->setPowerMode(mode);
    if (err)
        return err;

    mPowerMode = mode;
    return 0;
}

}  // namespace hwc
}  // namespace android
