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

#ifndef TEGRA_SCRATCH_POOL_H
#define TEGRA_SCRATCH_POOL_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <cutils/native_handle.h>

#include "tegra/ScratchBuffer.h"
#include "utils/fd.h"

namespace android {
namespace hwc {

class VicSession;

/* Somewhere for a merged frame to land.
 *
 * What the engine writes has to live somewhere that is neither a layer's
 * buffer nor the one SurfaceFlinger composites into, because both of those
 * belong to someone else. So the composer keeps a few of its own, the size of
 * the panel, and takes them in turn.
 *
 * More than one because a buffer is still being read by the display long
 * after the frame that used it was handed over: writing the next merge into
 * the buffer on screen would tear it. Taking them in turn, and refusing to
 * hand one back until the frame that showed it has been presented, is what
 * keeps that from happening.
 *
 * Pitch-linear on purpose, which is not the arrangement anything else on this
 * device uses. The narrow fourth window of the controller reads pitch and
 * nothing else, cannot resize, and takes only simple colour -- and a
 * full-screen merge at its own size is the one thing on this device that
 * satisfies all three. So the buffer that would otherwise have to displace a
 * layer from a real window can go in the window nobody could use. The
 * allocator has no switch for this; what it has is a rule that a buffer the
 * processor will read often is laid out in rows, so that is what is asked
 * for, and it is honest -- checking the merge means reading it.
 */
class ScratchPool {
 public:
  /* `count` buffers of `width` by `height`. Null, having said what failed, if
   * the allocator would not give them.
   *
   * The buffers come from the composer's own carveout zone when there is
   * one; the whole pool falls back to gralloc if any of them is refused,
   * so a pool is one origin or the other, never a mix -- one answer in the
   * dump instead of a dozen. */
  static std::unique_ptr<ScratchPool> Create(uint32_t width, uint32_t height,
                                             size_t count,
                                             VicSession *vic = nullptr);

  ~ScratchPool();

  ScratchPool(const ScratchPool &) = delete;
  ScratchPool &operator=(const ScratchPool &) = delete;

  /* The next buffer to draw into, and the fence saying when it may be drawn
   * into -- which is not the same as saying it is ready now.
   *
   * Every buffer stays in the rotation, including the one on screen. What
   * keeps that safe is the fence handed out beside it: it comes due when the
   * frame that replaced this buffer appears, and whoever draws is expected to
   * wait for it rather than for this call to return.
   *
   * The alternative -- holding a buffer back until its fence is due -- means
   * a pool of three lends out two, and the frame that finds nothing free is
   * simply lost. Which is the whole difference between this and the vendor's
   * own, whose scratch set hands its buffer over with exactly this fence as
   * the acquire fence of the composition.
   *
   * `ready` is left empty when the buffer has never been shown. Null return
   * only if the pool holds nothing at all. The pointer is into the pool's
   * own slot, which never moves: the pool is sized once, and no slot is
   * ever added or taken away. */
  ScratchBuffer *Next(drm_hwcomposer::SharedFd *ready);

  /* Takes back the last Next(): the same buffer will be handed out again.
   *
   * For the frame that was drawn but never shown -- the engine wrote the
   * buffer, and then the flip failed or the frame was abandoned before it.
   * The panel never saw it, so it is the right buffer to draw the next
   * attempt into; advancing past it instead walks the rotation onto the
   * buffer the panel is scanning right now, with nothing due on its fence
   * to say so. Writes to the same buffer need no fence between them: the
   * engine's channel serialises its own jobs. */
  void Rewind();

  /* Says that a frame has been posted, against the fence it will appear on.
   *
   * Which is not the same as saying that the buffer this frame was drawn into
   * is free at that fence -- it is the opposite. A frame's fence comes due
   * when the display starts showing it, and from then until the frame after
   * it appears, that buffer is being read out of memory continuously. What
   * this fence frees is the buffer of the frame *before*, which this one has
   * just replaced. */
  void Presented(const drm_hwcomposer::SharedFd &fence);

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t stride() const { return stride_; }

  /* Reads the pixels of every slot, for the dump: each slot's base
   * address and its first row's leading words and centre pixel. Whether
   * the engine writes a picture or noise into a slot is the one split
   * that says where a corruption lives -- in the writing or in the
   * reading -- and all slots are shown because the rotation can serve
   * any one of them. */
  std::string DumpSlotsContent() const;

  /* Where the pool's buffers were born -- the dump's one-line answer
   * for whether the zone is doing its job on this device. */
  ScratchBuffer::Origin origin() const { return origin_; }

 private:
  ScratchPool() = default;

  void FreeSlots();

  struct Slot {
    ScratchBuffer buffer;

    /* When the frame that replaced this buffer appears, which is the moment
     * the display stops reading this one. Empty while it is still the newest
     * thing posted, and while it has never been shown at all -- in both cases
     * there is no such frame yet to name. */
    drm_hwcomposer::SharedFd freed_when;
  };

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t stride_ = 0;
  ScratchBuffer::Origin origin_ = ScratchBuffer::Origin::kGralloc;

  /* The slot the last frame was drawn into, which the next frame posted will
   * replace on screen. Past the end until a frame has been posted. */
  size_t showing_ = 0;
  bool anything_showing_ = false;

  size_t at_ = 0;
  std::vector<Slot> slots_;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_SCRATCH_POOL_H
