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

#include "LayerData.h"

namespace android::drm_hwcomposer {

class Plane;
struct DisplayPipeline;
template <typename T>
class BindingOwner;

struct LayerToPlaneJoiningPlan {
  struct LayerToPlaneJoining {
    LayerData layer;
    std::shared_ptr<BindingOwner<Plane>> plane;
    int z_pos;

    // TODO: Before we allow C++20, we need this custom constructor to eliminate
    // unnecessary memory copying (LayerData is big) with vector::emplace_back.
    LayerToPlaneJoining(LayerData&& layer,
                        std::shared_ptr<BindingOwner<Plane>> plane,
                        int z_pos)
        : layer(std::move(layer)), plane(std::move(plane)), z_pos(z_pos) {
    }
  };

  std::vector<LayerToPlaneJoining> plan;
  /* The client entry's position in `plan` -- a rank, not a z-order. */
  std::optional<int> client_index;

  /* Whether the merge was steered onto the quiet run of the stack
   * rather than taking the top by first fit, and when it was not --
   * why. For the counters only; nothing downstream behaves differently
   * for a steered plan. The reasons matter more than the ratio: they
   * say whether the fallback is the table's size or the policy's
   * blindness, which is not a question to settle by assertion. */
  enum class Steering {
    kSteered,
    /* The ordinary planes hold the scene whole; merging would cost an
     * engine pass for tidiness. */
    kFitsOrdinary,
    /* All quiet or all drawing: no run is better than any other. */
    kMonotone,
    /* The longest quiet run outnumbers the merging planes. */
    kRunTooLong,
    /* What is not in the run outnumbers the ordinary planes: too many
     * drawing layers for the windows -- the table's actual size. */
    kLivesOverflow,
    /* A layer refused the plane class the steering chose for it. */
    kSeatRefused,
    /* No run of the needed width is uniform in transform: the engine
     * has one turn for the whole configuration, so a mixed run would
     * turn members that were not asked. */
    kMixedTurn,
  };
  Steering steering = Steering::kFitsOrdinary;

  /* The runs the steering weighed, in the order it met them: where each
   * began in the stack, how many drawing layers it held, and how many
   * pixels the merge would push for it -- each member counted at the
   * larger of what it reads and what it writes. For the record only;
   * nothing in the compositor acts on them. */
  struct Run {
    size_t begin = 0;
    size_t live = 0;
    uint64_t pixels = 0;
    /* The steepest resize among the members, in percent of the source;
     * a hundred when nothing is resized. */
    uint32_t scale_pct = 100;
    /* Whether the layers outside this run would all find a window, and
     * the run a merging plane: a run that cannot be seated is not a
     * choice the planner had. */
    bool seatable = false;
  };
  std::vector<Run> runs;
  /* Which of `runs` was taken, when the plan was steered. */
  size_t chosen_run = 0;
  /* How wide the runs were: the merge's width for this frame. */
  size_t run_len = 0;


  static auto CreateLayerToPlaneJoiningPlan(
      const DisplayPipeline &pipe, std::vector<LayerData> composition,
      std::optional<LayerData> cursor_layer = std::nullopt,
      bool prefer_merge = false)
      -> std::unique_ptr<LayerToPlaneJoiningPlan>;
};

}  // namespace android::drm_hwcomposer
