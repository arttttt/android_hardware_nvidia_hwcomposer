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

#include "compositor/PictureMemory.h"

#include <utility>

namespace android::drm_hwcomposer {

PictureMemory::Verdict PictureMemory::Judge(
    uint32_t width, uint32_t height,
    const std::vector<Member> &members) const {
  return Compare(width, height, members, false);
}

PictureMemory::Verdict PictureMemory::Compare(
    uint32_t width, uint32_t height,
    const std::vector<Member> &members, bool frames_aside) const {
  if (!remembered_)
    return Verdict::kFirstSight;
  if (members.size() != members_.size())
    return Verdict::kChangedShape;
  if (width != width_ || height != height_)
    return Verdict::kChangedSize;

  for (size_t i = 0; i < members.size(); ++i) {
    const Member &m = members[i];
    const Member &r = members_[i];

    if (frames_aside) {
      /* A newer frame of a layer may come in another of its buffers, so
       * the layer is what is compared, not the buffer. */
      if (m.serial == 0)
        return Verdict::kNameless;
      if (m.solid != r.solid || m.serial != r.serial || m.color != r.color)
        return Verdict::kChangedIdentity;
    } else {
      if (!m.solid && m.source == 0)
        return Verdict::kNameless;
      if (m.solid != r.solid || m.source != r.source || m.color != r.color)
        return Verdict::kChangedIdentity;
      if (m.generation != r.generation)
        return Verdict::kChangedFrame;
    }

    /* Exact comparison on purpose, floats included: both sides came out of
     * the same description untouched, so anything unequal really is a
     * different picture. */
    if (m.src != r.src || m.dst != r.dst)
      return Verdict::kChangedGeometry;
    if (m.alpha != r.alpha || m.premultiplied != r.premultiplied ||
        m.opaque != r.opaque)
      return Verdict::kChangedBlend;
    if (m.transform != r.transform)
      return Verdict::kChangedTransform;
    if (m.corners != r.corners || m.turned != r.turned)
      return Verdict::kChangedEffects;
  }
  return Verdict::kSame;
}

void PictureMemory::Remember(uint32_t width, uint32_t height,
                             std::vector<Member> members) {
  remembered_ = true;
  width_ = width;
  height_ = height;
  members_ = std::move(members);
}

}  // namespace android::drm_hwcomposer
