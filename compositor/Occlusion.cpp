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

#include "compositor/Occlusion.h"

#include <algorithm>
#include <cmath>

namespace android::drm_hwcomposer::occlusion {

namespace {

bool Empty(const IRect &r) {
  return r.right <= r.left || r.bottom <= r.top;
}

IRect Intersect(const IRect &a, const IRect &b) {
  return {std::max(a.left, b.left), std::max(a.top, b.top),
          std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

/* `region` without `cut`: each piece loses what `cut` covers of it, which
 * leaves up to four pieces of it -- the band above, the band below, and
 * what is left and right of the cut between them. */
void Subtract(std::vector<IRect> *region, const IRect &cut) {
  std::vector<IRect> out;
  out.reserve(region->size() + 4);
  for (const IRect &r : *region) {
    const IRect overlap = Intersect(r, cut);
    if (Empty(overlap)) {
      out.push_back(r);
      continue;
    }
    if (r.top < overlap.top)
      out.push_back({r.left, r.top, r.right, overlap.top});
    if (overlap.bottom < r.bottom)
      out.push_back({r.left, overlap.bottom, r.right, r.bottom});
    if (r.left < overlap.left)
      out.push_back({r.left, overlap.top, overlap.left, overlap.bottom});
    if (overlap.right < r.right)
      out.push_back({overlap.right, overlap.top, r.right, overlap.bottom});
  }
  region->swap(out);
}

/* What of an opaque layer is opaque: its frame, or, with rounded corners,
 * the crop they round less the square each corner takes -- a cross, as two
 * rectangles. The square is the radius whole pixels on a side; inside it
 * the corner may let something through. */
std::vector<IRect> OpaqueParts(const Layer &l) {
  if (!l.opaque)
    return {};
  if (l.radius <= 0.F)
    return {l.frame};
  const IRect crop = Intersect(l.frame, l.corner_crop);
  const auto r = static_cast<int32_t>(std::ceil(l.radius));
  std::vector<IRect> parts;
  const IRect tall{crop.left + r, crop.top, crop.right - r, crop.bottom};
  const IRect wide{crop.left, crop.top + r, crop.right, crop.bottom - r};
  if (!Empty(tall))
    parts.push_back(tall);
  if (!Empty(wide))
    parts.push_back(wide);
  return parts;
}

}  // namespace

std::vector<Shown> Resolve(const std::vector<Layer> &bottom_first) {
  std::vector<Shown> shown(bottom_first.size());

  /* What the layers already passed hide, walking down from the top. */
  std::vector<IRect> hiding;

  for (size_t i = bottom_first.size(); i-- > 0;) {
    const Layer &l = bottom_first[i];
    Shown &s = shown[i];
    s.bounds = l.frame;

    if (!l.keep && !Empty(l.frame)) {
      std::vector<IRect> region;
      if (l.visible.empty()) {
        region.push_back(l.frame);
      } else {
        for (const IRect &v : l.visible) {
          const IRect in = Intersect(v, l.frame);
          if (!Empty(in))
            region.push_back(in);
        }
      }
      for (const IRect &h : hiding) {
        if (region.empty())
          break;
        Subtract(&region, h);
      }

      if (region.empty()) {
        s.hidden = true;
      } else {
        IRect b = region.front();
        for (const IRect &r : region) {
          b.left = std::min(b.left, r.left);
          b.top = std::min(b.top, r.top);
          b.right = std::max(b.right, r.right);
          b.bottom = std::max(b.bottom, r.bottom);
        }
        s.bounds = b;
      }
    }

    for (const IRect &p : OpaqueParts(l))
      hiding.push_back(p);
  }
  return shown;
}

std::optional<FRect> CropSource(const FRect &source, const IRect &frame,
                                const IRect &shown, bool hflip, bool vflip,
                                bool rotate90) {
  if (rotate90 || Empty(frame) || shown.left < frame.left ||
      shown.top < frame.top || shown.right > frame.right ||
      shown.bottom > frame.bottom)
    return std::nullopt;

  const float sx = source.Width() / static_cast<float>(frame.Width());
  const float sy = source.Height() / static_cast<float>(frame.Height());

  /* How much of the frame is cut off on each edge; a flip reads the
   * source's edge from the frame's opposite one. */
  const auto cut_left = static_cast<float>(shown.left - frame.left);
  const auto cut_right = static_cast<float>(frame.right - shown.right);
  const auto cut_top = static_cast<float>(shown.top - frame.top);
  const auto cut_bottom = static_cast<float>(frame.bottom - shown.bottom);

  FRect out = source;
  out.left += (hflip ? cut_right : cut_left) * sx;
  out.right -= (hflip ? cut_left : cut_right) * sx;
  out.top += (vflip ? cut_bottom : cut_top) * sy;
  out.bottom -= (vflip ? cut_top : cut_bottom) * sy;
  return out;
}

}  // namespace android::drm_hwcomposer::occlusion
