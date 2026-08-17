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

#include <algorithm>
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

        /* The longest side a turned copy may have, told to the plane whose
         * answer keeps such copies out of the merge -- the intermediates
         * they land in are cut no larger. The same figure the state manager
         * sizes its pool by, and it has to be: the plane promises only what
         * the pool can hold. */
        if (mScratch)
            drm_hwcomposer::TegraPlane::SetTurnReach(
                std::max(mScratch->width(), mScratch->height()));
    }

    /* The controller's own cursor, claimed through the head's descriptor.
     * Independent of the engine on purpose: a pointer is cheap precisely
     * because it involves no composition, and a device with no engine
     * deserves it all the more. */
    mCursorUnit = CursorUnit::Claim(mHead->fd(), mVic.get());
    if (mCursorUnit)
        mCursorPlane = std::make_unique<drm_hwcomposer::TegraCursorPlane>();

    /* Whether this panel can be slowed when nobody draws is the kernel's
     * to answer, once, here. Independent of everything above: a governor
     * needs only the head's descriptor and the quiet. */
    mGovernor = RefreshGovernor::Probe(mHead->fd());

    /* After the engine, and it has to be: the state manager is handed both
     * and would otherwise be handed nothing on the very boot where they were
     * wanted. */
    atomic_state_manager =
        std::make_unique<drm_hwcomposer::TegraAtomicStateManager>(
            *mHead, tegraConnector.GetModes(), mVic.get(), mScratch.get(),
            mCursorUnit.get(), mGovernor.get());

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
    /* After the manager that speaks to it, before the head it speaks
     * through: letting go restores the native rate over the head's
     * descriptor. */
    mGovernor.reset();
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
             * That is not a lie it can be caught in. What lands here must be
             * one contiguous run of the stack -- a merged buffer occupies one
             * place in it -- and the joining plan keeps that true from either
             * direction: by first fit these, coming last, take the topmost
             * layers; by steering they take the quiet run, wherever it lies.
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

    /* The merge takes one contiguous run of the stack -- by default the
     * top, by steering the quiet run wherever it lies.
     *
     * Taking the bottom UNCONDITIONALLY was tried once, on the reasoning
     * that the bottom is where the buffers short of time live -- a merged
     * layer's buffer is free the moment the engine has read it, a frame
     * before a window would let go. The panel answered: the bottom of a
     * transition is three full screens, and the engine reading all of
     * them and writing a fourth every frame cost 95 per cent of the
     * ticks to memory bandwidth. The principle wanted a mechanism that
     * could pick the changing layers out of the middle of the stack, and
     * the joining plan's steering is that mechanism: it moves the merge
     * only onto layers that hold still, so the cache answers for them
     * and the engine does not reread the screens each frame -- the very
     * cost that buried the unconditional bottom. */
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
        /* Merging is offered only while the engine, its scratch AND the
         * modes are all known -- and that is an invariant stretched across
         * three classes, not a local check. The scratch pool is created
         * only when a mode names its size (above), and the state manager
         * weighs and clips the merged window only when it has modes to
         * weigh against. Rewire any of the three and a merging plane
         * without modes would slip an unweighed, unclipped window past the
         * bandwidth question. */
        if (plane->IsCursorCandidate() && mVic && mScratch)
            plane->SetMerging();

        /* With the cursor unit claimed, the narrow window is never the
         * cursor's seat: the unit is the better cursor by construction, and
         * the window -- when the engine has not taken it -- goes into the
         * ordinary list to answer for what its capabilities honestly
         * allow. */
        if (!plane->IsCursorCandidate() || plane->IsMerging() || mCursorUnit)
            usable.first.push_back(std::move(binding));
        else if (!usable.second)
            usable.second = std::move(binding);
    }

    /* The unit takes the cursor seat itself, wearing the adapter that
     * answers the planner's one question. */
    if (mCursorPlane && !usable.second) {
        auto binding = mCursorPlane->BindPipeline(this, true);
        if (binding)
            usable.second = std::move(binding);
    }

    return usable;
}

}  // namespace hwc
}  // namespace android
