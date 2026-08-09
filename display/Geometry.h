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

#ifndef DISPLAY_GEOMETRY_H
#define DISPLAY_GEOMETRY_H

#include <cstdint>

namespace android {
namespace hwc {

/* Rectangles as everything below the HWC2 boundary sees them.
 *
 * The framework's own hwc_rect_t and hwc_frect_t would do the job, and using
 * them would save a conversion. They are not used on purpose: they would make
 * the display backend include the HWC2 headers, and the backend has no
 * business knowing which API is driving it. The conversion happens once, in
 * the layer that already speaks HWC2.
 */

struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t width() const { return right - left; }
    int32_t height() const { return bottom - top; }
    bool isEmpty() const { return width() <= 0 || height() <= 0; }
};

/* Source crops arrive fractional: the framework may sample a scaled region
 * that does not land on pixel boundaries. */
struct FRect {
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    bool isEmpty() const { return width() <= 0.f || height() <= 0.f; }
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_GEOMETRY_H
