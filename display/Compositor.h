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

#ifndef DISPLAY_COMPOSITOR_H
#define DISPLAY_COMPOSITOR_H

#include "FramePlan.h"
#include "utils/UniqueFd.h"

namespace android {
namespace hwc {

/* Puts a planned frame on the panel.
 *
 * Two operations, and the split between them is the whole reason this is an
 * interface. `test` answers whether the hardware could show a plan, changing
 * nothing; `present` shows it. The framework asks the first question during
 * validate and only then commits, so a backend that cannot honour a plan must
 * be able to say so before anything is displayed.
 */
class Compositor {
public:
    virtual ~Compositor() = default;

    /* Can this plan be shown as described? Returns 0 if yes.
     *
     * A negative errno means the plan is beyond the hardware: too many
     * layers, a scaling factor out of range, an unsupported transform. The
     * caller answers that by moving layers into client composition and
     * planning again. Nothing about the display changes either way.
     */
    virtual int test(const FramePlan &plan) = 0;

    /* Shows the plan.
     *
     * On success returns 0 and, in `outPresentFence`, a fence that fires when
     * the frame has actually reached the panel and the previous frame's
     * buffers are free. The caller owns that descriptor. An empty fence is a
     * valid result and means the frame was already retired.
     */
    virtual int present(const FramePlan &plan, UniqueFd *outPresentFence) = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_COMPOSITOR_H
