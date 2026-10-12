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

#pragma once

#include <cstdint>

namespace android::drm_hwcomposer {

/* A rectangle as the composition works in them: edges, right and bottom
 * exclusive. On its own so that what only does geometry need not bring in
 * the rest of a layer, and can be checked on the workstation. */
template <typename T>
struct Rect {
  T left;
  T top;
  T right;
  T bottom;

  T Width() const {
    return right - left;
  }

  T Height() const {
    return bottom - top;
  }

  bool operator==(const Rect<T>& rhs) const {
    return left == rhs.left && top == rhs.top && right == rhs.right &&
           bottom == rhs.bottom;
  }
  bool operator!=(const Rect<T>& rhs) const {
    return !(*this == rhs);
  }
};

using IRect = Rect<int32_t>;
using FRect = Rect<float>;

}  // namespace android::drm_hwcomposer
