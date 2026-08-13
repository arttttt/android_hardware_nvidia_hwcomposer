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
#include <vector>

#include <cutils/native_handle.h>

#include "utils/fd.h"

namespace android {
namespace hwc {

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
   * the allocator would not give them. */
  static std::unique_ptr<ScratchPool> Create(uint32_t width, uint32_t height,
                                             size_t count);

  ~ScratchPool();

  ScratchPool(const ScratchPool &) = delete;
  ScratchPool &operator=(const ScratchPool &) = delete;

  /* A buffer nothing is reading, to draw the next frame into. Null when there
   * is no such buffer, which the caller answers by dropping the frame.
   *
   * Asks rather than waits -- see the note in the source on why a frame is
   * never made to wait here. */
  buffer_handle_t Next();

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

 private:
  ScratchPool() = default;

  struct Slot {
    buffer_handle_t handle = nullptr;

    /* On screen, or on its way there, and not yet replaced by anything. No
     * fence can say when such a buffer comes free, because the frame that
     * will free it has not been posted yet. */
    bool showing = false;

    /* Once it has been replaced: when the frame that replaced it appears,
     * which is the moment the display stops reading this one. */
    drm_hwcomposer::SharedFd freed_when;
  };

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t stride_ = 0;

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
