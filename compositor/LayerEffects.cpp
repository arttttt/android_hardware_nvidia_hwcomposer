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

#include "compositor/LayerEffects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace android::drm_hwcomposer {

namespace {

bool AllZero(const std::vector<float> &v) {
  return std::all_of(v.begin(), v.end(), [](float f) { return f == 0.F; });
}

bool KeyIs(const char *key, size_t length, const char *name) {
  return length == std::strlen(name) && std::memcmp(key, name, length) == 0;
}

/* The value as floats, or false if it is not a whole number of finite
 * ones. */
bool Floats(const uint8_t *value, size_t length, std::vector<float> *out) {
  if (length % sizeof(float) != 0)
    return false;
  out->resize(length / sizeof(float));
  if (length != 0)
    std::memcpy(out->data(), value, length);
  return std::all_of(out->begin(), out->end(), [](float f) { return std::isfinite(f); });
}

}  // namespace

bool LayerEffects::ReachesPastFrame() const {
  return !AllZero(shadow) || !AllZero(blur) || !AllZero(stretch);
}

bool LayerEffects::Any() const {
  return HasCorners() || ReachesPastFrame();
}

EffectResult ApplyEffectMetadata(const char *key, size_t key_length, const uint8_t *value,
                                 size_t value_length, LayerEffects *effects) {
  std::vector<float> v;
  if (!Floats(value, value_length, &v))
    return EffectResult::kMalformed;

  if (KeyIs(key, key_length, kEffectKeys[0])) {
    if (v.empty()) {
      effects->corners = {};
      return EffectResult::kApplied;
    }
    if (v.size() != effects->corners.size() || v[4] < 0.F)
      return EffectResult::kMalformed;
    std::copy(v.begin(), v.end(), effects->corners.begin());
    return EffectResult::kApplied;
  }

  /* The others are kept as they came; their length is checked against
   * what the layout says it must be. */
  std::vector<float> *slot = nullptr;
  size_t need = 0;
  if (KeyIs(key, key_length, kEffectKeys[1])) {
    slot = &effects->shadow;
    need = 17;
  } else if (KeyIs(key, key_length, kEffectKeys[2])) {
    slot = &effects->blur;
    /* Two words, then ten per region, as many as the second word says. */
    if (!v.empty()) {
      if (v.size() < 2 || v[1] < 0.F)
        return EffectResult::kMalformed;
      need = 2 + 10 * static_cast<size_t>(v[1]);
    }
  } else if (KeyIs(key, key_length, kEffectKeys[3])) {
    slot = &effects->stretch;
    need = 10;
  } else {
    return EffectResult::kUnknownKey;
  }
  if (!v.empty() && v.size() != need)
    return EffectResult::kMalformed;
  *slot = std::move(v);
  return EffectResult::kApplied;
}

}  // namespace android::drm_hwcomposer
