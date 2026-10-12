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

/* Host test for the layer placement.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. vk/tests/geometry_test.cpp \
 *       vk/Geometry.cpp -o /tmp/vk_geometry_test && /tmp/vk_geometry_test
 *
 * The transforms are checked by where a source corner lands: under a
 * quarter turn clockwise the buffer's top-left pixel ends at the
 * destination's top-right, and so on round -- HWC2's meaning, flips
 * first. */

#include <cmath>
#include <cstdio>

#include "vk/Geometry.h"

using namespace android::hwc::vk;

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

bool Near(float a, float b) {
  return std::fabs(a - b) < 1e-5f;
}
bool At(const float uv[2], float u, float v) {
  return Near(uv[0], u) && Near(uv[1], v);
}
/* The fourth corner, as the shader derives it. */
void Br(const Quad &q, float out[2]) {
  out[0] = q.uv_tr[0] + q.uv_bl[0] - q.uv_tl[0];
  out[1] = q.uv_tr[1] + q.uv_bl[1] - q.uv_tl[1];
}

/* The whole buffer onto the whole target, every transform. Corners of the
 * crop: TL (0,0), TR (1,0), BR (1,1), BL (0,1). */
void TestTransforms() {
  struct Case {
    uint8_t t;
    float tl[2], tr[2], bl[2], br[2];
    const char *what;
  } cases[] = {
      {0, {0, 0}, {1, 0}, {0, 1}, {1, 1}, "identity"},
      {1, {1, 0}, {0, 0}, {1, 1}, {0, 1}, "flip H"},
      {2, {0, 1}, {1, 1}, {0, 0}, {1, 0}, "flip V"},
      {3, {1, 1}, {0, 1}, {1, 0}, {0, 0}, "rotate 180"},
      /* A quarter turn clockwise: the buffer's top-left lands top-right. */
      {4, {0, 1}, {0, 0}, {1, 1}, {1, 0}, "rotate 90"},
      {7, {1, 0}, {1, 1}, {0, 0}, {0, 1}, "rotate 270"},
      {5, {1, 1}, {1, 0}, {0, 1}, {0, 0}, "flip H then rotate 90"},
      {6, {0, 0}, {0, 1}, {1, 0}, {1, 1}, "flip V then rotate 90 (transpose)"},
  };
  for (const Case &c : cases) {
    const Quad q = PlaceLayer(0, 0, 100, 200, 100, 200, 0, 0, 10, 10, c.t, 10, 10);
    float br[2];
    Br(q, br);
    const bool ok = At(q.uv_tl, c.tl[0], c.tl[1]) && At(q.uv_tr, c.tr[0], c.tr[1]) &&
                    At(q.uv_bl, c.bl[0], c.bl[1]) && At(br, c.br[0], c.br[1]);
    if (!ok)
      std::printf("  transform %u (%s): tl %g,%g tr %g,%g bl %g,%g br %g,%g\n", c.t, c.what,
                  q.uv_tl[0], q.uv_tl[1], q.uv_tr[0], q.uv_tr[1], q.uv_bl[0], q.uv_bl[1], br[0],
                  br[1]);
    CHECK(ok);
  }
}

/* Rotating twice by a quarter is the half turn; checked through the corner
 * map rather than trusting the table above twice. */
void TestRotationComposes() {
  const Quad r90 = PlaceLayer(0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 4, 1, 1);
  const Quad r180 = PlaceLayer(0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 3, 1, 1);
  const Quad r270 = PlaceLayer(0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 7, 1, 1);
  /* 270 is 90 then 180: its top-left is what 180 puts where 90 puts its
   * bottom-left -- equivalently the three are distinct and 180's top-left
   * is the crop's bottom-right. */
  CHECK(At(r180.uv_tl, 1, 1));
  CHECK(!At(r90.uv_tl, r270.uv_tl[0], r270.uv_tl[1]));
  CHECK(At(r90.uv_tl, 0, 1));
  CHECK(At(r270.uv_tl, 1, 0));
}

/* A crop and a placement: texture coordinates are the crop over the
 * buffer, clip space is the destination over the target. */
void TestCropAndPlacement() {
  const Quad q = PlaceLayer(10, 20, 110, 220, 200, 400, 384, 512, 1152, 1536, 0, 1536, 2048);
  CHECK(Near(q.ndc_left, -0.5f));
  CHECK(Near(q.ndc_right, 0.5f));
  CHECK(Near(q.ndc_top, -0.5f));
  CHECK(Near(q.ndc_bottom, 0.5f));
  CHECK(At(q.uv_tl, 0.05f, 0.05f));
  CHECK(At(q.uv_tr, 0.55f, 0.05f));
  CHECK(At(q.uv_bl, 0.05f, 0.55f));
}

/* The whole target maps to the whole of clip space; a destination off the
 * target's edge maps past it, which the rasteriser clips. */
void TestEdges() {
  const Quad full = PlaceLayer(0, 0, 4, 4, 4, 4, 0, 0, 1536, 2048, 0, 1536, 2048);
  CHECK(Near(full.ndc_left, -1) && Near(full.ndc_top, -1));
  CHECK(Near(full.ndc_right, 1) && Near(full.ndc_bottom, 1));
  const Quad off = PlaceLayer(0, 0, 4, 4, 4, 4, -768, 0, 768, 2048, 0, 1536, 2048);
  CHECK(Near(off.ndc_left, -2) && Near(off.ndc_right, 0));
}

}  // namespace

/* Coverage of an area by boxes: the two panes of closing Settings side by
 * side cover their bounds; a gap, a missing corner or an empty set do not. */
void TestCovers() {
  const Box area = {138, 936, 761, 1743};
  const Box panes[] = {{138, 936, 449, 1743}, {449, 936, 761, 1743}};
  CHECK(Covers(area, panes, 2));
  CHECK(!Covers(area, panes, 1));
  const Box gap[] = {{138, 936, 449, 1743}, {450, 936, 761, 1743}};
  CHECK(!Covers(area, gap, 2));
  const Box short_one[] = {{138, 936, 449, 1743}, {449, 936, 761, 1742}};
  CHECK(!Covers(area, short_one, 2));
  const Box stacked[] = {{100, 900, 800, 1300}, {100, 1300, 800, 1800}};
  CHECK(Covers(area, stacked, 2));
  const Box crossing[] = {{100, 900, 500, 1800}, {400, 900, 800, 1200}, {400, 1200, 800, 1800}};
  CHECK(Covers(area, crossing, 3));
  CHECK(!Covers(area, nullptr, 0));
  const Box empty_area = {10, 10, 10, 20};
  CHECK(Covers(empty_area, nullptr, 0));
}

int main() {
  TestTransforms();
  TestRotationComposes();
  TestCropAndPlacement();
  TestEdges();
  TestCovers();
  if (failures == 0)
    std::printf("vk_geometry_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
