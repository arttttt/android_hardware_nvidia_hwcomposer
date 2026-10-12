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

/* Host test for the partial redraw decision.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. \
 *       compositor/tests/partial_redraw_test.cpp compositor/PartialRedraw.cpp \
 *       -o /tmp/partial_redraw_test && /tmp/partial_redraw_test
 *
 * The group is the panel's strips: the status bar at the top and the
 * taskbar at the bottom of a 1536x2048 merge. */

#include <cstdio>

#include "compositor/PartialRedraw.h"

using namespace android::drm_hwcomposer;
using namespace android::drm_hwcomposer::partial;

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                                 \
    }                                                             \
  } while (0)

constexpr uint32_t kW = 1536;
constexpr uint32_t kH = 2048;

Member StatusBar(uint64_t gen) {
  Member m;
  m.serial = 1;
  m.generation = gen;
  m.src = {0, 0, 1536, 43};
  m.dst = {0, 0, 1536, 43};
  return m;
}

Member Taskbar(uint64_t gen) {
  Member m;
  m.serial = 2;
  m.generation = gen;
  m.src = {0, 0, 1536, 145};
  m.dst = {0, 1903, 1536, 2048};
  return m;
}

Snapshot Held(uint64_t status_gen, uint64_t taskbar_gen) {
  Snapshot s;
  s.valid = true;
  s.width = kW;
  s.height = kH;
  s.members = {StatusBar(status_gen), Taskbar(taskbar_gen)};
  return s;
}

/* The clock ticks: only its rectangle of the status bar is drawn again. */
void TestClockTick() {
  DamageHistory h;
  h.Record(1, 6, {{20, 8, 110, 36}});
  const Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(6), Taskbar(3)}, h);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.dirty == (IRect{16, 8, 112, 36}));
}

/* A taskbar damaged in its own pixels lands where the taskbar is. */
void TestOffsetIntoGroup() {
  DamageHistory h;
  h.Record(2, 4, {{100, 10, 200, 60}});
  const Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(5), Taskbar(4)}, h);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.dirty == (IRect{96, 1912, 208, 1964}));
}

/* A buffer three frames old gathers all three frames' damage. */
void TestChainOfFrames() {
  DamageHistory h;
  h.Record(1, 6, {{20, 8, 40, 36}});
  h.Record(1, 7, {{300, 8, 340, 36}});
  h.Record(1, 8, {{0, 0, 0, 0}});
  const Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(8), Taskbar(3)}, h);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.dirty == (IRect{16, 8, 352, 36}));
}

/* Nothing changed since the buffer was drawn: nothing to draw. Damage that
 * says "nothing" for a new frame changes nothing either. */
void TestUnchanged() {
  DamageHistory h;
  CHECK(Decide(Held(5, 3), kW, kH, {StatusBar(5), Taskbar(3)}, h).kind ==
        Plan::Kind::kUnchanged);
  h.Record(1, 6, {{0, 0, 0, 0}});
  CHECK(Decide(Held(5, 3), kW, kH, {StatusBar(6), Taskbar(3)}, h).kind ==
        Plan::Kind::kUnchanged);
}

/* Whatever cannot be accounted for draws the buffer whole. */
void TestFull() {
  DamageHistory h;
  h.Record(1, 6, {{20, 8, 110, 36}});
  const std::vector<Member> now = {StatusBar(6), Taskbar(3)};

  Snapshot never;
  CHECK(Decide(never, kW, kH, now, h).kind == Plan::Kind::kFull);

  /* A frame whose damage was never seen. */
  CHECK(Decide(Held(4, 3), kW, kH, now, h).kind == Plan::Kind::kFull);

  /* The whole buffer changed: the taskbar is the bottom strip, but the
   * status bar's whole is under half the group -- partial still. */
  h.Record(2, 4, {});
  Plan p = Decide(Held(6, 3), kW, kH, {StatusBar(6), Taskbar(4)}, h);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.dirty == (IRect{0, 1902, 1536, 2048}));

  /* A member moved, or drawn scaled. */
  auto moved = now;
  moved[1].dst = {0, 1940, 1536, 2085};
  CHECK(Decide(Held(5, 3), kW, kH, moved, h).kind == Plan::Kind::kFull);
  Snapshot scaled = Held(5, 3);
  scaled.members[1].src = {0, 0, 768, 72};
  auto scaled_now = now;
  scaled_now[1].src = {0, 0, 768, 72};
  CHECK(Decide(scaled, kW, kH, scaled_now, h).kind == Plan::Kind::kFull);

  /* Too many partial redraws in a row. */
  Snapshot tired = Held(5, 3);
  tired.partial_run = kMostPartialInARow;
  CHECK(Decide(tired, kW, kH, now, h).kind == Plan::Kind::kFull);

  /* A different group size, or a different set of members. */
  CHECK(Decide(Held(5, 3), kW, 1024, now, h).kind == Plan::Kind::kFull);
  CHECK(Decide(Held(5, 3), kW, kH, {StatusBar(6)}, h).kind ==
        Plan::Kind::kFull);
}

/* Damage past half the group is drawn whole. */
void TestShare() {
  DamageHistory h;
  Member big;
  big.serial = 3;
  big.generation = 2;
  big.src = {0, 0, 1536, 2048};
  big.dst = {0, 0, 1536, 2048};
  Snapshot s;
  s.valid = true;
  s.width = kW;
  s.height = kH;
  big.generation = 1;
  s.members = {big};
  big.generation = 2;
  h.Record(3, 2, {{0, 0, 1536, 1500}});
  CHECK(Decide(s, kW, kH, {big}, h).kind == Plan::Kind::kFull);
}

/* The history keeps a bounded number of frames and layers. */
void TestHistoryBounds() {
  DamageHistory h;
  for (uint64_t g = 1; g <= 20; ++g)
    h.Record(9, g, {{0, 0, 1, 1}});
  std::vector<IRect> rects;
  bool whole = false;
  CHECK(h.Between(9, 15, 20, &rects, &whole));
  CHECK(!h.Between(9, 2, 20, &rects, &whole));
  for (uint64_t s = 100; s < 100 + DamageHistory::kLayers + 5; ++s)
    h.Record(s, 1, {{0, 0, 1, 1}});
  CHECK(!h.Between(9, 19, 20, &rects, &whole));
}

}  // namespace

int main() {
  TestClockTick();
  TestOffsetIntoGroup();
  TestChainOfFrames();
  TestUnchanged();
  TestFull();
  TestShare();
  TestHistoryBounds();
  if (failures == 0)
    std::printf("partial_redraw_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
