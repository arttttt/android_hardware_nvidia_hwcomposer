/*
 * Copyright (C) 2022 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's drm/DrmCrtc.h.
 *
 * The part of a display controller that assembles one screen's worth of
 * picture: it is what a set of planes is composed onto and what drives the
 * timing.
 *
 * Above this line only two things are ever asked of it -- what it is called,
 * and whether it can correct colour on the way out. The second is a group of
 * settable attributes, and unlike the connector's, these are not absent for
 * good: this controller has a colour management unit with a matrix and two
 * lookup tables, which is what these attributes describe. They answer absent
 * today because nothing here fills them in yet, and the day something does,
 * the code above that reads them is already written.
 */

#pragma once

#include <cstdint>

#include "display/Connector.h"
#include "display/DrmProperty.h"
#include "display/PipelineBinding.h"

namespace android::drm_hwcomposer {

class Crtc : public PipelineBindable<Crtc> {
 public:
  virtual ~Crtc() = default;

  virtual uint32_t GetId() const = 0;

  /* Which of the controller's heads this is, counting from zero. Upstream
   * needs it to name the bit of a plane's mask that says the plane may be
   * used with this head. */
  virtual uint32_t GetIndexInResArray() const = 0;

  virtual const DrmProperty &GetActiveProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetModeProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetOutFencePtrProperty() const {
    return AbsentProperty();
  }

  /* The colour matrix applied on the way out of the controller. */
  virtual const DrmProperty &GetCtmProperty() const {
    return AbsentProperty();
  }

  /* The lookup table applied before the matrix, and its size. */
  virtual const DrmProperty &GetDegammaLutProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetDegammaLutSizeProperty() const {
    return AbsentProperty();
  }

  /* The lookup table applied after it, and its size. */
  virtual const DrmProperty &GetGammaLutProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetGammaLutSizeProperty() const {
    return AbsentProperty();
  }
};

}  // namespace android::drm_hwcomposer
