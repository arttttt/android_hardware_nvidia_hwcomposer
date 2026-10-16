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

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace android::drm_hwcomposer {

/* The effects SurfaceFlinger leaves to the composer on this device, as it
 * sends them through generic layer metadata (the device tree's
 * patches/frameworks/native/0003, Layer::writeEffectsMetadata): one key
 * each, little-endian floats, all zero for none. The layouts:
 *
 *   rounded_corners  left, top, right, bottom of the rounded crop, radius;
 *                    display space
 *   shadow           caster left, top, right, bottom; length; ambient
 *                    rgba; spot rgba; light x, y, z; light radius
 *   blur             background radius; region count; per region radius,
 *                    corners TL TR BL BR, alpha, left, top, right, bottom
 *   stretch          width, height, vector x, y, max x, y, mapped child
 *                    bounds left, top, right, bottom; the layer's space
 *
 * Shadows, blur and stretch are kept as they came, for the executor to
 * read when it draws them; the corners are read out now. */
struct LayerEffects {
  /* left, top, right, bottom, radius */
  std::array<float, 5> corners{};
  std::vector<float> shadow;
  std::vector<float> blur;
  std::vector<float> stretch;

  /* Whether anything would be drawn differently for these. */
  bool Any() const;
  /* Corners rounded enough to be seen: a radius of at least the floor
   * below, and always more than nothing. */
  bool HasCorners() const {
    return corners[4] > 0.F && corners[4] >= min_corner_radius;
  }
  /* Set once at start (Properties::MinCornerRadius); a radius under it is
   * drawn as a square corner, so the layer keeps a window. */
  static float min_corner_radius;
  /* A shadow, a blur or a stretch: drawn past the layer's frame, or from
   * what lies under it. Every key arrives every frame, zeros for none. */
  bool ReachesPastFrame() const;

  bool operator==(const LayerEffects &o) const {
    return corners == o.corners && shadow == o.shadow && blur == o.blur &&
           stretch == o.stretch;
  }
  bool operator!=(const LayerEffects &o) const { return !(*this == o); }
};

/* The keys this composer declares, in the order getLayerGenericMetadataKey
 * hands them out. */
constexpr std::array<const char *, 4> kEffectKeys = {
    "org.lineageos.mocha.rounded_corners",
    "org.lineageos.mocha.shadow",
    "org.lineageos.mocha.blur",
    "org.lineageos.mocha.stretch",
};

enum class EffectResult {
  kApplied,
  kUnknownKey,
  /* Not a whole number of floats, or the wrong length for the key, or not
   * finite: the layer keeps what it had. */
  kMalformed,
};

/* Applies one key's value to `effects`. An empty value resets the key, as
 * setLayerGenericMetadata's contract says. */
EffectResult ApplyEffectMetadata(const char *key, size_t key_length, const uint8_t *value,
                                 size_t value_length, LayerEffects *effects);

}  // namespace android::drm_hwcomposer
