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

#include <atomic>
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

  /* Say that what lands here is drawn by the image compositor before this
   * window ever sees it.
   *
   * That changes what the plane can accept, and changes it completely: the
   * window keeps its own narrow limits -- rows only, no resizing, simple
   * colour -- but they are now limits on a buffer we produce, which is
   * exactly that shape by construction. What arrives from a layer is judged
   * by what the engine can read instead, and the engine reads blocks, resizes
   * and blends.
   *
   * So the one window on this controller that could take nothing becomes the
   * one that can take anything. */
  void SetMerging() {
    merging_ = true;
  }

  bool IsMerging() const override {
    return merging_;
  }

  /* How many times a merging plane turned a layer away for carrying a
   * transform. Counts answers, not distinct layers: the planner asks per
   * plan weighed, so a persistent transformed layer counts every frame --
   * read it as none, some, or bursts, never as a population. This is the
   * number that decides whether the merge ever learns to turn layers
   * itself, the way the stock composer's scratch path did with the 2D
   * engine. */
  static uint64_t TransformRefusals() {
    return transform_refusals_.load(std::memory_order_relaxed);
  }

  /* How many times a merging plane turned a layer away for resizing past
   * what the engine takes. Same counting rule as above: answers per plan
   * weighed, not distinct layers. */
  static uint64_t ScaleRefusals() {
    return scale_refusals_.load(std::memory_order_relaxed);
  }

  /* The engine's reach on resizing one source, per axis, either way.
   *
   * Measured, not guessed: the verifier inside the engine's configure
   * step was asked directly (tools/vicscaletest) and answered sixteen --
   * exactly, on every axis, in both directions, with both axes at the
   * boundary at once, deterministically, and independently of the other
   * axis's extent. Sixteen to one passes; anything past it is refused,
   * and a refusal past validation is a frame the ladder cannot save,
   * which is why the judging happens here. */
  static constexpr float kEngineScaleReach = 16.0F;

  /* Whether resizing `src` to `dst` is past the engine's reach. The source
   * axes are given already turned into the display's frame -- the caller
   * that turns a layer swaps them first. One judge for the plane's own
   * answer and for the test switch in the state manager, so the two can
   * never drift apart when the boundary is recalibrated. */
  static bool BeyondEngineReach(float src_w, float src_h, float dst_w,
                                float dst_h);

  /* The longest side a turned copy may have -- the intermediates it lands
   * in are cut no larger. Told once by whoever sizes those intermediates;
   * zero means no turning machinery exists and no such bound applies. */
  static void SetTurnReach(uint32_t reach) { turn_reach_ = reach; }
  static uint32_t TurnReach() { return turn_reach_; }

 private:
  const uint32_t index_;
  const hwc::DcHead::WindowCapabilities &caps_;
  bool merging_ = false;

  static std::atomic<uint64_t> transform_refusals_;
  static std::atomic<uint64_t> scale_refusals_;
  static uint32_t turn_reach_;
};

}  // namespace android::drm_hwcomposer
