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

/* Host test for the feature-table parser.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. tegra/tests/window_features_test.cpp \
 *       tegra/WindowFeatures.cpp -o /tmp/window_features_test && \
 *       /tmp/window_features_test
 *
 * The rows are the ones the 3.10 kernel recites for the mocha panel's head:
 * windows 0-2 read every format the controller has and resize by two,
 * window 3 reads the five simple RGB formats and does not resize at all.
 * They stand in for the driver here; the composer itself never carries them.
 */

#include <cstdio>
#include <map>
#include <vector>

#include "tegra/WindowFeatures.h"

using namespace android::hwc;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,  \
              #cond);                                                   \
      failures++;                                                       \
    }                                                                   \
  } while (0)

/* The driver's property codes, as the parser knows them. */
constexpr uint32_t kFormats = 0;
constexpr uint32_t kBlend = 1;
constexpr uint32_t kMaximumSize = 2;
constexpr uint32_t kMaximumScale = 3;
constexpr uint32_t kFilter = 4;
constexpr uint32_t kLayout = 5;
constexpr uint32_t kInvert = 6;
constexpr uint32_t kField = 7;

FeatureEntry Row(uint32_t window, uint32_t option, uint32_t a0 = 0,
                 uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
  return FeatureEntry{window, option, {a0, a1, a2, a3}};
}

std::vector<FeatureEntry> FullWindow(uint32_t w) {
  return {Row(w, kFormats, 0x00FF30F8, 0x19F03E00), Row(w, kBlend, 1),
          Row(w, kMaximumSize, 4096, 1, 4096, 1),
          Row(w, kMaximumScale, 2, 2, 2, 2), Row(w, kFilter, 1, 1),
          Row(w, kLayout, 1, 0, 1), Row(w, kInvert, 1, 1, 1),
          Row(w, kField, 1)};
}

std::vector<FeatureEntry> SimpleWindow(uint32_t w) {
  return {Row(w, kFormats, 0x000030F0, 0), Row(w, kBlend, 1),
          Row(w, kMaximumSize, 4096, 1, 4096, 1),
          Row(w, kMaximumScale, 1, 1, 1, 1), Row(w, kFilter, 0, 0),
          Row(w, kLayout, 1, 0, 1), Row(w, kInvert, 1, 1, 0),
          Row(w, kField, 0)};
}

void AFullWindowIsDescribedWhole() {
  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows(FullWindow(0), caps);

  CHECK(caps.size() == 1);
  const WindowCapabilities &w = caps[0];
  CHECK(w.formats == 0x19F03E0000FF30F8ULL);
  CHECK(w.maxWidth == 4096 && w.minWidth == 1);
  CHECK(w.maxHeight == 4096 && w.minHeight == 1);
  CHECK(w.scaling);
  CHECK(w.maxUpH == 2 && w.maxUpV == 2 && w.maxDownH == 2 && w.maxDownV == 2);
  CHECK(w.pitchLayout && !w.tiledLayout && w.blocklinearLayout);
  CHECK(w.invertH && w.invertV && w.scanColumn);
}

void ASimpleWindowDoesNotResize() {
  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows(SimpleWindow(3), caps);

  const WindowCapabilities &w = caps[3];
  CHECK(w.formats == 0x30F0ULL);
  CHECK(!w.scaling);
  CHECK(w.maxUpH == 1 && w.maxUpV == 1 && w.maxDownH == 1 && w.maxDownV == 1);
  CHECK(w.invertH && w.invertV && !w.scanColumn);
}

void TheWholeHeadKeepsEveryWindowApart() {
  std::vector<FeatureEntry> table;
  for (uint32_t w = 0; w < 3; ++w) {
    std::vector<FeatureEntry> rows = FullWindow(w);
    table.insert(table.end(), rows.begin(), rows.end());
  }
  std::vector<FeatureEntry> simple = SimpleWindow(3);
  table.insert(table.end(), simple.begin(), simple.end());
  CHECK(table.size() == 32);

  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows(table, caps);
  CHECK(caps.size() == 4);
  CHECK(caps[2].scaling && !caps[3].scaling);
  CHECK(caps[1].formats != caps[3].formats);
}

void ARowNobodyReadsStillNamesTheWindow() {
  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows({Row(5, kBlend, 1)}, caps);

  CHECK(caps.size() == 1);
  CHECK(caps.count(5) == 1);
  CHECK(caps[5].formats == 0 && caps[5].maxWidth == 0 && !caps[5].scaling);
}

void AnUnknownPropertyChangesNothing() {
  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows({Row(0, kFormats, 0x30F0, 0), Row(0, 42, 7, 7, 7, 7)}, caps);

  CHECK(caps[0].formats == 0x30F0ULL);
  CHECK(caps[0].maxWidth == 0 && caps[0].maxUpH == 1);
}

void AnEmptyTableDescribesNoWindow() {
  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows({}, caps);
  CHECK(caps.empty());
}

void OneRatioOffOneIsResizing() {
  std::map<uint32_t, WindowCapabilities> caps;
  describeWindows({Row(0, kMaximumScale, 1, 1, 2, 1)}, caps);
  CHECK(caps[0].scaling);
  CHECK(caps[0].maxDownH == 2);
}

}  // namespace

int main() {
  AFullWindowIsDescribedWhole();
  ASimpleWindowDoesNotResize();
  TheWholeHeadKeepsEveryWindowApart();
  ARowNobodyReadsStillNamesTheWindow();
  AnUnknownPropertyChangesNothing();
  AnEmptyTableDescribesNoWindow();
  OneRatioOffOneIsResizing();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("window_features_test: all checks passed\n");
  return 0;
}
