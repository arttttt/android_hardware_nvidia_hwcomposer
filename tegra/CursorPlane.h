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

#ifndef TEGRA_CURSOR_PLANE_H
#define TEGRA_CURSOR_PLANE_H

#include <cstdint>

#include "display/Plane.h"

namespace android::drm_hwcomposer {

/* The cursor unit, wearing the one costume the planner recognises.
 *
 * The planner's cursor machinery -- finding the layer, weighing it,
 * carrying its binding at the top of the plan -- was built against the
 * contract "a cursor plane is a Plane". The unit is not a window and must
 * never stand in the ordinary plane list, where the planner would hand it
 * whatever layer came next; offered only in the cursor seat, it is asked
 * exactly the question it can answer: will you take this one small,
 * unscaled, untransformed thing that sits on top of everything.
 *
 * The answers here are the unit's real appetite. Everything else -- the
 * sprite's pixels, the ioctls, hiding and re-arming -- belongs to
 * CursorUnit; this class only judges.
 */
class TegraCursorPlane : public Plane {
 public:
  TegraCursorPlane() = default;

  /* An identity no window will ever carry: window identifiers are the
   * controller's small indices, and the request builder tells the cursor's
   * binding apart from theirs by this. */
  static constexpr uint32_t kPlaneId = 0xFFFFFFFFU;

  bool IsValidForLayer(const LayerData *layer) override;

  uint32_t GetId() const override {
    return kPlaneId;
  }
};

}  // namespace android::drm_hwcomposer

#endif  // TEGRA_CURSOR_PLANE_H
