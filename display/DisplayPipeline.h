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

#include "Compositor.h"
#include "DisplayMode.h"
#include "VSyncSource.h"

namespace android {
namespace hwc {

enum class PowerMode {
    Off,
    On,
};

/* One display, and everything the composer core may ask of it.
 *
 * This is the seam the hardware lives behind. Above it nothing knows what
 * kind of display controller is present or how a frame reaches the panel;
 * below it nothing knows that HWC2 exists. A second implementation, for a
 * different controller or for testing, is a matter of implementing this and
 * nothing else.
 *
 * The pipeline owns its compositor and vsync source and outlives both; the
 * references handed out stay valid for as long as the pipeline does.
 */
class DisplayPipeline {
public:
    virtual ~DisplayPipeline() = default;

    /* Human-readable, for logs and for the framework's display name. */
    virtual std::string name() const = 0;

    /* Timings the panel can run, in preference order: the first entry is
     * what the display comes up in. Fixed-mode panels report exactly one,
     * which is the case on this board. */
    virtual const std::vector<DisplayMode> &modes() const = 0;

    /* Index into modes() of the timing currently driving the panel. */
    virtual size_t activeModeIndex() const = 0;
    virtual int setActiveMode(size_t index) = 0;

    virtual int setPowerMode(PowerMode mode) = 0;

    virtual Compositor &compositor() = 0;
    virtual VSyncSource &vsyncSource() = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_DISPLAY_PIPELINE_H
