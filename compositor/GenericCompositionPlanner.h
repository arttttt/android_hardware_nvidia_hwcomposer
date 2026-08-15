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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "compositor/CompositionPlanner.h"

namespace android::drm_hwcomposer {

enum class CompositionType;
class ICompositorDisplay;
class HwcLayer;

// Implementation of CompositionPlanner built on top of upstream drm uAPI.
class GenericCompositionPlanner : public CompositionPlanner {
 public:
  ~GenericCompositionPlanner() override = default;
  ValidationResult ValidateDisplay(const ICompositorDisplay* display) override;

  std::string DumpState() override;

 private:
  /* What a plan costs to make. Validation runs for every frame the client
   * updates and repeats its work in full even when nothing about the frame
   * has changed; these are the numbers that say what that repetition is
   * worth before anything is built to avoid it. Written by the frame thread,
   * read by the dump's, hence the atomics. */
  struct ValidationStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_us{0};
    std::atomic<uint64_t> max_us{0};
    /* <4us, <16us, <64us, <256us and the rest. */
    std::atomic<uint64_t> buckets[5]{};
  };

  void RecordValidation(uint64_t duration_us);

  ValidationStats lifetime_;
  ValidationStats interval_;
  static std::tuple<size_t, size_t> GetClientLayers(
      const ICompositorDisplay* display,
      const std::vector<const HwcLayer*>& layers, bool use_cursor_plane);
  static bool IsClientLayer(const ICompositorDisplay* display,
                            const HwcLayer* layer);

  static CompositionTypeMap GetCompositionTypes(
      const std::vector<const HwcLayer*>& layers, size_t client_first_z,
      size_t client_size, bool use_cursor_plane);
  static bool HardwareSupportsLayerType(CompositionType comp_type);
  static uint32_t CalcPixOps(const std::vector<const HwcLayer*>& layers,
                             size_t first_z, size_t size);
  static std::tuple<size_t, size_t> GetExtraClientRange(
      const ICompositorDisplay* display,
      const std::vector<const HwcLayer*>& layers, size_t client_start,
      size_t client_size, bool use_cursor_plane);
};

}  // namespace android::drm_hwcomposer
