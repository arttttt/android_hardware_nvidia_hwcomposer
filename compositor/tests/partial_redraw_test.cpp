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
 *       compositor/PictureMemory.cpp \
 *       -o /tmp/partial_redraw_test && /tmp/partial_redraw_test
 *
 * The group is the panel's strips: the status bar at the top and the
 * taskbar at the bottom of a 1536x2048 merge. */

#include <cstdio>
#include <memory>

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

constexpr Engine kVic{false, true, false};
constexpr Engine kGpu{true, false, true};

constexpr uint32_t kW = 1536;
constexpr uint32_t kH = 2048;

Member StatusBar(uint64_t gen) {
  Member m;
  m.serial = 1;
  m.source = 11;
  m.generation = gen;
  m.src = {0, 0, 1536, 43};
  m.dst = {0, 0, 1536, 43};
  return m;
}

Member Taskbar(uint64_t gen) {
  Member m;
  m.serial = 2;
  m.source = 12;
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
  const Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(6), Taskbar(3)}, h, kVic);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts == (std::vector<IRect>{{16, 8, 112, 36}}));
}

/* A taskbar damaged in its own pixels lands where the taskbar is. */
void TestOffsetIntoGroup() {
  DamageHistory h;
  h.Record(2, 4, {{100, 10, 200, 60}});
  const Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(5), Taskbar(4)}, h, kVic);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts == (std::vector<IRect>{{96, 1912, 208, 1964}}));
}

/* A buffer three frames old gathers all three frames' damage; two places
 * close together are drawn as one. */
void TestChainOfFrames() {
  DamageHistory h;
  h.Record(1, 6, {{20, 8, 40, 36}});
  h.Record(1, 7, {{300, 8, 340, 36}});
  h.Record(1, 8, {{0, 0, 0, 0}});
  const Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(8), Taskbar(3)}, h, kVic);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts == (std::vector<IRect>{{16, 8, 352, 36}}));
}

/* Nothing changed since the buffer was drawn: nothing to draw. Damage that
 * says "nothing" for a new frame changes nothing either. */
void TestUnchanged() {
  DamageHistory h;
  CHECK(Decide(Held(5, 3), kW, kH, {StatusBar(5), Taskbar(3)}, h, kVic).kind ==
        Plan::Kind::kUnchanged);
  h.Record(1, 6, {{0, 0, 0, 0}});
  CHECK(Decide(Held(5, 3), kW, kH, {StatusBar(6), Taskbar(3)}, h, kVic).kind ==
        Plan::Kind::kUnchanged);
}

/* The strips as a full drawing writes them: each where it is, not the
 * screen between them. */
const std::vector<IRect> kStrips = {{0, 0, 1536, 44}, {0, 1902, 1536, 2048}};

/* Whatever cannot be accounted for is drawn in full -- the members' places,
 * not the buffer; only a buffer nothing is known of is written whole. */
void TestFull() {
  DamageHistory h;
  h.Record(1, 6, {{20, 8, 110, 36}});
  const std::vector<Member> now = {StatusBar(6), Taskbar(3)};

  Snapshot never;
  Plan p = Decide(never, kW, kH, now, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts.empty());
  CHECK(p.why == 'h');

  /* A frame whose damage was never seen. */
  p = Decide(Held(4, 3), kW, kH, now, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts == kStrips);
  CHECK(p.why == 'd');

  /* The whole of the taskbar changed: still less than the strips. */
  h.Record(2, 4, {});
  p = Decide(Held(6, 3), kW, kH, {StatusBar(6), Taskbar(4)}, h, kVic);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts == (std::vector<IRect>{{0, 1902, 1536, 2048}}));
  CHECK(p.why == 'n');

  /* A member moved: where it was and where it is are one part. */
  auto moved = now;
  moved[1].dst = {0, 1940, 1536, 2085};
  p = Decide(Held(5, 3), kW, kH, moved, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts == kStrips);
  CHECK(p.why == 'i');

  /* Drawn scaled, by an engine that cannot cut it: whole. */
  Snapshot scaled = Held(5, 3);
  scaled.members[1].src = {0, 0, 768, 72};
  auto scaled_now = now;
  scaled_now[1].src = {0, 0, 768, 72};
  p = Decide(scaled, kW, kH, scaled_now, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts.empty());
  CHECK(p.why == 's');

  /* Too many partial redraws in a row. */
  Snapshot tired = Held(5, 3);
  tired.partial_run = kMostPartialInARow;
  p = Decide(tired, kW, kH, now, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts == kStrips);
  CHECK(p.why == 'r');

  /* A different group size: nothing known of the buffer. */
  p = Decide(Held(5, 3), kW, 1024, now, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts.empty());
}

/* A member that left has its place cleared. The GPU clears a part with
 * nothing in it; the image engine needs a member to draw a part at all, so
 * the cleared place joins the nearest part that has one. */
void TestLeft() {
  DamageHistory h;
  Plan p = Decide(Held(5, 3), kW, kH, {StatusBar(6)}, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.why == 'c');
  CHECK(p.parts == (std::vector<IRect>{{0, 0, 1536, 2048}}));

  const Member pill = [] {
    Member m;
    m.serial = 7;
    m.source = 17;
    m.generation = 1;
    m.src = {0, 0, 200, 20};
    m.dst = {668, 1960, 868, 1980};
    return m;
  }();
  Snapshot s = Held(5, 3);
  s.members.push_back(pill);
  p = Decide(s, kW, kH, {StatusBar(5), Taskbar(3)}, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts == kStrips);
}

/* The status bar and the taskbar changed in the same frame: two parts for
 * the engine that draws a part at a time, not the screen between them; the
 * box around both for the GPU, which then draws its members in full. */
void TestBothStrips() {
  DamageHistory h;
  h.Record(1, 6, {{20, 8, 110, 36}});
  h.Record(2, 4, {{100, 10, 200, 60}});
  const std::vector<Member> now = {StatusBar(6), Taskbar(4)};
  Plan p = Decide(Held(5, 3), kW, kH, now, h, kVic);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts ==
        (std::vector<IRect>{{16, 8, 112, 36}, {96, 1912, 208, 1964}}));
  CHECK(Area(p.parts) < 20000);

  p = Decide(Held(5, 3), kW, kH, now, h, kGpu);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts == (std::vector<IRect>{{16, 8, 208, 1964}}));
}

/* Drawing in part is drawing less, or the whole of what is held is drawn
 * instead. */
void TestShare() {
  DamageHistory h;
  Member big;
  big.serial = 3;
  big.source = 13;
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
  CHECK(Decide(s, kW, kH, {big}, h, kVic).kind == Plan::Kind::kPartial);
  big.generation = 3;
  h.Record(3, 3, {});
  const Plan p = Decide(s, kW, kH, {big}, h, kVic);
  CHECK(p.kind == Plan::Kind::kFull);
  CHECK(p.parts == (std::vector<IRect>{{0, 0, 1536, 2048}}));
}

/* Scattered damage is drawn in a bounded number of parts, the nearest
 * joined first. */
void TestMostParts() {
  Member tall;
  tall.serial = 8;
  tall.source = 18;
  tall.src = {0, 0, 1536, 2048};
  tall.dst = {0, 0, 1536, 2048};
  tall.generation = 1;
  Snapshot s;
  s.valid = true;
  s.width = kW;
  s.height = kH;
  s.members = {tall};
  DamageHistory h;
  std::vector<IRect> spots;
  for (int32_t i = 0; i < 6; ++i)
    spots.push_back({0, i * 400, 1536, i * 400 + 40});
  h.Record(8, 2, spots);
  tall.generation = 2;
  const Plan p = Decide(s, kW, kH, {tall}, h, kVic);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts.size() == kMostParts);
  CHECK(Area(p.parts) < static_cast<uint64_t>(kW) * kH / 2);
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

/* A member cut to the region: place and source alike, in proportion when
 * scaled; one outside the region is left out. */
void TestClip() {
  FRect src{0, 0, 1536, 145};
  IRect dst{0, 1903, 1536, 2048};
  CHECK(ClipTo({96, 1912, 208, 1964}, &src, &dst));
  CHECK(dst == (IRect{96, 1912, 208, 1964}));
  CHECK(src == (FRect{96, 9, 208, 61}));

  FRect half{0, 0, 768, 1024};
  IRect full{0, 0, 1536, 2048};
  CHECK(ClipTo({16, 8, 112, 36}, &half, &full));
  CHECK(half == (FRect{8, 4, 56, 18}));

  FRect bar{0, 0, 1536, 43};
  IRect top{0, 0, 1536, 43};
  CHECK(!ClipTo({96, 1912, 208, 1964}, &bar, &top));
  CHECK(top == (IRect{0, 0, 1536, 43}));
}

/* The GPU cuts by its scissor, so a scaled member is drawn in part, its
 * damage taken through the scale and grown by the filter's reach; the
 * image engine draws it whole. */
void TestScaled() {
  Member wall;
  wall.serial = 4;
  wall.source = 14;
  wall.generation = 1;
  wall.src = {0, 0, 768, 1024};
  wall.dst = {0, 0, 1536, 2048};
  Snapshot s;
  s.valid = true;
  s.width = kW;
  s.height = kH;
  s.members = {wall};
  DamageHistory h;
  h.Record(4, 2, {{10, 10, 20, 20}});
  wall.generation = 2;
  CHECK(Decide(s, kW, kH, {wall}, h, kVic).kind == Plan::Kind::kFull);
  const Plan p = Decide(s, kW, kH, {wall}, h, kGpu);
  CHECK(p.kind == Plan::Kind::kPartial);
  CHECK(p.parts == (std::vector<IRect>{{16, 18, 48, 42}}));
}

/* A solid colour has no frames: a new colour, or new corners, is a
 * different picture, drawn whole. */
void TestColourAndCorners() {
  Member dim;
  dim.serial = 5;
  dim.solid = true;
  dim.color = {0, 0, 0, 0.6F};
  dim.dst = {0, 0, 1536, 2048};
  Snapshot s;
  s.valid = true;
  s.width = kW;
  s.height = kH;
  s.members = {dim};
  DamageHistory h;
  CHECK(Decide(s, kW, kH, {dim}, h, kGpu).kind == Plan::Kind::kUnchanged);
  Member darker = dim;
  darker.color[3] = 0.7F;
  CHECK(Decide(s, kW, kH, {darker}, h, kGpu).kind == Plan::Kind::kFull);
  Member rounder = dim;
  rounder.corners = {0, 0, 1536, 2048, 4.F};
  CHECK(Decide(s, kW, kH, {rounder}, h, kGpu).kind == Plan::Kind::kFull);
  Member nameless = dim;
  nameless.serial = 0;
  s.members = {nameless};
  CHECK(Decide(s, kW, kH, {nameless}, h, kGpu).kind == Plan::Kind::kFull);
}

/* The slots: drawn whole until something is held, held pictures shown
 * undrawn only with a fence to show them against, and forgotten when lost;
 * partial redraws counted in a row and reset by a whole one. */
void TestSlots() {
  SlotPictures slots(kVic);
  int a = 0;
  const std::vector<Member> pic = {StatusBar(5), Taskbar(3)};
  CHECK(slots.Decide(&a, kW, kH, pic).kind == Plan::Kind::kFull);

  slots.Drawn(&a, kW, kH, pic, Plan{}, nullptr);
  CHECK(slots.Decide(&a, kW, kH, pic).kind == Plan::Kind::kFull);

  slots.Drawn(&a, kW, kH, pic, Plan{}, std::make_shared<int>(-1));
  CHECK(slots.Decide(&a, kW, kH, pic).kind == Plan::Kind::kUnchanged);

  slots.Lost(&a);
  CHECK(slots.Decide(&a, kW, kH, pic).kind == Plan::Kind::kFull);

  slots.Record(StatusBar(6), {{20, 8, 110, 36}});
  std::vector<Member> next = {StatusBar(6), Taskbar(3)};
  slots.Drawn(&a, kW, kH, pic, Plan{}, std::make_shared<int>(-1));
  Plan p = slots.Decide(&a, kW, kH, next);
  CHECK(p.kind == Plan::Kind::kPartial);
  for (uint32_t i = 0; i < kMostPartialInARow; ++i)
    slots.Drawn(&a, kW, kH, pic, p, std::make_shared<int>(-1));
  CHECK(slots.Decide(&a, kW, kH, next).kind == Plan::Kind::kFull);
  slots.Drawn(&a, kW, kH, pic, Plan{}, std::make_shared<int>(-1));
  CHECK(slots.Decide(&a, kW, kH, next).kind == Plan::Kind::kPartial);
}

}  // namespace

int main() {
  TestClip();
  TestScaled();
  TestColourAndCorners();
  TestSlots();
  TestClockTick();
  TestOffsetIntoGroup();
  TestChainOfFrames();
  TestUnchanged();
  TestFull();
  TestLeft();
  TestBothStrips();
  TestShare();
  TestMostParts();
  TestHistoryBounds();
  if (failures == 0)
    std::printf("partial_redraw_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
