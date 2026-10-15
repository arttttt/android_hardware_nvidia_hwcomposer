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

#include <array>
#include <cstdint>
#include <vector>

#include "compositor/Rect.h"

namespace android::drm_hwcomposer {

/* The last picture an engine drew, kept to be shown again.
 *
 * Whatever draws several layers into a buffer of the composer's own -- the
 * image engine merging a group, the GPU executor drawing the client range
 * -- draws a picture that is a pure function of its members: whose pixels,
 * which frame of them, what part goes where, and how it is blended, turned
 * and shaped. While none of that changes the buffer already written is the
 * frame, and showing it again costs nothing.
 *
 * Compared against the previous frame only, which is what keeps it sound:
 * a client drawing again hands its layer a buffer again, which moves the
 * layer's frame count, so pixels cannot change under a member that compares
 * equal. That holds only over an unbroken run of frames the engine's path
 * judged; a frame it sat out -- drawn some other way, refused, never shown
 * -- is a frame nobody watched, and Forget() is what the path says then.
 *
 * Only the members are kept here. What was drawn into, and its fence, are
 * the engine's own business, kept beside this by whoever drew. */
class PictureMemory {
 public:
  /* One member, as its engine placed it. Whose pixels is the buffer's
   * identity -- nought for a member that has none, which no picture can be
   * recognised by -- or, for a solid colour, nought with the colour set. */
  struct Member {
    uint64_t source = 0;
    uint64_t generation = 0;
    /* Which layer it is, whichever buffer its frame is in: what a frame's
     * damage is counted against. Not part of what a picture is recognised
     * by here -- the buffer and its frame already say who drew it. */
    uint64_t serial = 0;
    bool solid = false;
    std::array<float, 4> color{};

    FRect src{};
    IRect dst{};
    float alpha = 1.F;
    bool premultiplied = true;
    bool opaque = false;
    uint8_t transform = 0;

    /* Rounded corners: crop left, top, right, bottom, radius. */
    std::array<float, 5> corners{};
  };

  /* Why a picture is not the remembered one -- or that it is. */
  enum class Verdict {
    kSame,
    kFirstSight,
    kChangedShape,
    kChangedSize,
    kNameless,
    kChangedIdentity,
    kChangedFrame,
    kChangedGeometry,
    kChangedBlend,
    kChangedTransform,
    kChangedEffects,
  };

  /* Is this picture the remembered one? `width` by `height` is its size,
   * part of the picture. Where it is shown is not: one memory serves one
   * engine's output, and a picture moved to another window is the same
   * picture. */
  Verdict Judge(uint32_t width, uint32_t height,
                const std::vector<Member> &members) const;

  /* Is it the remembered picture but for newer frames of its members:
   * the same layers -- by serial, whichever of their buffers a frame came
   * in -- in the same places, blended and turned the same, only some of
   * them drawn again since? */
  bool SameButFrames(uint32_t width, uint32_t height,
                     const std::vector<Member> &members) const {
    return Compare(width, height, members, true) == Verdict::kSame;
  }

  void Remember(uint32_t width, uint32_t height,
                std::vector<Member> members);

  /* A frame went unjudged: nothing remembered can be trusted after it. */
  void Forget() {
    remembered_ = false;
  }

  bool Remembers() const {
    return remembered_;
  }

 private:
  Verdict Compare(uint32_t width, uint32_t height,
                  const std::vector<Member> &members, bool frames_aside) const;

  bool remembered_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<Member> members_;
};

}  // namespace android::drm_hwcomposer
