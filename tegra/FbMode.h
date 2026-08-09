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

#ifndef TEGRA_FB_MODE_H
#define TEGRA_FB_MODE_H

#include "display/DisplayMode.h"

namespace android {
namespace hwc {

/* Reads the panel timing for head `index` from /dev/graphics/fbN.
 *
 * The display controller's own interface has no way to ask what the panel is
 * doing: tegra_dc_ext posts frames and reports events, and every question
 * about resolution or refresh is answered by the framebuffer device that sits
 * on the same hardware. So the mode comes from there, once at start-up, and
 * this composer never writes through that descriptor.
 *
 * Returns 0 on success. On a partial answer the gaps are filled rather than
 * failed on -- a panel that reports no physical size still has a resolution,
 * and refusing to drive it over a missing millimetre count would be absurd.
 */
int readDisplayMode(int index, DisplayMode *outMode);

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_FB_MODE_H
