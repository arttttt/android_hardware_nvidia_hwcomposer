/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "CompositionPlanner.h"

#include <vector>

#include "compositor/LayerData.h"
#include "hwc/HwcLayer.h"
#include "utils/Logging.h"

namespace android::drm_hwcomposer {

namespace {
auto GetFlattenedCompositionTypes(const std::vector<const HwcLayer*>& layers)
    -> CompositionPlanner::CompositionTypeMap {
  CompositionPlanner::CompositionTypeMap composition_types;
  for (const auto* layer : layers) {
    composition_types[layer] = CompositionType::kClient;
  }
  return composition_types;
}

const char* NameOf(CompositionPlanner::FlattenReason reason) {
  switch (reason) {
    case CompositionPlanner::FlattenReason::kNone:
      return "no reason given";
    case CompositionPlanner::FlattenReason::kStaticScene:
      return "the scene stopped changing";
    case CompositionPlanner::FlattenReason::kValidateFailed:
      return "the hardware would not take the plan";
    case CompositionPlanner::FlattenReason::kCtmWithOffset:
      return "a colour transform only the GPU can apply";
    case CompositionPlanner::FlattenReason::kNoPerPlaneColorspaceSupport:
      return "layers in different colourspaces and no colour pipeline";
  }
  return "unknown";
}
}  // namespace

CompositionPlanner::ValidatedComposition
CompositionPlanner::GetFlattenedComposition(
    const std::vector<const HwcLayer*>& layers, FlattenReason flatten_reason) {
  /* Every way a plan can end up entirely on the GPU passes through here, and
   * each of them arrives carrying why -- so this is the one place that can
   * say it, and the reason is already computed rather than guessed at.
   *
   * Said only when it changes. A plan holds for long stretches, so a line per
   * frame would be sixty a second saying the same thing, which is both
   * useless and a cost of its own on a device this old. A line per change is
   * a handful over a whole session and misses nothing.
   *
   * Kept here rather than in the reporting that already collects this: that
   * goes to a statistics service, which answers how often, not which frames.
   */
  static FlattenReason last_reason = FlattenReason::kNone;
  static size_t last_count = 0;
  if (flatten_reason != last_reason || layers.size() != last_count) {
    last_reason = flatten_reason;
    last_count = layers.size();
    HWC_LOGD("plan: all %zu layer(s) to the client -- %s", layers.size(),
             NameOf(flatten_reason));
  }

  return ValidatedComposition{.composition_types = GetFlattenedCompositionTypes(
                                  layers),
                              .composition_plan = nullptr,
                              .flatten_reason = flatten_reason};
}
}  // namespace android::drm_hwcomposer
