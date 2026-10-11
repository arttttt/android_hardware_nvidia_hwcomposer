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

/* Host test for the merge group choice.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. compositor/tests/merge_group_test.cpp \
 *       compositor/MergeGroup.cpp -o /tmp/merge_group_test && \
 *       /tmp/merge_group_test
 *
 * The stacks are the ones the Android 12 traces show on the mocha panel,
 * 1536x2048: the home screen with the taskbar drawing on every frame of a
 * transition, between a status bar and the two screen decoration strips
 * the privacy dot keeps on the panel. */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "compositor/MergeGroup.h"

using namespace android::drm_hwcomposer;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                                      \
    }                                                                  \
  } while (0)

GroupMember At(int32_t l, int32_t t, int32_t r, int32_t b, bool live = false,
               uint32_t turn = 0) {
  GroupMember m;
  m.frame = GroupMember::Frame{l, t, r, b};
  m.live = live;
  m.turn = turn;
  return m;
}

GroupMember Whole(bool live = false) {
  GroupMember m;
  m.live = live;
  return m;
}

/* The order is a permutation that keeps every overlapping pair in order. */
bool KeepsOverlaps(const std::vector<GroupMember> &stack,
                   const std::vector<size_t> &order) {
  std::vector<size_t> rank(stack.size());
  for (size_t r = 0; r < order.size(); r++)
    rank[order[r]] = r;
  for (size_t a = 0; a < stack.size(); a++) {
    for (size_t b = a + 1; b < stack.size(); b++) {
      const auto &p = stack[a].frame;
      const auto &q = stack[b].frame;
      const bool overlap =
          !p || !q ||
          (std::max(p->left, q->left) < std::min(p->right, q->right) &&
           std::max(p->top, q->top) < std::min(p->bottom, q->bottom));
      if (overlap && rank[a] > rank[b])
        return false;
    }
  }
  return true;
}

/* Home in a transition: wallpaper, launcher, status bar, a drawing
 * taskbar, the top and bottom decoration strips. Three windows leave a
 * group of three; every contiguous run of three holds a live layer, but
 * the taskbar covers nothing of the status bar and can be lowered past
 * it, leaving the three quiet strips together. */
void TestHomeTaskbarLowered() {
  const std::vector<GroupMember> stack = {
      Whole(),                           // wallpaper
      Whole(true),                       // launcher, animating
      At(0, 0, 1536, 43),                // status bar
      At(0, 1903, 1536, 2048, true),     // taskbar, drawing
      At(0, 0, 1536, 43),                // decoration, top
      At(0, 2005, 1536, 2048),           // decoration, bottom
  };
  auto c = ChooseMergeGroup(stack, 3);
  CHECK(c.has_value());
  if (!c)
    return;
  CHECK(c->live == 0);
  CHECK(c->lowered == 1);
  CHECK(c->begin == 3);
  CHECK((c->order == std::vector<size_t>{0, 1, 3, 2, 4, 5}));
  CHECK(KeepsOverlaps(stack, c->order));
}

/* A quiet contiguous run wins without anything moving. */
void TestQuietRunUntouched() {
  const std::vector<GroupMember> stack = {
      Whole(), Whole(), At(0, 0, 100, 100), Whole(true)};
  auto c = ChooseMergeGroup(stack, 2);
  CHECK(c.has_value());
  if (!c)
    return;
  CHECK(c->live == 0);
  CHECK(c->lowered == 0);
  CHECK(c->begin == 0);
  CHECK((c->order == std::vector<size_t>{0, 1, 2, 3}));
}

/* A live layer that covers a member below it must stay in place: the
 * group takes it rather than reorder overlapping layers. */
void TestOverlappingLiveNotLowered() {
  const std::vector<GroupMember> stack = {
      At(0, 0, 100, 100), At(50, 50, 150, 150, true), At(200, 0, 300, 100)};
  auto c = ChooseMergeGroup(stack, 3);
  CHECK(c.has_value());
  if (!c)
    return;
  CHECK(c->live == 1);
  CHECK(c->lowered == 0);
  CHECK(KeepsOverlaps(stack, c->order));
}

/* A layer with no frame is the whole panel and overlaps everything. */
void TestWholePanelNeverLowered() {
  const std::vector<GroupMember> stack = {At(0, 0, 10, 10), Whole(true),
                                          At(20, 20, 30, 30)};
  auto c = ChooseMergeGroup(stack, 2);
  CHECK(c.has_value());
  if (!c)
    return;
  CHECK(c->lowered == 0);
  CHECK(c->live == 1);
}

/* Lowering is refused when too few layers remain above to fill the
 * group: the live layer is taken instead. */
void TestNoRoomAbove() {
  const std::vector<GroupMember> stack = {At(0, 0, 10, 10),
                                          At(0, 100, 10, 110, true)};
  auto c = ChooseMergeGroup(stack, 2);
  CHECK(c.has_value());
  if (!c)
    return;
  CHECK(c->live == 1);
  CHECK(c->lowered == 0);
  CHECK((c->order == std::vector<size_t>{0, 1}));
}

/* A layer of another turn is lowered when it can be, and otherwise
 * leaves no group from that start. */
void TestForeignTurn() {
  const std::vector<GroupMember> lowered = {
      At(0, 0, 10, 10), At(0, 100, 10, 110, false, 4), At(0, 200, 10, 210)};
  auto c = ChooseMergeGroup(lowered, 2);
  CHECK(c.has_value());
  if (c) {
    CHECK(c->lowered == 1);
    CHECK((c->order == std::vector<size_t>{1, 0, 2}));
    CHECK(KeepsOverlaps(lowered, c->order));
  }

  const std::vector<GroupMember> blocked = {
      At(0, 0, 10, 10), At(0, 0, 10, 10, false, 4)};
  CHECK(!ChooseMergeGroup(blocked, 2).has_value());
}

/* Edges: an empty request, one wider than the stack, the whole stack. */
void TestEdges() {
  const std::vector<GroupMember> stack = {Whole(), Whole(true)};
  CHECK(!ChooseMergeGroup(stack, 0).has_value());
  CHECK(!ChooseMergeGroup(stack, 3).has_value());
  CHECK(!ChooseMergeGroup({}, 1).has_value());
  auto all = ChooseMergeGroup(stack, 2);
  CHECK(all.has_value());
  if (all) {
    CHECK(all->live == 1);
    CHECK((all->order == std::vector<size_t>{0, 1}));
  }
}

/* Ties go to the lower group, as the contiguous steering did. */
void TestTieGoesLow() {
  const std::vector<GroupMember> stack = {Whole(), Whole(), Whole(true),
                                          At(0, 0, 10, 10), At(0, 0, 10, 10)};
  auto c = ChooseMergeGroup(stack, 2);
  CHECK(c.has_value());
  if (c) {
    CHECK(c->live == 0);
    CHECK(c->begin == 0);
  }
}

/* Random stacks of strips and panels: whatever is chosen keeps every
 * overlapping pair in order, puts exactly `width` members together at
 * `begin` sharing one turn, and is never worse than the best contiguous
 * run of the same width. */
void TestRandomStacks() {
  std::srand(1);
  for (int round = 0; round < 20000; round++) {
    const size_t n = 2 + static_cast<size_t>(std::rand() % 7);
    std::vector<GroupMember> stack;
    for (size_t i = 0; i < n; i++) {
      GroupMember m;
      m.live = std::rand() % 3 == 0;
      m.turn = std::rand() % 8 == 0 ? 4 : 0;
      if (std::rand() % 4 != 0) {
        const int32_t top = (std::rand() % 8) * 256;
        const int32_t h = 43 + std::rand() % 400;
        m.frame = GroupMember::Frame{0, top, 1536, top + h};
      }
      stack.push_back(m);
    }
    const size_t width = 1 + static_cast<size_t>(std::rand()) % n;

    size_t best_run = SIZE_MAX;
    for (size_t b = 0; b + width <= n; b++) {
      bool uniform = true;
      size_t live = 0;
      for (size_t i = b; i < b + width; i++) {
        uniform = uniform && stack[i].turn == stack[b].turn;
        live += stack[i].live ? 1 : 0;
      }
      if (uniform)
        best_run = std::min(best_run, live);
    }

    auto c = ChooseMergeGroup(stack, width);
    if (best_run != SIZE_MAX)
      CHECK(c.has_value());
    if (!c)
      continue;
    CHECK(c->order.size() == n);
    CHECK(KeepsOverlaps(stack, c->order));
    CHECK(c->begin + width <= n);
    CHECK(c->live <= best_run);
    size_t live = 0;
    for (size_t r = c->begin; r < c->begin + width; r++) {
      CHECK(stack[c->order[r]].turn == stack[c->order[c->begin]].turn);
      live += stack[c->order[r]].live ? 1 : 0;
    }
    CHECK(live == c->live);
    std::vector<size_t> sorted = c->order;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < n; i++)
      CHECK(sorted[i] == i);
    if (failures > 10)
      return;
  }
}

/* A large scene of alternating drawing and quiet strips, none touching:
 * the budget ends the search, and what comes back is still a valid
 * seating no worse than the best contiguous run. */
void TestBudgetBoundsLargeScene() {
  std::vector<GroupMember> stack;
  for (int i = 0; i < 40; i++)
    stack.push_back(At(0, i * 50, 1536, i * 50 + 40, i % 2 == 0));
  auto c = ChooseMergeGroup(stack, 12);
  CHECK(c.has_value());
  if (!c)
    return;
  CHECK(KeepsOverlaps(stack, c->order));
  CHECK(c->live <= 6);
}

}  // namespace

int main() {
  TestHomeTaskbarLowered();
  TestQuietRunUntouched();
  TestOverlappingLiveNotLowered();
  TestWholePanelNeverLowered();
  TestNoRoomAbove();
  TestForeignTurn();
  TestEdges();
  TestTieGoesLow();
  TestRandomStacks();
  TestBudgetBoundsLargeScene();
  if (failures == 0)
    std::printf("merge_group_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
