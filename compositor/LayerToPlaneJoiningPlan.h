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
  std::optional<int> client_z_order;

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

  bool steered = false;

  static auto CreateLayerToPlaneJoiningPlan(
      const DisplayPipeline &pipe, std::vector<LayerData> composition,
      std::optional<LayerData> cursor_layer = std::nullopt)
      -> std::unique_ptr<LayerToPlaneJoiningPlan>;
};

}  // namespace android::drm_hwcomposer
