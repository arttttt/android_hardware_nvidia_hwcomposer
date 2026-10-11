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

#include "compositor/MergeGroup.h"

#include <algorithm>

namespace android::drm_hwcomposer {

namespace {

bool Overlaps(const GroupMember &a, const GroupMember &b) {
  /* The whole panel covers everything, and so does a layer whose place is
   * not known: lowering it past anything would be a guess. */
  if (!a.frame || !b.frame)
    return true;
  const GroupMember::Frame &p = *a.frame;
  const GroupMember::Frame &q = *b.frame;
  return std::max(p.left, q.left) < std::min(p.right, q.right) &&
         std::max(p.top, q.top) < std::min(p.bottom, q.bottom);
}

/* The groups that start at `begin`, searched whole. Walking up the stack,
 * each layer is taken into the group, or -- when it is live, or cannot
 * share the group's turn -- may instead be lowered beneath it: allowed
 * when it covers none of the members already taken (those are below it
 * now and would be above it after; members above it stay above it) and
 * enough layers remain above to fill the group. Taking a live layer can
 * be the better branch, because lowering one pushes the group up into
 * whatever lies above, so both are tried. Only live and foreign layers
 * branch, and a branch already carrying as many live layers as the best
 * so far is cut, so the walk stays small. It runs at validate, so it is
 * also bounded outright: past a budget of steps no layer is lowered any
 * more, and the remaining starts are judged as the contiguous runs they
 * would have been before. */
class GroupSearch {
 public:
  GroupSearch(const std::vector<GroupMember> &stack, size_t width)
      : stack_(stack), width_(width) {
    members_.reserve(width);
  }

  void From(size_t begin) {
    begin_ = begin;
    turn_ = stack_[begin].turn;
    members_.assign(1, begin);
    lowered_.clear();
    Walk(begin + 1, stack_[begin].live ? 1 : 0);
  }

  std::optional<GroupChoice> &best() {
    return best_;
  }

 private:
  void Walk(size_t i, size_t live) {
    if (steps_ > 0)
      steps_--;
    if (best_ && (live > best_->live ||
                  (live == best_->live && lowered_.size() >= best_->lowered)))
      return;
    if (members_.size() == width_) {
      Record(i, live);
      return;
    }
    if (i == stack_.size())
      return;

    const GroupMember &layer = stack_[i];
    const bool foreign = layer.turn != turn_;
    if (!foreign) {
      members_.push_back(i);
      Walk(i + 1, live + (layer.live ? 1 : 0));
      members_.pop_back();
    }
    if ((layer.live || foreign) && steps_ > 0) {
      const size_t above = stack_.size() - i - 1;
      const bool room = above >= width_ - members_.size();
      const bool clear =
          std::none_of(members_.begin(), members_.end(), [&](size_t m) {
            return Overlaps(stack_[m], layer);
          });
      if (room && clear) {
        lowered_.push_back(i);
        Walk(i + 1, live);
        lowered_.pop_back();
      }
    }
  }

  void Record(size_t next, size_t live) {
    GroupChoice choice;
    choice.width = width_;
    choice.live = live;
    choice.lowered = lowered_.size();
    choice.order.reserve(stack_.size());
    for (size_t k = 0; k < begin_; k++)
      choice.order.push_back(k);
    choice.order.insert(choice.order.end(), lowered_.begin(), lowered_.end());
    choice.begin = choice.order.size();
    choice.order.insert(choice.order.end(), members_.begin(), members_.end());
    for (size_t k = next; k < stack_.size(); k++)
      choice.order.push_back(k);
    best_ = std::move(choice);
  }

  /* A dozen layers with half of them drawing take a few hundred steps;
   * the budget is there for the scene nobody has seen yet. */
  static constexpr size_t kSteps = 4096;

  const std::vector<GroupMember> &stack_;
  const size_t width_;
  size_t steps_ = kSteps;
  size_t begin_ = 0;
  uint32_t turn_ = 0;
  std::vector<size_t> members_;
  std::vector<size_t> lowered_;
  std::optional<GroupChoice> best_;
};

}  // namespace

std::optional<GroupChoice> ChooseMergeGroup(
    const std::vector<GroupMember> &stack, size_t width) {
  if (width == 0 || width > stack.size())
    return std::nullopt;

  /* Fewest live members first, then fewest layers moved; ties go to the
   * lower group, where wallpaper and application -- the natural group --
   * sit, because a later start must do strictly better to replace it. */
  GroupSearch search(stack, width);
  for (size_t begin = 0; begin + width <= stack.size(); begin++)
    search.From(begin);
  return std::move(search.best());
}

}  // namespace android::drm_hwcomposer
