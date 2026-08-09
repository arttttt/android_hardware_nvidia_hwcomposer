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

#include "display/PipelineBinding.h"

namespace android::drm_hwcomposer {

struct LayerData;

/* One piece of display hardware that can show one layer by itself.
 *
 * What the planner needs to know about such a thing is a single question --
 * can this one show that layer -- and the answer depends on what the hardware
 * can do rather than on how it is driven. So that question is all this says,
 * and everything about how a plane is programmed lives with whoever drives
 * it.
 *
 * The planner reaches a plane through a binding that says the plane is
 * currently this display's, which is why this is bindable; two displays must
 * not both put something on the same one.
 *
 * Extracted from drm-hwcomposer's DrmPlane, which is the same idea with the
 * driving attached: eight hundred lines of reading properties from a DRM
 * device and assembling atomic requests, none of which mean anything on a
 * controller that has no DRM driver. What the planner asks of it is these two
 * calls.
 */
class Plane : public PipelineBindable<Plane> {
 public:
  virtual ~Plane() = default;

  /* Can this plane show this layer as it is? False whenever anything about
   * the layer -- its format, its arrangement in memory, a rotation, a
   * transparency, a way of blending -- is beyond what this plane does, in
   * which case the framework will have to draw it instead. */
  virtual bool IsValidForLayer(const LayerData *layer) = 0;

  /* Whether this plane can correct colour on its own -- its own matrix, its
   * own lookup tables -- rather than taking whatever the display applies to
   * everything. Windows here cannot, so the default says so. */
  virtual bool HasColorPipeline() const {
    return false;
  }

  /* Which plane this is, for the log. Numbering is the hardware's own. */
  virtual uint32_t GetId() const = 0;
};

}  // namespace android::drm_hwcomposer
