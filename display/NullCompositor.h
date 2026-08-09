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

#ifndef DISPLAY_NULL_COMPOSITOR_H
#define DISPLAY_NULL_COMPOSITOR_H

#include "Compositor.h"

namespace android {
namespace hwc {

/* Accepts every frame and shows none of them.
 *
 * Not a stub left behind by unfinished work: it separates two questions that
 * would otherwise fail together. Does the composer answer the framework
 * correctly -- the function table, the display it reports, the validate and
 * present handshake, the vertical blank the framework paces itself by? And
 * separately, does a frame reach the panel?
 *
 * With this in place the first question can be answered on its own. The
 * system boots, the framework runs, logs and a shell are reachable, and the
 * screen stays black. Everything else on the device becomes testable while
 * the flip is still being written, and when the flip does arrive, a failure
 * belongs to it rather than to either half.
 *
 * Every frame is retired immediately, with no fence: the framework reads an
 * empty fence as "already done", which for a frame nobody displayed is the
 * truth.
 */
class NullCompositor : public Compositor {
public:
    int test(const FramePlan &) override { return 0; }

    int present(const FramePlan &, UniqueFd *outPresentFence) override {
        outPresentFence->reset();
        return 0;
    }
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_NULL_COMPOSITOR_H
