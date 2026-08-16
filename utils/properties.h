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

#include <string>

namespace android::drm_hwcomposer {

enum class CtmHandling {
  kDrmOrGpu,    /* Handled by DRM is possible, otherwise by GPU */
  kDrmOrIgnore, /* Handled by DRM is possible, otherwise displayed as is */
};

class Properties {
 public:
  static auto IsPresentFenceNotReliable() -> bool;
  static auto InternalDisplayNames() -> std::string;
  static auto UseOverlayPlanes() -> bool;
  static auto ScaleWithGpu() -> bool;
  static auto EnableVirtualDisplay() -> bool;
  static auto EnableExternalDisplays() -> bool;
  static auto EnableHdcpOnHotplug() -> bool;
  static auto GetCtmHandling() -> CtmHandling;
  static auto BugfixCursorCtmOffset() -> bool;

  /* Whether the display controller's colour pipeline consumes the client's
   * transform. One switch read by all three parties -- the capability
   * claim, the all-client substitution, the backend's consumer -- so they
   * cannot disagree. */
  static auto CmuColorPipeline() -> bool;

  /* Whether the pipeline's resting state corrects the panel's gamut toward
   * sRGB rather than showing the panel as it is. */
  static auto CalibratedColorMode() -> bool;
  static auto GetBackendOverride() -> std::string;
  static auto GetDevicePath() -> std::string;
  static auto UseColorPipeline() -> bool;
  static auto SkipInternalDisplayReset() -> bool;
  static auto ForceColorMode() -> int;
  static auto PersistentHdrEnabled() -> bool;
  static auto ExternalHdrEnabled() -> bool;
  static auto SkipPlaneDamageClips() -> bool;
};

}  // namespace android::drm_hwcomposer
