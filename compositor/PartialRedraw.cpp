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

bool SameGeometry(const Member &a, const Member &b) {
  return a.serial == b.serial && a.src == b.src && a.dst == b.dst &&
         a.alpha == b.alpha && a.premultiplied == b.premultiplied &&
         a.transform == b.transform;
}

/* Drawn at its own size, unturned: a buffer pixel is a picture pixel, so
 * the damage maps across by an offset alone. */
bool OneToOne(const Member &m) {
  return m.transform == 0 &&
         std::fabs(m.src.Width() - static_cast<float>(m.dst.Width())) < 0.01F &&
         std::fabs(m.src.Height() - static_cast<float>(m.dst.Height())) < 0.01F;
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
            const std::vector<Member> &members, const DamageHistory &history) {
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
    if (!SameGeometry(now, then) || !OneToOne(now) ||
        now.generation < then.generation)
      return full;
    if (now.generation == then.generation)
      continue;

    std::vector<IRect> rects;
    bool whole = false;
    if (!history.Between(now.serial, then.generation, now.generation, &rects,
                         &whole))
      return full;

    /* From the buffer's pixels to the picture's, and only inside where the
     * member lands. */
    const int32_t dx = now.dst.left - static_cast<int32_t>(now.src.left);
    const int32_t dy = now.dst.top - static_cast<int32_t>(now.src.top);
    if (whole)
      rects = {{now.dst.left - dx, now.dst.top - dy, now.dst.right - dx,
                now.dst.bottom - dy}};
    for (const IRect &r : rects) {
      IRect on{std::max(r.left + dx, now.dst.left),
               std::max(r.top + dy, now.dst.top),
               std::min(r.right + dx, now.dst.right),
               std::min(r.bottom + dy, now.dst.bottom)};
      if (Empty(on))
        continue;
      dirty = any ? IRect{std::min(dirty.left, on.left),
                          std::min(dirty.top, on.top),
                          std::max(dirty.right, on.right),
                          std::max(dirty.bottom, on.bottom)}
                  : on;
      any = true;
    }
  }

  if (!any)
    return Plan{Plan::Kind::kUnchanged, {}};

  dirty = {std::max(Down(dirty.left, kColumnGrain), bounds.left),
           std::max(Down(dirty.top, kRowGrain), bounds.top),
           std::min(Up(dirty.right, kColumnGrain), bounds.right),
           std::min(Up(dirty.bottom, kRowGrain), bounds.bottom)};
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

}  // namespace android::drm_hwcomposer::partial
