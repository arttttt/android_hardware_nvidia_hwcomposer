/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <optional>
#include <vector>

#include <ui/GraphicTypes.h>

#include "compositor/DisplayInfo.h"
#include "display/DrmMode.h"

namespace android::drm_hwcomposer {

/* Upstream names the high-dynamic-range list through the AIDL graphics
 * common package, which this platform predates. The rest of upstream reaches
 * the same enumeration as `ui::Hdr`, and that one this platform has, so it is
 * the name used here. */
using Hdr = ui::Hdr;

// Per-display capabilities associated with a DrmDisplayPipeline.
class BackendDisplayCapabilities {
 public:
  virtual std::optional<std::vector<Hdr>> GetHdrTypesOverride() const {
    return std::nullopt;
  }

  // Returns true if the Backend implementation has provided an override for the
  // supported ColorModes.
  virtual std::optional<std::vector<ColorMode>> GetColorModeOverrides() const {
    return std::nullopt;
  }

  // Filters the list of DRM modes. Returns only the modes supported by the
  // backend. Only the returned DrmModes will be passed to the backend through
  // AtomicCommitArgs.
  virtual std::vector<DrmMode> FilterModes(
      const std::vector<DrmMode> &modes) const {
    return modes;
  }

  virtual ~BackendDisplayCapabilities() = 0;
};

inline BackendDisplayCapabilities::~BackendDisplayCapabilities() = default;

}  // namespace android::drm_hwcomposer
