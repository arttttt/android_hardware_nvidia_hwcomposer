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

/* Host test for the occlusion.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. compositor/tests/occlusion_test.cpp \
 *       compositor/Occlusion.cpp -o /tmp/occlusion_test && /tmp/occlusion_test
 *
 * The scenes are the panel's own: 1536x2048, the wallpaper under a launcher,
 * an app as two 768-wide halves, the taskbar 145 rows at the bottom. */

#include <cmath>
#include <cstdio>

#include "compositor/Occlusion.h"

using namespace android::drm_hwcomposer;
using namespace android::drm_hwcomposer::occlusion;

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                                 \
    }                                                             \
  } while (0)

const IRect kScreen{0, 0, 1536, 2048};

Layer Opaque(IRect frame) {
  Layer l;
  l.frame = frame;
  l.opaque = true;
  return l;
}

Layer Translucent(IRect frame) {
  Layer l;
  l.frame = frame;
  return l;
}

bool Near(float a, float b) {
  return std::fabs(a - b) < 1e-3F;
}

/* Nothing above: every layer is shown whole. */
void TestNothingAbove() {
  const auto s = Resolve({Opaque(kScreen), Translucent({0, 1903, 1536, 2048})});
  CHECK(!s[0].hidden && s[0].bounds == kScreen);
  CHECK(!s[1].hidden && s[1].bounds == (IRect{0, 1903, 1536, 2048}));
}

/* An opaque app over the wallpaper hides it; a translucent one does not. */
void TestOpaqueHides() {
  auto s = Resolve({Opaque(kScreen), Opaque(kScreen)});
  CHECK(s[0].hidden);
  CHECK(!s[1].hidden);
  s = Resolve({Opaque(kScreen), Translucent(kScreen)});
  CHECK(!s[0].hidden && s[0].bounds == kScreen);
}

/* The app as two halves, side by side: together they hide the wallpaper,
 * which neither does alone. */
void TestHalvesTogether() {
  auto s = Resolve({Opaque(kScreen), Opaque({0, 0, 768, 2048}),
                    Opaque({768, 0, 1536, 2048})});
  CHECK(s[0].hidden);
  s = Resolve({Opaque(kScreen), Opaque({0, 0, 768, 2048})});
  CHECK(!s[0].hidden && s[0].bounds == (IRect{768, 0, 1536, 2048}));
}

/* An opaque taskbar crops the app under it to what is above it; a hole in
 * the middle leaves the bounds as they were, since a ring has the frame's
 * bounds. */
void TestPartial() {
  auto s = Resolve({Opaque(kScreen), Opaque({0, 1903, 1536, 2048})});
  CHECK(!s[0].hidden && s[0].bounds == (IRect{0, 0, 1536, 1903}));
  s = Resolve({Opaque(kScreen), Opaque({100, 100, 1436, 1948})});
  CHECK(!s[0].hidden && s[0].bounds == kScreen);
}

/* Rounded corners: inside them the layer is opaque, the corners are not.
 * The end of a recents expansion -- the app nearly full screen, a radius
 * under a pixel -- leaves the wallpaper seen at the corners and the top
 * row, never hidden; two halves under one rounded crop leave only the four
 * outer corners. */
void TestRoundedCorners() {
  Layer app = Opaque(kScreen);
  app.corner_crop = kScreen;
  app.radius = 3.6F;
  auto s = Resolve({Opaque(kScreen), app});
  CHECK(!s[0].hidden && s[0].bounds == kScreen);

  Layer left = Opaque({0, 1, 768, 1939});
  Layer right = Opaque({768, 1, 1536, 1939});
  left.corner_crop = right.corner_crop = IRect{0, 1, 1536, 1939};
  left.radius = right.radius = 0.2F;
  s = Resolve({Opaque(kScreen), left, right});
  CHECK(!s[0].hidden && s[0].bounds == kScreen);

  /* A rounded layer still hides what lies wholly inside its cross. */
  s = Resolve({Opaque({500, 500, 600, 600}), app});
  CHECK(s[0].hidden);
  /* ... and not what reaches into a corner. */
  s = Resolve({Opaque({0, 0, 2, 2}), app});
  CHECK(!s[0].hidden);
}

/* SurfaceFlinger's region narrows what is seen; one that misses the frame
 * leaves nothing. */
void TestSurfaceFlingerRegion() {
  Layer app = Opaque(kScreen);
  app.visible = {{0, 0, 1536, 1000}};
  auto s = Resolve({app});
  CHECK(!s[0].hidden && s[0].bounds == (IRect{0, 0, 1536, 1000}));
  app.visible = {{0, 3000, 1536, 4000}};
  s = Resolve({app});
  CHECK(s[0].hidden);
}

/* A kept layer is neither hidden nor cropped, but hides what is under it
 * when it is opaque. */
void TestKept() {
  Layer cursor = Opaque({0, 0, 64, 64});
  cursor.keep = true;
  Layer shadowed = Opaque(kScreen);
  shadowed.keep = true;
  auto s = Resolve({cursor, shadowed});
  CHECK(!s[0].hidden && s[0].bounds == (IRect{0, 0, 64, 64}));
  s = Resolve({Opaque(kScreen), shadowed});
  CHECK(s[0].hidden);
}

/* The source follows the frame's crop at the layer's own scale, flips
 * reading the source's edge from the frame's opposite one; a quarter turn
 * is not cropped. */
void TestCropSource() {
  const FRect src{0, 0, 768, 1940};
  const IRect frame{230, 331, 768, 1689};  /* the recents card's left half */
  const IRect shown{230, 331, 768, 1000};
  auto c = CropSource(src, frame, shown, false, false, false);
  CHECK(c && Near(c->left, 0) && Near(c->right, 768) && Near(c->top, 0));
  CHECK(c && Near(c->bottom, (1000 - 331) * 1940.F / (1689 - 331)));

  c = CropSource(src, frame, shown, false, true, false);
  CHECK(c && Near(c->top, (1689 - 1000) * 1940.F / (1689 - 331)));
  CHECK(c && Near(c->bottom, 1940));

  c = CropSource({0, 0, 100, 100}, {0, 0, 100, 100}, {10, 0, 100, 100},
                 true, false, false);
  CHECK(c && Near(c->left, 0) && Near(c->right, 90));

  CHECK(!CropSource(src, frame, shown, false, false, true));
  CHECK(!CropSource(src, frame, {0, 0, 10, 10}, false, false, false));
}

}  // namespace

int main() {
  TestNothingAbove();
  TestOpaqueHides();
  TestHalvesTogether();
  TestPartial();
  TestRoundedCorners();
  TestSurfaceFlingerRegion();
  TestKept();
  TestCropSource();
  if (failures == 0)
    std::printf("occlusion_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
