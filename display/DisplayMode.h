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

#ifndef DISPLAY_DISPLAY_MODE_H
#define DISPLAY_DISPLAY_MODE_H

#include <cstdint>

namespace android {
namespace hwc {

/* One display timing, in the terms the framework asks for.
 *
 * Deliberately not the hardware's own mode structure: what a display
 * controller calls a mode carries porches, sync widths and pixel clocks that
 * no consumer above this line has any use for. The backend converts once, on
 * the way out.
 */
struct DisplayMode {
    int32_t width = 0;
    int32_t height = 0;

    /* Nanoseconds between two vertical blanks. */
    int32_t vsyncPeriodNs = 0;

    /* Thousandths of a pixel per inch, which is how the framework wants it.
     * Zero where the panel size is unknown; consumers must treat it as
     * "no information" rather than as a real density. */
    int32_t dpiX = 0;
    int32_t dpiY = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_DISPLAY_MODE_H
