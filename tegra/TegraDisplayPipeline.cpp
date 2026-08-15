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

#include <cutils/properties.h>

#include <iterator>
#include <utility>

#include "utils/Logging.h"

#include "tegra/FbDevice.h"
#include "tegra/TegraAtomicStateManager.h"

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

    /* Lit before it is asked anything.
     *
     * At boot this changes nothing: the panel is already on, with the boot
     * animation on it. It matters the second time round. A composer outlives
     * the SurfaceFlinger it serves, and a SurfaceFlinger blanks the panel on
     * its way out -- so the next one to arrive inherits a dark controller.
     *
     * A dark controller is not merely idle, it is misleading. It describes its
     * windows as reading no formats and being no pixels wide, and it refuses
     * every flip. Worse, nothing later undoes it: the state manager starts out
     * believing the panel is on, so the very request that would have lit it is
     * taken for a no-op, and the display stays dark for as long as the process
     * lives.
     *
     * Whatever power mode SurfaceFlinger actually wants arrives moments later
     * and is obeyed. This only makes the starting point true.
     */
    setPanelPowered(index, true);

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
      mCrtc(tegraConnector.GetId()) {
    /* Binding is what says a piece of hardware is this display's. Nothing
     * else can claim these afterwards, and letting go of the binding is what
     * would give them back. */
    connector = tegraConnector.BindPipeline(this);
    crtc = mCrtc.BindPipeline(this);

    /* Asked for once, here, rather than the first time a frame needs it: a
     * failure to reach the engine is a fact about the device, and finding it
     * out while assembling a frame would mean finding it out sixty times a
     * second. Null is not a fault -- it is what every device says until
     * someone asks for the engine by name. */
    mVic = VicSession::Create();

    /* And somewhere for it to write, sized to the panel. Only when there is
     * an engine to write it: three screens of memory is not a thing to hold
     * on a device that will never merge anything. */
    if (mVic) {
        const auto &modes = tegraConnector.GetModes();
        if (!modes.empty()) {
            const auto &mode = modes.front().GetRawMode();
            mScratch = ScratchPool::Create(mode.hdisplay, mode.vdisplay,
                                           kScratchBuffers);
        }

        /* An engine with nowhere to write is no more use than no engine, and
         * leaving it open would say otherwise to everything downstream. */
        if (!mScratch)
            mVic.reset();
    }

    /* After the engine, and it has to be: the state manager is handed both
     * and would otherwise be handed nothing on the very boot where they were
     * wanted. */
    atomic_state_manager =
        std::make_unique<drm_hwcomposer::TegraAtomicStateManager>(
            *mHead, tegraConnector.GetModes(), mVic.get(), mScratch.get());

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
    mVic.reset();
    mScratch.reset();
    planner.reset();
    connector.reset();
    crtc.reset();
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

            /* The merging window is offered several times over.
             *
             * A planner gives one layer to one plane, and there is no way to
             * tell it that a plane takes more -- so it is told there are more
             * planes. All of these name the same window, and everything that
             * lands on any of them is drawn into the one buffer that window
             * shows.
             *
             * That is not a lie it can be caught in. Planes are handed out in
             * the order they appear here, so these, coming last, take the
             * topmost layers -- which is exactly the group a single buffer can
             * hold, because a merged buffer occupies one place in the stack
             * and anything above it would have to be inside it.
             *
             * As many as the engine draws in one pass and no more.
             */
            if (mVic && mScratch && caps->pitchLayout && !caps->scaling) {
                for (size_t i = 1; i < VicSession::kMaxLayers; i++)
                    mPlanes.push_back(
                        std::make_unique<drm_hwcomposer::TegraPlane>(index,
                                                                     *caps));
            }
        }
    }

    drm_hwcomposer::UsablePlanes usable;

    /* The merge takes the top of the stack: the topmost layers are the ones
     * a single buffer can hold without anything having to be inside it.
     *
     * Taking the bottom instead was tried, on the reasoning that the bottom
     * is where the buffers short of time live -- a merged layer's buffer is
     * free the moment the engine has read it, a frame before a window would
     * let go. The panel answered: the bottom of a transition is three full
     * screens, and the engine reading all of them and writing a fourth every
     * frame cost 95 per cent of the ticks to memory bandwidth. The principle
     * wants a mechanism that can pick the changing layers out of the middle
     * of the stack; this one can only take a run from one end, so it takes
     * the end that is cheap. */
    for (const auto &plane : mPlanes) {
        auto binding = plane->BindPipeline(this, true);
        if (!binding)
            continue;

        /* A window that reads neither blocks nor anything resized is no use
         * for an ordinary layer -- everything the GPU draws is arranged in
         * blocks. What becomes of it depends on whether there is an engine to
         * draw for it.
         *
         * With one, it is the most valuable window on the controller rather
         * than the least: what lands there is composed into a buffer of our
         * own first, and that buffer is by construction the one shape this
         * window can show. Offered as an ordinary plane, and the limits it
         * answers by become the engine's.
         *
         * Without one, it is what it has always been -- the small unscaled
         * thing a cursor wants, which is the only use it has. This tablet has
         * no cursor, so that is a use in name only, but naming it is still
         * better than leaving the window unaccounted for. */
        if (plane->IsCursorCandidate() && mVic && mScratch)
            plane->SetMerging();

        if (!plane->IsCursorCandidate() || plane->IsMerging())
            usable.first.push_back(std::move(binding));
        else if (!usable.second)
            usable.second = std::move(binding);
    }

    return usable;
}

}  // namespace hwc
}  // namespace android
