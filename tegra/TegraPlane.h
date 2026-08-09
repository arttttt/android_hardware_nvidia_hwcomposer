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

#include "display/Plane.h"
#include "tegra/DcHead.h"

namespace android::drm_hwcomposer {

/* One window of a display head, answering for itself.
 *
 * The windows of a head are not alike, and this is where that stops being a
 * fact about the hardware and starts being an answer the planner can use.
 * What it knows about itself came from the controller at start-up; nothing
 * here is assumed.
 */
class TegraPlane : public Plane {
 public:
  /* `capabilities` outlives this object: both belong to the head. */
  TegraPlane(uint32_t index, const hwc::DcHead::WindowCapabilities &caps)
      : index_(index), caps_(caps) {
  }

  bool IsValidForLayer(const LayerData *layer) override;

  uint32_t GetId() const override {
    return index_;
  }

  /* Is this the one to offer as a cursor plane?
   *
   * A window that reads neither memory arranged in blocks nor anything
   * resized will decline every layer the GPU drew, which is all of them. It
   * is not broken and it is not spare -- it is the narrow window this
   * controller has, and a cursor is exactly the small unscaled thing it can
   * show. Offering it as that puts it to the only use it has. */
  bool IsCursorCandidate() const {
    return !caps_.blocklinearLayout && !caps_.scaling;
  }

 private:
  const uint32_t index_;
  const hwc::DcHead::WindowCapabilities &caps_;
};

}  // namespace android::drm_hwcomposer
