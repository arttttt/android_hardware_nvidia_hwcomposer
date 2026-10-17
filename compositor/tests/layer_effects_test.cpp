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

/* Host test for reading the effects SurfaceFlinger sends.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. compositor/tests/layer_effects_test.cpp \
 *       compositor/LayerEffects.cpp -o /tmp/layer_effects_test && \
 *       /tmp/layer_effects_test
 *
 * The values are built the way the SF patch builds them: a vector of
 * floats copied byte for byte. */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "compositor/LayerEffects.h"

using namespace android::drm_hwcomposer;

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

EffectResult Apply(const char *key, const std::vector<float> &v, LayerEffects *e) {
  return ApplyEffectMetadata(key, std::strlen(key), reinterpret_cast<const uint8_t *>(v.data()),
                             v.size() * sizeof(float), e);
}

void TestCorners() {
  LayerEffects e;
  CHECK(!e.Any());
  CHECK(Apply(kEffectKeys[0], {10, 20, 1546, 2068, 32}, &e) == EffectResult::kApplied);
  CHECK(e.HasCorners() && e.Any());
  CHECK(e.corners[2] == 1546 && e.corners[4] == 32);
  /* All zero is no corners, and is how SF says it once they are gone. */
  CHECK(Apply(kEffectKeys[0], {0, 0, 0, 0, 0}, &e) == EffectResult::kApplied);
  CHECK(!e.HasCorners() && !e.Any());
}

/* A radius under the floor is a square corner: the layer keeps a window.
 * The floor is the property's; zero, the default here, draws every one. */
void TestCornerFloor() {
  LayerEffects e;
  Apply(kEffectKeys[0], {0, 0, 768, 1940, 1.0F}, &e);
  CHECK(e.HasCorners() && e.Any());
  LayerEffects::min_corner_radius = 2.0F;
  CHECK(!e.HasCorners() && !e.Any());
  Apply(kEffectKeys[0], {0, 0, 768, 1940, 2.0F}, &e);
  CHECK(e.HasCorners() && e.Any());
  LayerEffects::min_corner_radius = 4.0F;
  CHECK(!e.HasCorners());
  Apply(kEffectKeys[0], {0, 0, 768, 1940, 5.13F}, &e);
  CHECK(e.HasCorners());
  LayerEffects::min_corner_radius = 0.F;
  Apply(kEffectKeys[0], {0, 0, 768, 1940, 0.23F}, &e);
  CHECK(e.HasCorners());
}

void TestResetByEmptyValue() {
  LayerEffects e;
  Apply(kEffectKeys[0], {0, 0, 100, 100, 8}, &e);
  CHECK(ApplyEffectMetadata(kEffectKeys[0], std::strlen(kEffectKeys[0]), nullptr, 0, &e) ==
        EffectResult::kApplied);
  CHECK(!e.HasCorners());
}

/* A wrong length, a byte count that is not floats, a negative radius, a
 * NaN: refused, and the layer keeps what it had. */
void TestMalformed() {
  LayerEffects e;
  Apply(kEffectKeys[0], {0, 0, 100, 100, 8}, &e);
  const LayerEffects before = e;
  CHECK(Apply(kEffectKeys[0], {0, 0, 100}, &e) == EffectResult::kMalformed);
  CHECK(Apply(kEffectKeys[0], {0, 0, 100, 100, -1}, &e) == EffectResult::kMalformed);
  CHECK(Apply(kEffectKeys[0], {0, 0, 100, 100, std::numeric_limits<float>::quiet_NaN()}, &e) ==
        EffectResult::kMalformed);
  const uint8_t three[3] = {1, 2, 3};
  CHECK(ApplyEffectMetadata(kEffectKeys[0], std::strlen(kEffectKeys[0]), three, 3, &e) ==
        EffectResult::kMalformed);
  CHECK(Apply(kEffectKeys[1], std::vector<float>(16, 1.F), &e) == EffectResult::kMalformed);
  CHECK(e == before);
}

void TestShadowBlurStretch() {
  LayerEffects e;
  std::vector<float> shadow(17, 0.F);
  shadow[4] = 24;  /* length */
  shadow[8] = 0.3F;
  CHECK(Apply(kEffectKeys[1], shadow, &e) == EffectResult::kApplied);
  CHECK(e.shadow.size() == 17 && e.Any());

  /* Background radius only, no regions. */
  CHECK(Apply(kEffectKeys[2], {40, 0}, &e) == EffectResult::kApplied);
  /* Two regions say twenty-two words; twelve is not that. */
  std::vector<float> short_two(12, 1.F);
  short_two[1] = 2;
  CHECK(Apply(kEffectKeys[2], short_two, &e) == EffectResult::kMalformed);
  /* And one region is exactly twelve. */
  CHECK(Apply(kEffectKeys[2], std::vector<float>(12, 1.F), &e) == EffectResult::kApplied);
  std::vector<float> two(22, 1.F);
  two[1] = 2;
  CHECK(Apply(kEffectKeys[2], two, &e) == EffectResult::kApplied);
  CHECK(e.blur.size() == 22);

  CHECK(Apply(kEffectKeys[3], std::vector<float>(10, 0.F), &e) == EffectResult::kApplied);
  CHECK(Apply(kEffectKeys[3], std::vector<float>(9, 0.F), &e) == EffectResult::kMalformed);

  /* Everything back to zero: nothing to draw. */
  Apply(kEffectKeys[1], std::vector<float>(17, 0.F), &e);
  Apply(kEffectKeys[2], {0, 0}, &e);
  CHECK(!e.Any());
}

/* A turned layer: six floats, kept as they came; all zero is not turned,
 * any other length is refused and the last good corners stay. A turn is
 * an effect -- the layer is the client's to draw. */
void TestTurned() {
  LayerEffects e;
  const char *key = kEffectKeys[4];
  CHECK(Apply(key, {1200, 0, 1200, 100, 1000, 0}, &e) == EffectResult::kApplied);
  CHECK(e.HasTurn());
  CHECK(e.Any());
  CHECK(!e.ReachesPastFrame());
  CHECK(e.turned[0] == 1200 && e.turned[3] == 100 && e.turned[4] == 1000);

  CHECK(Apply(key, {1, 2, 3}, &e) == EffectResult::kMalformed);
  CHECK(e.turned[0] == 1200);
  const float inf = std::numeric_limits<float>::infinity();
  CHECK(Apply(key, {inf, 0, 0, 0, 0, 0}, &e) == EffectResult::kMalformed);

  CHECK(Apply(key, std::vector<float>(6, 0.F), &e) == EffectResult::kApplied);
  CHECK(!e.HasTurn());
  CHECK(!e.Any());

  /* An empty value resets the key, as for the others. */
  Apply(key, {1, 1, 2, 1, 1, 2}, &e);
  CHECK(ApplyEffectMetadata(key, std::strlen(key), nullptr, 0, &e) == EffectResult::kApplied);
  CHECK(!e.HasTurn());

  /* Two layers differ by their turn alone. */
  LayerEffects a;
  LayerEffects b;
  Apply(key, {1, 1, 2, 1, 1, 2}, &a);
  CHECK(a != b);
}

void TestUnknownKey() {
  LayerEffects e;
  CHECK(Apply("org.lineageos.mocha.glow", {1}, &e) == EffectResult::kUnknownKey);
  /* A prefix of a real key is not the key. */
  CHECK(ApplyEffectMetadata(kEffectKeys[0], 10, nullptr, 0, &e) == EffectResult::kUnknownKey);
}

}  // namespace

int main() {
  TestCorners();
  TestCornerFloor();
  TestResetByEmptyValue();
  TestMalformed();
  TestShadowBlurStretch();
  TestTurned();
  TestUnknownKey();
  if (failures == 0)
    std::printf("layer_effects_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
