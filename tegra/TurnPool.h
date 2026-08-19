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

#ifndef TEGRA_TURN_POOL_H
#define TEGRA_TURN_POOL_H

#include <cstdint>
#include <vector>

#include <cutils/native_handle.h>

namespace android {
namespace hwc {

/* Somewhere for a turned copy of a layer to land, before the group reads it.
 *
 * Not the show pool's problem, though it looks like one. The show pool's
 * buffers face the panel: they are handed to a window, scanned for a frame or
 * a thousand, and freed by the fence of the frame that replaces them. A
 * turned intermediate faces nothing -- it is written by one engine pass and
 * read by the group's pass in the same frame, and the engine's channel
 * serialises the two by the driver's own construction: the channel is
 * opened serialized, every submission waits on the one before it, and a
 * session holds exactly one channel -- the same premise the vendor's blit
 * library rested on in production. It needs no fence, no notion of being
 * shown, and no fixed shape; giving it the show pool's machinery would mean
 * teaching that machinery to tell shown buffers from never-shown ones.
 *
 * What it does need is thrift, because these buffers are dear. They live in
 * the same reserved pool as every other surface the display touches, and a
 * panel-sized slot is twelve megabytes -- while the turned copy of a volume
 * popup is a third of one. So nothing is allocated until a turned layer
 * actually arrives; each slot is cut to the size that was asked, rounded up
 * a little so near misses reuse it; and when no layer has turned for a few
 * hundred frames the slots are given back. Scenes that turn are sessions --
 * a video watched, a camera opened -- and between sessions the memory
 * belongs to everyone else.
 */
class TurnPool {
 public:
  /* `cap` slots at most, each no larger than `max_width` by `max_height`
   * (asking past that is refused, not clamped). */
  TurnPool(uint32_t max_width, uint32_t max_height, size_t cap);
  ~TurnPool();

  TurnPool(const TurnPool &) = delete;
  TurnPool &operator=(const TurnPool &) = delete;

  /* A buffer at least `width` by `height`, for this frame only.
   *
   * Reuses an idle slot that fits, or allocates one cut to the asked size
   * (rounded up so the next popup-sized turn lands in the same slot).
   * Returns null when the cap is spent or the allocator refuses -- the
   * caller's turn fails the way every engine refusal fails.
   *
   * No fence comes back on purpose: the only reader is the group pass of
   * the same frame, ordered behind the write by the engine's own channel. */
  buffer_handle_t Take(uint32_t width, uint32_t height);

  /* The frame is over: what was taken may be taken again next frame, and
   * `turned_any` says whether the frame turned anything at all. A few
   * hundred frames of "no" in a row and the slots are freed -- the first
   * turn after that pays the allocation again, which measured too small
   * to reach even the longest-tail percentile. */
  void FrameEnd(bool turned_any);

  /* For the dump: what the pool is holding right now. */
  size_t held_slots() const { return slots_.size(); }
  size_t held_bytes() const;

  /* How many times the idle trim gave everything back -- the pool's own
   * heartbeat for the dump: a count that grows says rotation comes in
   * episodes, one that never moves says the pool is either always busy
   * or never used. */
  uint64_t trims() const { return trims_; }
  size_t cap() const { return cap_; }

 private:
  struct Slot {
    buffer_handle_t handle = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes = 0;
    bool taken = false;
  };

  uint64_t trims_ = 0;
  uint32_t max_width_ = 0;
  uint32_t max_height_ = 0;
  size_t cap_ = 0;
  uint32_t idle_frames_ = 0;
  std::vector<Slot> slots_;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_TURN_POOL_H
