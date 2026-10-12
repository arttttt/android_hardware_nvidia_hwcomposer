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

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "compositor/Rect.h"

namespace android::drm_hwcomposer::partial {

/* Redrawing only what changed, in a buffer drawn before.
 *
 * The buffers an engine draws into are taken in turn, so the one handed out
 * holds whatever was drawn into it some frames ago. Each remembers what that
 * was -- its snapshot -- and a new picture over the same members, in the
 * same places, differs from it only where the members' own pixels changed
 * since: the damage their clients sent, frame by frame of their buffers.
 * Only that is drawn again; the engine leaves the rest of the buffer as it
 * was. Anything it cannot account for -- a member moved, resized, turned,
 * scaled, a frame of a buffer whose damage was never seen -- and the buffer
 * is drawn whole. */

/* One member of a picture, keyed by its layer rather than its buffer: a
 * layer's frames go through several buffers, and the damage is between
 * one frame and the next whichever buffer each is in. */
struct Member {
  uint64_t serial = 0;
  uint64_t generation = 0;
  FRect src{};
  IRect dst{};
  float alpha = 1.F;
  bool premultiplied = true;
  uint8_t transform = 0;
};

/* What each layer's clients said changed, frame by frame of its buffers: in
 * the buffer's own pixels, no rectangles for "all of it", one empty
 * rectangle for "nothing". */
class DamageHistory {
 public:
  void Record(uint64_t serial, uint64_t generation,
              const std::vector<IRect> &damage);

  /* What changed in the layer's buffer after frame `from` up to and
   * including frame `to`: false when a frame between is not known, true
   * with `whole` set when one of them changed all of it. */
  bool Between(uint64_t serial, uint64_t from, uint64_t to,
               std::vector<IRect> *rects, bool *whole) const;

  /* How many frames are kept for a layer, and layers at most. */
  static constexpr size_t kFrames = 8;
  static constexpr size_t kLayers = 64;

 private:
  struct Frame {
    uint64_t generation;
    std::vector<IRect> damage;
  };
  struct Layer {
    std::deque<Frame> frames;
    uint64_t recorded_at = 0;
  };
  std::map<uint64_t, Layer> layers_;
  uint64_t clock_ = 0;
};

/* What a buffer holds: the members it was last drawn with, and how many
 * times in a row it has been drawn only in part. */
struct Snapshot {
  bool valid = false;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Member> members;
  uint32_t partial_run = 0;
};

struct Plan {
  enum class Kind {
    /* Draw the buffer whole. */
    kFull,
    /* Draw `dirty` only; the rest of the buffer stays. */
    kPartial,
    /* The buffer already holds this picture: nothing to draw. */
    kUnchanged,
  };
  Kind kind = Kind::kFull;
  IRect dirty{};
};

/* After this many partial redraws in a row a buffer is drawn whole, which
 * bounds the life of anything a wrong damage left behind; and a region past
 * this share of the buffer is drawn whole, as not worth the bookkeeping. */
constexpr uint32_t kMostPartialInARow = 30;
constexpr float kMostPartialShare = 0.5F;

/* What to draw into a buffer whose snapshot is `held`, for a picture of
 * `width` by `height` made of `members`. */
Plan Decide(const Snapshot &held, uint32_t width, uint32_t height,
            const std::vector<Member> &members, const DamageHistory &history);

/* A member cut down to the part of it that lands inside `bound`: the place
 * trimmed, and the part of the buffer read trimmed in proportion. False
 * when nothing of it lands there. */
bool ClipTo(const IRect &bound, FRect *src, IRect *dst);

}  // namespace android::drm_hwcomposer::partial
