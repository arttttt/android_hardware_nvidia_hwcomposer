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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
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
#include "utils/properties.h"

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
  /* Counted whole, from any exit, which is what the destructor buys. */
  struct Timed {
    GenericCompositionPlanner* planner;
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::
        now();
    ~Timed() {
      planner->RecordValidation(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - began)
              .count()));
    }
  } timed{this};

  const auto layers = display->GetOrderLayersByZPos();

  /* At the adaptive door a still multi-layer scene is not flattened on
   * the GPU: the plan below consolidates it into the merge instead --
   * the seating sees the same stillness and widens its run -- so the
   * scene ends on one scanned buffer either way, with the cheaper
   * executor. Every other reason to flatten stands untouched. */
  const FlatteningController* flatcon = display->GetFlatCon();
  if (flatcon != nullptr && flatcon->ShouldFlatten() &&
      !(Properties::MergePreference() == 2 && layers.size() > 1)) {
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

  /* A frame that changed nothing takes the previous plan. The flags decide:
   * every mutation of plan-shaping state raised a bit where it happened, and
   * a frame arriving with none raised is the frame before it as far as the
   * plan cares. Placed after the early exits above so a scene that ought to
   * flatten flattens instead of endlessly reusing a live plan. */
  {
    const uint32_t invalidators = display->TakePlanInvalidators();
    const auto& reusable = display->GetReusablePlan();

    /* A flattened composition is never the plan to hand back: it was chosen
     * because the scene had stopped, and the frame asking here is the scene
     * moving again -- with nothing raised, since new pixels raise nothing. */
    const bool have_plan = reusable &&
                           reusable->flatten_reason == FlattenReason::kNone;

    if (invalidators == 0 && have_plan) {
      const auto copy_began = std::chrono::steady_clock::now();
      ValidationResult result{.composition = *reusable,
                              .short_circuited = true};
      const auto copy_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - copy_began)
              .count());
      for (ValidationStats* stats : {&lifetime_, &interval_}) {
        stats->reused.fetch_add(1, std::memory_order_relaxed);
        stats->reuse_copy_us.fetch_add(copy_us, std::memory_order_relaxed);
      }
      return result;
    }
    if (invalidators != 0) {
      for (ValidationStats* stats : {&lifetime_, &interval_})
        stats->invalidators_seen.fetch_or(invalidators,
                                          std::memory_order_relaxed);
    }
  }

  bool use_cursor_plane = false;
  const auto* cursor_layer = GetCursorLayer(layers);
  const auto cursor_plane = display->GetCursorPlane();
  if (cursor_layer != nullptr && cursor_plane != nullptr &&
      // A hardware cursor draws over every plane unconditionally, so the
      // cursor layer must be the topmost layer of the scene -- anything
      // meant to be above it would be drawn under it otherwise.
      !layers.empty() && layers.back() == cursor_layer &&
      !IsClientLayer(display, cursor_layer) &&
      cursor_plane->Get()->IsValidForLayer(&cursor_layer->GetLayerData()) &&
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
    validated_composition.punch_out_layers = GetPunchOutLayers(layers,
                                                               client_start,
                                                               client_size);

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
                                                        use_cursor_plane,
                                                        /*forced_extra_client=*/
                                                        0);
  bool success = validate_and_test();

  /* One rung of a ladder before the floor. A refusal here is the controller
   * declining what it was asked to feed; the answer the stock composer gave
   * is to ask again with decisively less rather than surrender everything to
   * the GPU.
   *
   * Decisively is the word doing the work. The display's load is a stair,
   * not a slope: swapping one layer for the client target keeps the same
   * surfaces scanning and the same answer coming, so stepping by one layer
   * walks a plateau of refusals at a full kernel round-trip a step. The one
   * step worth taking is to the next drop -- the plan that fits the real
   * scanout surfaces outright, needing no merge -- and the sliding-window
   * search picks the cheapest layers to hand over on the way there. One
   * recompute, one more question to the kernel; a second refusal falls
   * through to the floor below, and the refusal's errno is kept where the
   * dump can show whether the ladder is fighting bandwidth or something
   * else entirely. */
  if (!success && layers.size() > 1) {
    const size_t refused_start = client_start;
    const size_t refused_size = client_size;

    for (ValidationStats* stats : {&lifetime_, &interval_})
      stats->last_refusal_error.store(commit_status.error_code,
                                      std::memory_order_relaxed);

    const size_t direct_planes = display->GetNumDirectPlanes();
    const size_t device_layers = layers.size() - refused_size;
    size_t rung = 1;
    if (direct_planes > 0 && device_layers + 1 > direct_planes) {
      rung = device_layers + 1 - direct_planes;
    }

    std::tie(client_start, client_size) = GetClientLayers(
        display, layers, use_cursor_plane, /*forced_extra_client=*/rung);

    if (client_start != refused_start || client_size != refused_size) {
      for (ValidationStats* stats : {&lifetime_, &interval_})
        stats->degrade_attempts.fetch_add(1, std::memory_order_relaxed);

      success = validate_and_test();

      if (success) {
        for (ValidationStats* stats : {&lifetime_, &interval_})
          stats->degrade_rescues.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

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
    const std::vector<const HwcLayer*>& layers, bool use_cursor_plane,
    size_t forced_extra_client) {
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
                             use_cursor_plane, forced_extra_client);
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

/* Which layers the client must punch a hole for.
 *
 * The client draws every layer it kept into one buffer and hands it over as
 * the client target, which is shown on a window of its own at the depth of
 * the layers it holds. A layer this composer took for the hardware and put
 * BELOW that depth is scanned out from its own window, and the client target
 * lies over it -- so wherever that layer sits, the client target has to be
 * transparent, or the pixels it happens to be carrying cover a layer the
 * client never drew.
 *
 * The client does not work this out for itself. It clears under a layer only
 * when the composer asks, by name, through the requests this list becomes
 * (SurfaceFlinger.cpp: the clear is gated on getClearClientTarget, which is
 * exactly what the composer answered). Ask for nothing and the client target
 * keeps whatever its buffer held a few frames ago -- which after a rotation
 * is the picture in the old orientation, laid over the new one.
 *
 * Only what lies below is asked for. A hardware layer ABOVE the client range
 * has its own window in front of the client target and needs no hole; asking
 * for one there would erase what the client did draw. And with no client
 * composition at all there is no target to punch. */
auto GenericCompositionPlanner::GetPunchOutLayers(
    const std::vector<const HwcLayer*>& layers, size_t client_first_z,
    size_t client_size) -> std::vector<const HwcLayer*> {
  std::vector<const HwcLayer*> punch_out;
  if (client_size == 0)
    return punch_out;

  for (size_t z_order = 0; z_order < client_first_z && z_order < layers.size();
       ++z_order) {
    punch_out.push_back(layers[z_order]);
  }
  return punch_out;
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
    size_t client_size, bool use_cursor_plane, size_t forced_extra_client) {
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
  if ((layers_size - client_size > avail_planes ||
       forced_extra_client > 0) &&
      client_size < layers_size) {
    /* The ladder's question arrives through |forced_extra_client|: grow the
     * client range by that much beyond what the plane budget asks, since on
     * this hardware the budget is deliberately plentiful -- the merging
     * window is offered many times over -- and a budget-based retreat would
     * change nothing. Growing the range is the retreat that means
     * something: one layer fewer for the windows and the engine to feed. */
    size_t extra_client = layers_size - client_size > avail_planes
                              ? (layers_size - client_size) - avail_planes
                              : 0;
    extra_client = std::min(extra_client + forced_extra_client,
                            layers_size - client_size);
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

void GenericCompositionPlanner::RecordValidation(uint64_t duration_us) {
  for (ValidationStats* stats : {&lifetime_, &interval_}) {
    stats->calls.fetch_add(1, std::memory_order_relaxed);
    stats->total_us.fetch_add(duration_us, std::memory_order_relaxed);

    uint64_t seen = stats->max_us.load(std::memory_order_relaxed);
    while (seen < duration_us &&
           !stats->max_us.compare_exchange_weak(seen, duration_us,
                                                std::memory_order_relaxed)) {
    }

    const size_t bucket = duration_us < 4 ? 0
                          : duration_us < 16 ? 1
                          : duration_us < 64 ? 2
                          : duration_us < 256 ? 3
                                              : 4;
    stats->buckets[bucket].fetch_add(1, std::memory_order_relaxed);
  }
}

std::string GenericCompositionPlanner::DumpState() {
  const auto print = [](std::stringstream& ss, ValidationStats& stats,
                        bool reset) {
    const auto calls = stats.calls.load(std::memory_order_relaxed);
    const auto reused = stats.reused.load(std::memory_order_relaxed);
    const auto seen = stats.invalidators_seen.load(std::memory_order_relaxed);
    const auto attempts = stats.degrade_attempts.load(
        std::memory_order_relaxed);
    const auto rescues = stats.degrade_rescues.load(std::memory_order_relaxed);
    const auto copy_us = stats.reuse_copy_us.load(std::memory_order_relaxed);
    const auto total = stats.total_us.load(std::memory_order_relaxed);
    const auto max = stats.max_us.load(std::memory_order_relaxed);

    ss << "  plans made              : " << calls << "\n"
       << "  of them reused          : " << reused << "\n"
       << "  invalidators seen       : 0x" << std::hex << seen << std::dec
       << "\n"
       << "  degrade attempts        : " << attempts << "\n"
       << "  degrade rescues         : " << rescues << "\n";
    if (attempts > 0) {
      ss << "  last refusal errno      : "
         << stats.last_refusal_error.load(std::memory_order_relaxed) << "\n";
    }
    if (reused > 0) {
      ss << "  reuse copy time         : " << copy_us << " us (mean "
         << copy_us / reused << ")\n";
    }
    if (calls > 0) {
      ss << "  time spent planning     : " << total << " us"
         << " (mean " << total / calls << ", max " << max << ")\n"
         << "  spread                  :";
      static const char* kNames[] = {" <4us:", " <16us:", " <64us:", " <256us:",
                                     " slower:"};
      for (size_t i = 0; i < 5; ++i) {
        ss << kNames[i] << stats.buckets[i].load(std::memory_order_relaxed);
      }
      ss << "\n";
    }

    if (reset) {
      stats.calls.store(0, std::memory_order_relaxed);
      stats.reused.store(0, std::memory_order_relaxed);
      stats.invalidators_seen.store(0, std::memory_order_relaxed);
      stats.degrade_attempts.store(0, std::memory_order_relaxed);
      stats.degrade_rescues.store(0, std::memory_order_relaxed);
      stats.last_refusal_error.store(0, std::memory_order_relaxed);
      stats.reuse_copy_us.store(0, std::memory_order_relaxed);
      stats.total_us.store(0, std::memory_order_relaxed);
      stats.max_us.store(0, std::memory_order_relaxed);
      for (auto& bucket : stats.buckets)
        bucket.store(0, std::memory_order_relaxed);
    }
  };

  std::stringstream ss;
  ss << "Validation since last dumpsys request:\n";
  print(ss, interval_, /*reset=*/true);
  ss << "Validation over its lifetime:\n";
  print(ss, lifetime_, /*reset=*/false);
  return ss.str();
}

}  // namespace android::drm_hwcomposer
