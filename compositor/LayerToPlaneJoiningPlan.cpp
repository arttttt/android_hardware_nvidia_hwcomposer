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

#include "LayerToPlaneJoiningPlan.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/LayerData.h"
#include "display/DisplayPipeline.h"
#include "display/Plane.h"

namespace android::drm_hwcomposer {

namespace {

using PlaneRef = std::shared_ptr<BindingOwner<Plane>>;

/* The first fit: each layer takes the first remaining plane that can
 * show it, and a plane once passed is never offered again. With the
 * merging planes listed last, whatever fails to find an ordinary plane
 * falls into the merge and everything above it follows -- the merge
 * takes the top of the stack, whether or not the top is where the
 * drawing is. */
bool PlaceFirstFit(LayerToPlaneJoiningPlan &plan,
                   const std::vector<PlaneRef> &avail_planes,
                   std::vector<LayerData> &composition) {
  auto first_avail_plane = avail_planes.begin();
  for (auto &dhl : composition) {
    // Consume avail_planes until a valid plane is found for the layer.

    const auto suitable_plane = std::find_if(first_avail_plane,
                                             avail_planes.end(),
                                             [&dhl](const auto &plane) {
                                               return plane->Get()
                                                   ->IsValidForLayer(&dhl);
                                             });
    if (suitable_plane == avail_planes.end()) {
      return false;
    }
    plan.plan.emplace_back(std::move(dhl), *suitable_plane,
                           static_cast<int>(plan.plan.size()));

    first_avail_plane = suitable_plane + 1;
  }
  return true;
}

/* The steering the merge has waited for. The window that holds a merged
 * buffer can only take a contiguous run of the stack, and the first fit
 * always hands it the top -- where popups and animations live, so the
 * engine redraws the group on every frame of exactly the scenes that
 * are busiest. Steering gives the merge a contiguous run of the needed
 * width carrying the fewest drawing layers instead, wherever in the
 * stack it lies, and seats the drawing layers in windows of their own:
 * the group's content then holds still, the merge cache answers every
 * frame, and the engine sleeps through the animation.
 *
 * Only worth doing when a merge is unavoidable and the scene is mixed;
 * anything else falls through to the first fit, as does any layout this
 * one cannot seat -- a drawing layer the windows cannot take, or a
 * scene that wants to put more layers into the merge than it takes in
 * one pass. Nothing is moved until the whole
 * seating is known to work, so falling through costs a walk, not a
 * frame. */
bool PlaceSteered(LayerToPlaneJoiningPlan &plan,
                  const std::vector<PlaneRef> &avail_planes,
                  std::vector<LayerData> &composition) {
  using Steering = LayerToPlaneJoiningPlan::Steering;

  size_t ordinary_count = 0;
  size_t merging_count = 0;
  for (const auto &plane : avail_planes) {
    if (plane->Get()->IsMerging()) {
      merging_count++;
    } else {
      ordinary_count++;
    }
  }

  /* A scene the ordinary planes can hold whole should not pay an engine
   * pass for tidiness. */
  if (merging_count == 0 || composition.size() <= ordinary_count) {
    plan.steering = Steering::kFitsOrdinary;
    return false;
  }

  bool any_live = false;
  bool any_quiet = false;
  for (const auto &dhl : composition) {
    (dhl.live ? any_live : any_quiet) = true;
  }
  /* All quiet or all drawing: no run is better than any other, and the
   * first fit's shape is as good as shapes get. */
  if (!any_live || !any_quiet) {
    plan.steering = Steering::kMonotone;
    return false;
  }

  /* A contiguous run of exactly the needed width, carrying the fewest
   * drawing layers; ties go to the lower run, where wallpaper and
   * application -- the natural group -- sit.
   *
   * The width is fixed: adding a layer to the run can never lower the
   * number of live layers it holds, so the minimum is always reached at
   * the minimum width, and a wider run cannot win. And a run one member
   * wider would have stolen that member a window for no gain -- a direct
   * window is cheaper than the merge. Hence one pass instead of a
   * search.
   *
   * This leaves exactly as many layers outside the run as there are
   * ordinary windows. If one of those fails to seat, the whole seating
   * fails and everything falls to the first fit, exactly as today. A
   * wider run would have pulled the troublesome layer into the merge and
   * saved the layout. Widening is trivial -- repeat the search one layer
   * wider, up to the merge's limit -- and is deliberately not done: the
   * seating-refusal counter in the field is steady at zero.
   *
   * The only honest refusal left is wanting to put more layers into the
   * merge than it takes in one pass. */
  const size_t need = composition.size() - ordinary_count;
  if (need > merging_count) {
    plan.steering = Steering::kRunTooLong;
    return false;
  }

  size_t live_here = 0;
  for (size_t i = 0; i < need; i++)
    if (composition[i].live)
      live_here++;

  size_t run_begin = 0;
  const size_t run_len = need;
  size_t fewest_live = live_here;
  for (size_t i = need; i < composition.size(); i++) {
    if (composition[i - need].live)
      live_here--;
    if (composition[i].live)
      live_here++;
    if (live_here < fewest_live) {
      fewest_live = live_here;
      run_begin = i - need + 1;
    }
  }

  /* Seat everything tentatively -- the plan's z comes from position in
   * the plan, not from which window shows a layer, so the quiet run may
   * sit anywhere in the stack while the windows around it take the
   * drawing layers. Each class keeps the first fit's own rule inside
   * itself: a plane once passed is never offered again. */
  std::vector<PlaneRef> seats;
  seats.reserve(composition.size());
  auto ordinary_it = avail_planes.begin();
  auto merging_it = avail_planes.begin();
  for (size_t i = 0; i < composition.size(); i++) {
    const bool merged = i >= run_begin && i < run_begin + run_len;
    const auto *dhl = &composition[i];
    const auto fits = [merged, dhl](const PlaneRef &plane) {
      return plane->Get()->IsMerging() == merged &&
             plane->Get()->IsValidForLayer(dhl);
    };
    auto &it = merged ? merging_it : ordinary_it;
    it = std::find_if(it, avail_planes.end(), fits);
    if (it == avail_planes.end()) {
      plan.steering = Steering::kSeatRefused;
      return false;
    }
    seats.push_back(*it);
    ++it;
  }

  for (size_t i = 0; i < composition.size(); i++) {
    plan.plan.emplace_back(std::move(composition[i]), std::move(seats[i]),
                           static_cast<int>(plan.plan.size()));
  }
  plan.steering = Steering::kSteered;
  return true;
}

}  // namespace

auto LayerToPlaneJoiningPlan::CreateLayerToPlaneJoiningPlan(
    const DisplayPipeline &pipe, std::vector<LayerData> composition,
    std::optional<LayerData> cursor_layer)
    -> std::unique_ptr<LayerToPlaneJoiningPlan> {
  auto [avail_planes, cursor_plane] = pipe.GetUsablePlanes();

  if (cursor_layer) {
    if (!cursor_plane ||
        !cursor_plane->Get()->IsValidForLayer(&cursor_layer.value())) {
      // Cursor plane can't be used. The cursor layer may need to fallback to
      // device or client composition.
      return {};
    }
  }

  auto plan = std::make_unique<LayerToPlaneJoiningPlan>();
  plan->plan.reserve(composition.size() +
                     static_cast<size_t>(cursor_layer.has_value()));

  plan->steered = PlaceSteered(*plan, avail_planes, composition);
  if (!plan->steered && !PlaceFirstFit(*plan, avail_planes, composition)) {
    return {};
  }

  // Add cursor plane last to ensure it gets highest z-pos.
  if (cursor_layer) {
    // cursor_plane was already checked at the beginning of function.
    plan->plan.emplace_back(std::move(cursor_layer.value()), cursor_plane,
                            static_cast<int>(plan->plan.size()));
  }

  return plan;
}

}  // namespace android::drm_hwcomposer
