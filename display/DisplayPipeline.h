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

#ifndef DISPLAY_DISPLAY_PIPELINE_H
#define DISPLAY_DISPLAY_PIPELINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "DrmMode.h"
#include "PipelineBinding.h"
#include "VSyncSource.h"

namespace android {
namespace drm_hwcomposer {

class AtomicStateManager;
class BackendDisplayCapabilities;
class CompositionPlanner;

}  // namespace drm_hwcomposer

namespace hwc {

/* One display, and everything the composer core may ask of it.
 *
 * This is the seam the hardware lives behind. Above it nothing knows what
 * kind of display controller is present or how a frame reaches the panel;
 * below it nothing knows that HWC2 exists. A second implementation, for a
 * different controller or for testing, is a matter of implementing this and
 * nothing else.
 *
 * Upstream splits this in two. A pipeline there is the chain of DRM objects a
 * frame travels down -- connector, encoder, controller, planes -- and the
 * things built on top of that chain: a state manager, a planner, a statement
 * of what the backend can do. Separately, a connector answers what the panel
 * on the far end of the cable is: which timings it runs, how large it is, and
 * whether it is built into the machine or plugged into it.
 *
 * There is no cable here and nothing to enumerate, so both roles land on one
 * object. The core is not told which of the two it is talking to, and does
 * not need to be: it asks the same questions in the same order it asks them
 * of a DRM display.
 *
 * The pipeline owns its state manager, planner and vertical-blank source and
 * outlives all three; the references handed out stay valid for as long as the
 * pipeline does.
 */
class DisplayPipeline {
public:
    virtual ~DisplayPipeline() = default;

    /* The planes this display may put layers on, and the one meant for a
     * cursor if it has one.
     *
     * Handed out as bindings rather than plainly, so that a plane cannot end
     * up claimed by two displays at once: holding the binding is what says it
     * is this one's, and letting go is what gives it back. The shape of the
     * pair is what the planner expects to be given.
     */
    virtual drm_hwcomposer::UsablePlanes usablePlanes() const = 0;

    /* Carries out what a frame's plan describes, and answers beforehand
     * whether it could. Mode changes and power changes travel the same way,
     * as arguments to a commit rather than as calls of their own. */
    virtual drm_hwcomposer::AtomicStateManager &atomicStateManager() = 0;

    /* Decides which layers this display's planes can take and which are left
     * to the client. */
    virtual drm_hwcomposer::CompositionPlanner &planner() = 0;

    /* What this backend can do beyond what the planes report, or null where
     * it has nothing to add. Null is the ordinary answer: the base class
     * already says "no opinion" to every question. */
    virtual const drm_hwcomposer::BackendDisplayCapabilities *capabilities()
        const = 0;

    /* Human-readable, for logs and for the framework's display name. */
    virtual std::string name() const = 0;

    /* Timings the panel can run. The one marked preferred is what the display
     * comes up in. Fixed-mode panels report exactly one, which is the case on
     * this board. */
    virtual const std::vector<drm_hwcomposer::DrmMode> &modes() const = 0;

    /* Physical size of the visible area. Zero where the panel does not say,
     * which consumers must read as "no information" rather than as a display
     * of no size. */
    virtual uint32_t mmWidth() const = 0;
    virtual uint32_t mmHeight() const = 0;

    /* Whether this display is something the user plugged in. It decides which
     * of the two high-dynamic-range settings applies, and how the framework
     * is told about the display appearing and going away. */
    virtual bool isExternal() const = 0;

    virtual VSyncSource &vsyncSource() = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_DISPLAY_PIPELINE_H
