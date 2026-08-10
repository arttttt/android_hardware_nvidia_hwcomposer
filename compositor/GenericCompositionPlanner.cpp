/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "GenericCompositionPlanner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/FlatteningController.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "display/CommitStatus.h"
#include "display/Plane.h"
#include "hwc/HwcLayer.h"
#include "utils/Logging.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {

const HwcLayer* GetCursorLayer(const std::vector<const HwcLayer*>& layers) {
  auto it = std::find_if(layers.begin(), layers.end(),
                         [&](auto* layer) -> bool {
                           return layer->GetSfType() ==
                                  CompositionType::kCursor;
                         });
  if (it == layers.end()) {
    return nullptr;
  }
  return *it;
}

}  // namespace

auto GenericCompositionPlanner::ValidateDisplay(
    const ICompositorDisplay* display) -> ValidationResult {
  const auto layers = display->GetOrderLayersByZPos();

  const FlatteningController* flatcon = display->GetFlatCon();
  if (flatcon != nullptr && flatcon->ShouldFlatten()) {
    return {.composition = GetFlattenedComposition(layers,
                                                   FlattenReason::kStaticScene),
            .short_circuited = false};
  }

  if (display->CtmByGpu()) {
    return {.composition = GetFlattenedComposition(layers, FlattenReason::
                                                               kCtmWithOffset),
            .short_circuited = false};
  }

  if (CompositionPlanner::LayersUseDifferentColorspaces(layers) &&
      !display->UseColorPipeline()) {
    return {.composition = GetFlattenedComposition(
                layers, FlattenReason::kNoPerPlaneColorspaceSupport),
            .short_circuited = false};
  }

  bool use_cursor_plane = false;
  const auto* cursor_layer = GetCursorLayer(layers);
  const auto cursor_plane = display->GetCursorPlane();
  if (cursor_layer != nullptr && cursor_plane != nullptr &&
      !IsClientLayer(display, cursor_layer) &&
      cursor_plane->Get()->IsValidForLayer(&cursor_layer->GetLayerData()) &&
      // TODO: Add a check for cursor plane color transform support.
      !display->CursorPlaneNeedsColorPipeline(*cursor_layer)) {
    // Create and test a composition using only cursor plane and all other
    // layers client-composited to infer whether the cursor plane can be used.
    ValidatedComposition cursor_composition{
        .composition_types = GetCompositionTypes(layers, 0, layers.size() - 1,
                                                 /*use_cursor_plane=*/true)};
    use_cursor_plane = display->TestComposition(cursor_composition).success;
  }

  size_t client_start = 0;
  size_t client_size = 0;
  ValidatedComposition validated_composition{};
  CommitStatus commit_status;

  // Populates and tests |validated_composition|, returning whether it
  // succeeded.
  auto validate_and_test = [&]() -> bool {
    validated_composition
        .composition_types = GetCompositionTypes(layers, client_start,
                                                 client_size, use_cursor_plane);

    bool testing_needed = client_start != 0 || client_size != layers.size();
    if (testing_needed) {
      commit_status = display->TestComposition(validated_composition);
      return commit_status.success;
    }

    // Reset the plan in case it was set during a previous test.
    validated_composition.composition_plan.reset();

    return true;
  };

  // Initial composition attempt.
  std::tie(client_start, client_size) = GetClientLayers(display, layers,
                                                        use_cursor_plane);
  bool success = validate_and_test();

  // Cursor fallback: convert all non-cursor layers to client composition and
  // reattempt. (Cursor layer is preserved as _either_ cursor _or_ device
  // composited.)
  if (!success && cursor_layer != nullptr) {
    if (layers.back()->GetSfType() != CompositionType::kCursor) {
      ALOGE("Cursor layer was not found at highest z-order");
      // Continue to next fallback.
    } else {
      client_start = 0;
      client_size = layers.size() - 1;
      success = validate_and_test();
    }
  }

  // Final fallback: convert all layers to client composition.
  if (!success) {
    validated_composition = GetFlattenedComposition(layers,
                                                    FlattenReason::
                                                        kValidateFailed);
    validated_composition.error_code = commit_status.error_code;
  }

  if (use_cursor_plane) {
    validated_composition.cursor_plane_validated = success;
  }

  /* The other half of the answer. Everything that ends up wholly on the GPU
   * says why on its way through GetFlattenedComposition; this says what got
   * through when it did not, which is the number the whole exercise is about.
   *
   * Said only when it changes, for the same reason as there: a plan holds for
   * long stretches, and a line a frame would be sixty a second of the same
   * sentence.
   */
  if (success) {
    size_t on_hardware = 0;
    for (const auto& [layer, type] : validated_composition.composition_types) {
      if (type != CompositionType::kClient) {
        ++on_hardware;
      }
    }

    static size_t last_on_hardware = SIZE_MAX;
    static size_t last_total = SIZE_MAX;
    if (on_hardware != last_on_hardware || layers.size() != last_total) {
      last_on_hardware = on_hardware;
      last_total = layers.size();
      HWC_LOGX("plan: %zu of %zu layer(s) on the hardware", on_hardware,
               layers.size());

      /* And which, with what they are, because the count alone does not say
       * whether the GPU was left with a full screen to draw or a scrap. A
       * layer left to the client is one the display could not be given, and
       * the reason is nearly always in these numbers -- how big it is, where
       * it lands, whether it is being resized. */
      for (size_t z = 0; z < layers.size(); ++z) {
        const auto it = validated_composition.composition_types.find(layers[z]);
        const bool client = it == validated_composition.composition_types.end()
                                ? true
                                : it->second == CompositionType::kClient;

        const auto& pi = layers[z]->GetLayerData().pi;
        const auto& src = pi.source_crop.f_rect;
        const auto& dst = pi.display_frame.i_rect;

        HWC_LOGX("  z=%zu -> %-8s src=%.0fx%.0f dst=%dx%d+%d+%d%s", z,
                 client ? "client" : "hardware",
                 src ? src->Width() : 0.F, src ? src->Height() : 0.F,
                 dst ? dst->Width() : 0, dst ? dst->Height() : 0,
                 dst ? dst->left : 0, dst ? dst->top : 0,
                 pi.RequireScalingOrPhasing() ? " resized" : "");
      }
    }
  }

  return {.composition = std::move(validated_composition),
          .short_circuited = false};
}

std::tuple<size_t, size_t> GenericCompositionPlanner::GetClientLayers(
    const ICompositorDisplay* display,
    const std::vector<const HwcLayer*>& layers, bool use_cursor_plane) {
  size_t client_start = 0;
  size_t client_size = 0;

  for (size_t z_order = 0; z_order < layers.size(); ++z_order) {
    if (IsClientLayer(display, layers[z_order])) {
      if (client_size == 0) {
        client_start = z_order;
      }
      client_size = (z_order - client_start) + 1;
    }
  }

  return GetExtraClientRange(display, layers, client_start, client_size,
                             use_cursor_plane);
}

bool GenericCompositionPlanner::IsClientLayer(const ICompositorDisplay* display,
                                              const HwcLayer* layer) {
  return !HardwareSupportsLayerType(layer->GetSfType()) ||
         !layer->IsLayerUsableAsDevice() || display->CtmByGpu() ||
         (layer->GetLayerData().pi.RequireScalingOrPhasing() &&
          display->ForcedScalingWithGpu());
}

bool GenericCompositionPlanner::HardwareSupportsLayerType(
    CompositionType comp_type) {
  return comp_type == CompositionType::kDevice ||
         comp_type == CompositionType::kCursor;
}

uint32_t GenericCompositionPlanner::CalcPixOps(
    const std::vector<const HwcLayer*>& layers, size_t first_z, size_t size) {
  uint32_t pixops = 0;
  ALOGE_IF(first_z + size > layers.size(),
           "CalcPixOps provided range outside of layers");
  for (size_t z_order = first_z;
       z_order < std::min(first_z + size, layers.size()); ++z_order) {
    pixops += layers[z_order]->GetPixOps();
  }
  return pixops;
}

auto GenericCompositionPlanner::GetCompositionTypes(
    const std::vector<const HwcLayer*>& layers, size_t client_first_z,
    size_t client_size, bool use_cursor_plane) -> CompositionTypeMap {
  CompositionTypeMap composition_types;
  for (size_t z_order = 0; z_order < layers.size(); ++z_order) {
    if (z_order >= client_first_z && z_order < client_first_z + client_size) {
      composition_types[layers[z_order]] = CompositionType::kClient;
    } else if (use_cursor_plane &&
               layers[z_order]->GetSfType() == CompositionType::kCursor) {
      composition_types[layers[z_order]] = CompositionType::kCursor;
    } else {
      composition_types[layers[z_order]] = CompositionType::kDevice;
    }
  }
  return composition_types;
}

std::tuple<size_t, size_t> GenericCompositionPlanner::GetExtraClientRange(
    const ICompositorDisplay* display,
    const std::vector<const HwcLayer*>& layers, size_t client_start,
    size_t client_size, bool use_cursor_plane) {
  size_t avail_planes = display->GetNumAvailablePlanes();
  size_t layers_size = layers.size();

  // Cursor plane is not counted among |avail_planes|, so the cursor layer
  // shouldn't be counted in |layers_size|.
  if (use_cursor_plane) {
    ALOGE_IF(layers.empty() ||
                 layers.back()->GetSfType() != CompositionType::kCursor,
             "Cursor layer was not found at highest z-order");
    --layers_size;
  }

  // If there are more layers than planes, save one plane for client composited
  // layers.
  if (avail_planes < layers_size) {
    avail_planes--;
  }

  // If the cursor plane isn't being used, and the cursor layer isn't already
  // in the client range, reserve a plane for it to be device composited.
  if (!use_cursor_plane && avail_planes > 0 && layers_size > 0 &&
      client_start + client_size < layers_size &&
      layers.back()->GetSfType() == CompositionType::kCursor) {
    avail_planes--;
    layers_size--;
  }

  ALOGE_IF(client_start + client_size > layers.size(),
           "GetExtraClientRange provided client range outside of layers");
  // If extra layers need to be added to the client range, prepare to perform a
  // sliding window search.
  if (layers_size - client_size > avail_planes) {
    const size_t extra_client = (layers_size - client_size) - avail_planes;
    size_t start = 0;
    size_t steps = 0;
    if (client_size != 0) {
      // There are already client layers present, so the window needs to
      // encompass them. Determine the maximum offsets of the ensuing search.
      const size_t prepend = std::min(client_start, extra_client);
      const size_t append = std::min(layers_size - (client_start + client_size),
                                     extra_client);
      start = client_start - prepend;
      client_size += extra_client;
      steps = 1 + std::min(std::min(append, prepend),
                           layers_size - (start + client_size));
    } else {
      // There are no other client layers present, so the window may search the
      // entire range.
      client_size = extra_client;
      steps = 1 + layers_size - extra_client;
    }

    // Use a sliding window to determine the client range that results in the
    // fewest GPU pixops.
    uint32_t gpu_pixops = UINT32_MAX;
    for (size_t i = 0; i < steps; i++) {
      const uint32_t po = CalcPixOps(layers, start + i, client_size);
      if (po < gpu_pixops) {
        gpu_pixops = po;
        client_start = start + i;
      }
    }
  }

  return std::make_tuple(client_start, client_size);
}

}  // namespace android::drm_hwcomposer
