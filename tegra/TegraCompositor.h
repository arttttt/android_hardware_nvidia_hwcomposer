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

#ifndef TEGRA_COMPOSITOR_H
#define TEGRA_COMPOSITOR_H

#include <cstdint>

#include "display/Compositor.h"
#include "tegra/DcHead.h"

namespace android {
namespace hwc {

/* Shows a planned frame through the display controller.
 *
 * Turns each entry of a plan into a hardware window and posts them in one
 * flip. Everything specific to this controller ends here: what a window is,
 * how a buffer is described to it, which format codes it speaks.
 */
class TegraCompositor : public Compositor {
public:
    /* `head` outlives this object; the pipeline owns both. `panelWidth` is
     * needed to read a buffer's row length, which the allocator reports in
     * units that only make sense against the image it holds. */
    TegraCompositor(DcHead &head, uint32_t panelWidth);

    int test(const FramePlan &plan) override;
    int present(const FramePlan &plan, UniqueFd *outPresentFence) override;

private:
    /* Fills a window from one planned layer. */
    int describeWindow(const PlannedLayer &layer, uint32_t index, uint32_t z,
                       DcHead::Window *outWindow);

    /* Says in the log whether an earlier frame made it to the panel, and
     * keeps `postFence` to answer the same question later. */
    void traceFrameLanded(const UniqueFd &postFence);

    DcHead &mHead;
    const uint32_t mPanelWidth;

    /* The fences of the last two flips, newest first. Held only to be asked
     * about, and only while tracing is on. Two of them because a flip's fence
     * comes due one flip later, and the flip that would settle it is posted
     * at the very end of the present that fills this in. */
    UniqueFd mPostFences[2];
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_COMPOSITOR_H
