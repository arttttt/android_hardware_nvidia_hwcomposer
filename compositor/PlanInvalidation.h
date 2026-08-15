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

/* Why a composition plan stopped being reusable.
 *
 * A plan -- which layer goes to which window, which are the client's --
 * depends on a small, known set of state. Whoever changes a piece of that
 * state raises the bit for it here, and a frame that arrives with no bits
 * raised takes the previous plan without redoing the planning or asking the
 * kernel whether the same frame would still fit.
 *
 * The bits are raised where the state actually changes and compared against
 * the value being replaced, so re-stating the same value raises nothing.
 * What is deliberately NOT here: a new buffer of the same size, format and
 * layout (the plan does not care what the pixels are), damage rectangles,
 * and brightness -- all three describe content or per-frame data that the
 * commit reads from live state anyway.
 */
enum PlanInvalidator : uint32_t {
  /* The layer's buffer changed shape: size, format or memory layout. */
  kBufferGeometry = 1U << 0,
  kBlendMode = 1U << 1,
  kCompositionType = 1U << 2,
  kDisplayFrame = 1U << 3,
  kPlaneAlpha = 1U << 4,
  kSourceCrop = 1U << 5,
  kTransform = 1U << 6,
  kZOrder = 1U << 7,
  /* Colourspace, encoding, sample range or transfer function. */
  kDataspace = 1U << 8,

  kLayerAdded = 1U << 9,
  kLayerRemoved = 1U << 10,

  kColorTransform = 1U << 11,
  kColorMode = 1U << 12,
  kDisplayConfig = 1U << 13,
  kPowerMode = 1U << 14,

  /* Anything at all: a failed commit, a torn-down buffer, a fresh start. */
  kAllDirty = 1U << 15,
};

}  // namespace android::drm_hwcomposer
