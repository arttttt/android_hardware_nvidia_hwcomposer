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

#include "compositor/PartialRedraw.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace android::drm_hwcomposer::partial {

namespace {

bool Empty(const IRect &r) {
  return r.right <= r.left || r.bottom <= r.top;
}

/* The engine's edges, for a region it is to write: columns in sixteens and
 * rows in pairs, which nothing in the documentation asks for and nothing
 * then can be caught out by. */
constexpr int32_t kColumnGrain = 16;
constexpr int32_t kRowGrain = 2;

int32_t Down(int32_t v, int32_t grain) {
  return v / grain * grain;
}
int32_t Up(int32_t v, int32_t grain) {
  return (v + grain - 1) / grain * grain;
}

/* Everything about a member but which frame of it: equal, and the only
 * difference between the two pictures is in its pixels. */
bool SameButFrame(const Member &a, const Member &b) {
  return a.serial == b.serial && a.solid == b.solid && a.color == b.color &&
         a.src == b.src && a.dst == b.dst && a.alpha == b.alpha &&
         a.premultiplied == b.premultiplied && a.opaque == b.opaque &&
         a.transform == b.transform && a.corners == b.corners;
}

bool Scaled(const Member &m) {
  return std::fabs(m.src.Width() - static_cast<float>(m.dst.Width())) >=
             0.01F ||
         std::fabs(m.src.Height() - static_cast<float>(m.dst.Height())) >=
             0.01F;
}

/* A rectangle of the member's buffer, as the part of the picture it lands
 * on. Scaled, the filter reads a pixel's neighbours too, so the box grows by
 * one on every side; and nothing outside where the member lands. */
IRect Onto(const IRect &r, const Member &m, bool scaled) {
  const float sx = static_cast<float>(m.dst.Width()) / m.src.Width();
  const float sy = static_cast<float>(m.dst.Height()) / m.src.Height();
  const int32_t pad = scaled ? 1 : 0;
  const auto x = [&](int32_t v, bool up) {
    const float f = static_cast<float>(m.dst.left) +
                    (static_cast<float>(v) - m.src.left) * sx;
    return static_cast<int32_t>(up ? std::ceil(f - 0.001F)
                                   : std::floor(f + 0.001F));
  };
  const auto y = [&](int32_t v, bool up) {
    const float f = static_cast<float>(m.dst.top) +
                    (static_cast<float>(v) - m.src.top) * sy;
    return static_cast<int32_t>(up ? std::ceil(f - 0.001F)
                                   : std::floor(f + 0.001F));
  };
  return {std::max(x(r.left, false) - pad, m.dst.left),
          std::max(y(r.top, false) - pad, m.dst.top),
          std::min(x(r.right, true) + pad, m.dst.right),
          std::min(y(r.bottom, true) + pad, m.dst.bottom)};
}

}  // namespace

void DamageHistory::Record(uint64_t serial, uint64_t generation,
                           const std::vector<IRect> &damage) {
  ++clock_;
  Layer &l = layers_[serial];
  l.recorded_at = clock_;
  if (!l.frames.empty() && l.frames.back().generation >= generation)
    return;
  l.frames.push_back({generation, damage});
  while (l.frames.size() > kFrames)
    l.frames.pop_front();

  /* The layer heard from longest ago goes when there are too many. */
  if (layers_.size() > kLayers) {
    auto oldest = layers_.begin();
    for (auto it = layers_.begin(); it != layers_.end(); ++it)
      if (it->second.recorded_at < oldest->second.recorded_at)
        oldest = it;
    layers_.erase(oldest);
  }
}

bool DamageHistory::Between(uint64_t serial, uint64_t from, uint64_t to,
                            std::vector<IRect> *rects, bool *whole) const {
  *whole = false;
  const auto it = layers_.find(serial);
  if (it == layers_.end())
    return false;
  for (uint64_t g = from + 1; g <= to; ++g) {
    const auto f = std::find_if(
        it->second.frames.begin(), it->second.frames.end(),
        [g](const Frame &frame) { return frame.generation == g; });
    if (f == it->second.frames.end())
      return false;
    if (f->damage.empty()) {
      *whole = true;
      return true;
    }
    for (const IRect &r : f->damage)
      if (!Empty(r))
        rects->push_back(r);
  }
  return true;
}

Plan Decide(const Snapshot &held, uint32_t width, uint32_t height,
            const std::vector<Member> &members, const DamageHistory &history,
            Engine engine) {
  Plan full;
  if (!held.valid || held.width != width || held.height != height ||
      held.members.size() != members.size() ||
      held.partial_run >= kMostPartialInARow)
    return full;

  const IRect bounds{0, 0, static_cast<int32_t>(width),
                     static_cast<int32_t>(height)};
  bool any = false;
  IRect dirty{};

  for (size_t i = 0; i < members.size(); ++i) {
    const Member &now = members[i];
    const Member &then = held.members[i];
    if (now.serial == 0 || !SameButFrame(now, then) ||
        now.generation < then.generation)
      return full;
    /* An engine that has its members cut for it reads a scaled one from a
     * shifted phase wherever the cut falls -- changed or not, any of them
     * the region crosses. */
    const bool scaled = Scaled(now);
    if (scaled && !engine.draws_scaled_in_part)
      return full;
    if (now.generation == then.generation)
      continue;

    /* Damage is in the buffer's own axes; a turned member would need it
     * turned too. */
    if (now.transform != 0 || now.src.Width() <= 0 || now.src.Height() <= 0)
      return full;

    std::vector<IRect> rects;
    bool whole = false;
    if (!history.Between(now.serial, then.generation, now.generation, &rects,
                         &whole))
      return full;

    std::vector<IRect> on;
    if (whole)
      on.push_back(now.dst);
    for (const IRect &r : rects)
      on.push_back(Onto(r, now, scaled));
    for (const IRect &r : on) {
      if (Empty(r))
        continue;
      dirty = any ? IRect{std::min(dirty.left, r.left),
                          std::min(dirty.top, r.top),
                          std::max(dirty.right, r.right),
                          std::max(dirty.bottom, r.bottom)}
                  : r;
      any = true;
    }
  }

  if (!any)
    return Plan{Plan::Kind::kUnchanged, {}};

  dirty = {std::max(Down(dirty.left, kColumnGrain), bounds.left),
           std::max(Down(dirty.top, kRowGrain), bounds.top),
           std::min(Up(dirty.right, kColumnGrain), bounds.right),
           std::min(Up(dirty.bottom, kRowGrain), bounds.bottom)};
  if (Empty(dirty))
    return Plan{Plan::Kind::kUnchanged, {}};
  const float share =
      static_cast<float>(dirty.Width()) * static_cast<float>(dirty.Height()) /
      (static_cast<float>(width) * static_cast<float>(height));
  if (share > kMostPartialShare)
    return full;
  return Plan{Plan::Kind::kPartial, dirty};
}

bool ClipTo(const IRect &bound, FRect *src, IRect *dst) {
  const IRect in{std::max(dst->left, bound.left), std::max(dst->top, bound.top),
                 std::min(dst->right, bound.right),
                 std::min(dst->bottom, bound.bottom)};
  if (Empty(in))
    return false;
  const float sx = src->Width() / static_cast<float>(dst->Width());
  const float sy = src->Height() / static_cast<float>(dst->Height());
  *src = {src->left + static_cast<float>(in.left - dst->left) * sx,
          src->top + static_cast<float>(in.top - dst->top) * sy,
          src->right - static_cast<float>(dst->right - in.right) * sx,
          src->bottom - static_cast<float>(dst->bottom - in.bottom) * sy};
  *dst = in;
  return true;
}

Plan SlotPictures::Decide(const void *slot, uint32_t width, uint32_t height,
                          const std::vector<Member> &members) const {
  const auto it = slots_.find(slot);
  if (it == slots_.end())
    return {};
  Plan plan = partial::Decide(it->second.snapshot, width, height, members,
                              history_, engine_);
  /* Shown undrawn means shown against the fence of its last drawing; with
   * none to name, it is drawn. */
  if (plan.kind == Plan::Kind::kUnchanged && !it->second.fence)
    plan.kind = Plan::Kind::kFull;
  return plan;
}

void SlotPictures::Drawn(const void *slot, uint32_t width, uint32_t height,
                         std::vector<Member> members, const Plan &plan,
                         SharedFd fence) {
  Held &h = slots_[slot];
  const uint32_t run = h.snapshot.partial_run;
  h.snapshot.valid = true;
  h.snapshot.width = width;
  h.snapshot.height = height;
  h.snapshot.members = std::move(members);
  h.snapshot.partial_run = plan.kind == Plan::Kind::kPartial     ? run + 1
                           : plan.kind == Plan::Kind::kUnchanged ? run
                                                                 : 0;
  h.fence = std::move(fence);
}

void SlotPictures::Lost(const void *slot) {
  const auto it = slots_.find(slot);
  if (it != slots_.end())
    it->second.snapshot.valid = false;
}

SharedFd SlotPictures::Fence(const void *slot) const {
  const auto it = slots_.find(slot);
  return it == slots_.end() ? SharedFd{} : it->second.fence;
}

}  // namespace android::drm_hwcomposer::partial
