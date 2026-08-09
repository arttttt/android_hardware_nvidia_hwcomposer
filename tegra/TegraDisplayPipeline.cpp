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

#include "TegraDisplayPipeline.h"

#include <errno.h>
#include <stdio.h>

#include <utility>

#include "utils/Logging.h"

#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "tegra/FbDevice.h"
#include "tegra/TegraAtomicStateManager.h"
#include "tegra/TegraCompositor.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-pipeline"

namespace android {
namespace hwc {

std::unique_ptr<TegraDisplayPipeline> TegraDisplayPipeline::create(
    TegraConnector &connector) {
    const auto index = static_cast<int>(connector.GetId());

    std::unique_ptr<DcHead> head = DcHead::open(index);
    if (!head)
        return nullptr;

    /* Both devices, because a blank takes both: the head is what is asked to
     * report them, the control device is where they come out. The head index
     * doubles as the event handle -- the controller reports blanks against
     * the same numbering the device nodes use. */
    std::unique_ptr<TegraVSyncSource> vsync =
        TegraVSyncSource::create(*head, static_cast<uint32_t>(index));
    if (!vsync)
        return nullptr;

    return std::unique_ptr<TegraDisplayPipeline>(new TegraDisplayPipeline(
        connector, std::move(head), std::move(vsync)));
}

TegraDisplayPipeline::TegraDisplayPipeline(TegraConnector &tegraConnector,
                                           std::unique_ptr<DcHead> head,
                                           std::unique_ptr<TegraVSyncSource> vsync)
    : mHead(std::move(head)),
      mVSync(std::move(vsync)),
      mCompositor(new TegraCompositor(*mHead)),
      mCrtc(tegraConnector.GetId()) {
    /* Binding is what says a piece of hardware is this display's. Nothing
     * else can claim these afterwards, and letting go of the binding is what
     * would give them back. */
    connector = tegraConnector.BindPipeline(this);
    crtc = mCrtc.BindPipeline(this);

    atomic_state_manager =
        std::make_unique<drm_hwcomposer::TegraAtomicStateManager>(
            *mHead, tegraConnector.GetModes());

    /* The planner is not built here. Which one runs is a decision the backend
     * makes from a property, and a pipeline has no business overriding it. */
}

TegraDisplayPipeline::~TegraDisplayPipeline() {
    /* Ordered, and it has to be. What a pipeline owns is declared in the base
     * class and so outlives everything declared here, while what it owns is
     * built on top of what is here: the state manager holds the head and the
     * connector's modes. Left to the ordinary order it would be reading both
     * after they were gone.
     *
     * The vertical blank reader goes first for the same reason -- it must
     * stop before the devices it reads from do. */
    mVSync.reset();
    atomic_state_manager.reset();
    planner.reset();
    connector.reset();
    crtc.reset();
    mCompositor.reset();
    mHead.reset();
}

drm_hwcomposer::UsablePlanes TegraDisplayPipeline::GetUsablePlanes() const {
    /* Made once, on the first asking. The head decides which windows are its
     * at the same moment, and what each can do does not change after. */
    if (mPlanes.empty()) {
        for (uint32_t index : mHead->windows()) {
            const DcHead::WindowCapabilities *caps = mHead->capabilities(index);
            if (caps == nullptr)
                continue;

            mPlanes.push_back(
                std::make_unique<drm_hwcomposer::TegraPlane>(index, *caps));
        }
    }

    drm_hwcomposer::UsablePlanes usable;

    for (const auto &plane : mPlanes) {
        auto binding = plane->BindPipeline(this, true);
        if (!binding)
            continue;

        /* A window that reads neither blocks nor anything resized is no use
         * for an ordinary layer -- everything the GPU draws is arranged in
         * blocks -- but it is exactly what a cursor wants, and the planner
         * looks for one. Offered as that rather than left idle. */
        if (!plane->IsCursorCandidate())
            usable.first.push_back(std::move(binding));
        else if (!usable.second)
            usable.second = std::move(binding);
    }

    return usable;
}

void TegraDisplayPipeline::setCompositor(std::unique_ptr<Compositor> compositor) {
    if (!compositor) {
        HWC_LOGE("refusing a null compositor; keeping the current one");
        return;
    }
    mCompositor = std::move(compositor);
}

}  // namespace hwc
}  // namespace android
