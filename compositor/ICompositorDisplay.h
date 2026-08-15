/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/DisplayInfo.h"
#include "display/PipelineBinding.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

class Plane;
class FlatteningController;

struct CommitStatus;

// ICompositorDisplay exists purely to isolate methods in HwcDisplay used inside
// the compositor/ directory. HwcDisplay has many Android-specific dependencies
// that prevent host-side unit tests from building, so this interface
// facilitates mocking of portion of HwcDisplay used in this directory.
class ICompositorDisplay {
 public:
  virtual ~ICompositorDisplay() = default;

  virtual std::vector<const HwcLayer *> GetOrderLayersByZPos() const = 0;

  virtual const FlatteningController *GetFlatCon() const = 0;

  virtual size_t GetNumAvailablePlanes() const = 0;
  virtual std::shared_ptr<BindingOwner<Plane>> GetCursorPlane() const = 0;

  virtual CommitStatus TestComposition(
      CompositionPlanner::ValidatedComposition &composition) const = 0;

  /* The last composition the display actually showed, ready to be handed
   * back for a frame that changed nothing, or empty when there is no such
   * thing -- a fresh start, a failed commit. Its plan reference is dropped;
   * the commit rebuilds the joining from the types it carries. */
  virtual const std::optional<CompositionPlanner::ValidatedComposition> &
  GetReusablePlan() const = 0;

  /* Everything that changed underneath the last plan, as PlanInvalidator
   * bits gathered from the display and every layer, cleared by the asking.
   * Zero means the previous plan still describes this frame. A display that
   * does not track this answers all-dirty, which is never wrong, only never
   * reused. */
  virtual uint32_t TakePlanInvalidators() const {
    return 0xFFFFFFFF;
  }

  virtual bool CtmByGpu() const = 0;
  virtual bool ForcedScalingWithGpu() const = 0;
  virtual bool UseColorPipeline() const = 0;

  // Returns the currently configured display resolution as {width, height}.
  virtual std::pair<uint32_t, uint32_t> GetSize() const = 0;

  virtual const HwcLayer &GetClientLayer() const = 0;

  virtual std::shared_ptr<const HalColorTransformMatrix>
  GetColorTransformMatrix() const = 0;

  virtual bool CursorPlaneNeedsColorPipeline(
      const HwcLayer &cursor_layer) const = 0;
};

}  // namespace android::drm_hwcomposer
