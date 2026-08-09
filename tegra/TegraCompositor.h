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
    /* `head` outlives this object; the pipeline owns both. */
    explicit TegraCompositor(DcHead &head);

    int test(const FramePlan &plan) override;
    int present(const FramePlan &plan, UniqueFd *outPresentFence) override;

private:
    /* Fills a window from one planned layer. The window borrows the fence it
     * is to wait on; `outFence` owns it and must outlive the flip. */
    int describeWindow(const PlannedLayer &layer, uint32_t index, uint32_t z,
                       DcHead::Window *outWindow, UniqueFd *outFence);

    DcHead &mHead;

    /* The fence the previous flip handed back, which is the one that comes
     * due when the next flip lands. Held so the next present can pass it on
     * as its own; see present for why that shift is the whole point. */
    UniqueFd mPreviousPostFence;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_COMPOSITOR_H
