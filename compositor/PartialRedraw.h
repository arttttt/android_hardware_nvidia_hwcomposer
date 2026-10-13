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

#include "compositor/PictureMemory.h"
#include "compositor/Rect.h"
#include "utils/fd.h"

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
 * recoloured, a frame of a buffer whose damage was never seen -- and the
 * buffer is drawn in full: every member, and every place a member held in
 * it before. What lies outside those was never drawn into, or was cleared
 * when its member left, so it is left alone -- a group of strips at the
 * edges of the screen is not a screenful of empty pixels written again.
 * Only a buffer that holds nothing known is written whole.
 *
 * A member is the one the memory of drawn pictures keeps, its layer's
 * serial included: a layer's frames go through several buffers, and the
 * damage is between one frame and the next whichever buffer each is in. */
using Member = PictureMemory::Member;

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
    /* Draw everything the buffer holds: the members where they are now and
     * wherever members stood before, so a place left empty is cleared. */
    kFull,
    /* Draw where the members changed; the rest of the buffer stays. */
    kPartial,
    /* The buffer already holds this picture: nothing to draw. */
    kUnchanged,
  };
  Kind kind = Kind::kFull;
  /* The parts of the buffer to draw. Each is written whole, covered or
   * not, and nothing outside them is touched. None, for a full drawing,
   * means the whole buffer: nothing is known of what it holds. */
  std::vector<IRect> parts;
};

/* The box around some parts, and the pixels they come to. */
IRect Bounds(const std::vector<IRect> &parts);
uint64_t Area(const std::vector<IRect> &parts);

/* After this many partial redraws in a row a buffer is drawn in full,
 * which bounds the life of anything a wrong damage left behind. */
constexpr uint32_t kMostPartialInARow = 30;

/* What one more part costs an engine that draws a part at a time, as the
 * pixels it would draw in that time: two parts are drawn as one when the box
 * around both draws no more than the two of them and that. Parts that
 * overlap are always one; and parts are joined, least growth first, down to
 * the most one picture is drawn in. */
constexpr uint64_t kPartPixels = 65536;
constexpr size_t kMostParts = 4;

/* What an engine can be trusted to draw in part. One that clips by itself
 * draws a scaled member exactly, cut or not -- the GPU, by its scissor; one
 * whose members have to be cut before it sees them would read a scaled one
 * from a shifted phase, and seams would show. One that draws a picture in
 * one go takes its parts as the box around them; one that draws a part at
 * a time takes them as they are, and if it needs a member to draw a part
 * at all, a part that only clears is joined to one that draws. */
struct Engine {
  bool draws_scaled_in_part = false;
  bool draws_many_parts = false;
  bool draws_empty_part = false;
};

/* What to draw into a buffer whose snapshot is `held`, for a picture of
 * `width` by `height` made of `members`. */
Plan Decide(const Snapshot &held, uint32_t width, uint32_t height,
            const std::vector<Member> &members, const DamageHistory &history,
            Engine engine);

/* A member cut down to the part of it that lands inside `bound`: the place
 * trimmed, and the part of the buffer read trimmed in proportion. False
 * when nothing of it lands there. */
bool ClipTo(const IRect &bound, FRect *src, IRect *dst);

/* The buffers one engine draws into, each with what it holds and the fence
 * its last drawing came due on, and the damage of the members drawn. Keyed
 * by whatever names a buffer to its owner, which must outlive this. */
class SlotPictures {
 public:
  explicit SlotPictures(Engine engine) : engine_(engine) {}

  /* A member's frame and its damage, to be found by a buffer that comes
   * round later. Every member of every picture drawn, before deciding. */
  void Record(const Member &m, const std::vector<IRect> &damage) {
    history_.Record(m.serial, m.generation, damage);
  }

  Plan Decide(const void *slot, uint32_t width, uint32_t height,
              const std::vector<Member> &members) const;

  /* `slot` now holds this picture, drawn by `plan`, done at `fence`. */
  void Drawn(const void *slot, uint32_t width, uint32_t height,
             std::vector<Member> members, const Plan &plan,
             SharedFd fence);

  /* What `slot` holds is no longer known: its drawing was refused, or
   * dropped, or replaced by something this does not see. */
  void Lost(const void *slot);

  /* The fence the slot's last drawing came due on: what a buffer shown
   * undrawn is shown against. */
  SharedFd Fence(const void *slot) const;

 private:
  struct Held {
    Snapshot snapshot;
    SharedFd fence;
  };
  Engine engine_;
  std::map<const void *, Held> slots_;
  DamageHistory history_;
};

}  // namespace android::drm_hwcomposer::partial
