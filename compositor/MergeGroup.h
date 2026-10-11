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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace android::drm_hwcomposer {

/* What the group choice needs to know of one layer of the stack, and
 * nothing else -- so the choice can be checked on a workstation without
 * a pipeline, a plane or a buffer behind it. */
struct GroupMember {
  bool live = false;
  /* The panel rectangle; nullopt is the whole panel, and overlaps
   * everything. */
  struct Frame {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
  };
  std::optional<Frame> frame;
  /* Members of one group must share it: the engine has a single turn
   * for the whole configuration. */
  uint32_t turn = 0;
};

/* A group for the merging window, and the stack reordered around it.
 *
 * `order` lists the original indices bottom to top as they are to be
 * seated, with the group's members adjacent from `begin`. It differs
 * from the original order only where a live layer that sat between
 * members was lowered beneath the group, which is done only when the
 * layer covers none of the members it passes -- so every pair of layers
 * that overlaps keeps its order, and the panel shows the same picture. */
struct GroupChoice {
  std::vector<size_t> order;
  size_t begin = 0;
  size_t width = 0;
  size_t live = 0;
  /* How many live layers were lowered past group members. */
  size_t lowered = 0;
};

/* The group of exactly `width` members carrying the fewest live layers,
 * ties to the lowest; nullopt when no group of that width can share a
 * turn. A contiguous run is the case with nothing lowered, so the choice
 * is never worse than the run. */
std::optional<GroupChoice> ChooseMergeGroup(
    const std::vector<GroupMember> &stack, size_t width);

}  // namespace android::drm_hwcomposer
