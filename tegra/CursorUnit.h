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

#ifndef TEGRA_CURSOR_UNIT_H
#define TEGRA_CURSOR_UNIT_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>

#include <cutils/native_handle.h>

namespace android {
namespace hwc {

/* The controller's own cursor.
 *
 * A fifth scanning unit beside the four windows, with its own registers,
 * its own sprite memory, and its own activation bit that latches on the
 * vertical blank without a frame being posted. That last part is the whole
 * point: once the sprite is loaded, moving it is one small ioctl, not a
 * composition -- the stock composer moved a mouse at the mouse's own rate
 * while posting nothing, and without this unit every twitch of a pointer
 * is a full frame through the GPU or the engine.
 *
 * The unit draws over every window, unconditionally -- that is hardware,
 * not policy -- so whoever offers it a layer must make sure nothing was
 * meant to be above that layer. And it draws independently of frames,
 * which is its strength and its trap: a sprite nobody hides outlives the
 * scene it belonged to, so hiding is a duty here, not an optimisation.
 *
 * The sprite lives in buffers of this class's own, square and cut to the
 * sizes the hardware names, because the unit's appetite is stricter than
 * any layer's: rows with no padding at all, a base address aligned to a
 * kilobyte -- the kernel oopses on less, it does not refuse -- and colour
 * with alpha in the low byte, which no allocator here produces. So the
 * layer's pixels are copied in by hand on the rare frame the sprite
 * changes, swizzled on the way, and the two slots take turns so a new
 * sprite is never written over one being scanned.
 */
class CursorUnit {
 public:
  /* The unit, claimed from the head that owns it, or null having said why
   * not. The descriptor is borrowed and must outlive this object; the
   * kernel grants the cursor to one descriptor at a time, which is the
   * claim's whole meaning. */
  static std::unique_ptr<CursorUnit> Claim(int dc_fd);

  ~CursorUnit();

  CursorUnit(const CursorUnit &) = delete;
  CursorUnit &operator=(const CursorUnit &) = delete;

  /* The largest sprite the hardware takes, per side. */
  static constexpr uint32_t kMaxSide = 256;

  /* Puts `sprite` on screen at (x, y), loading its pixels only when `id`
   * says they changed. Width and height are the layer's own and
   * `stride_px` is the sprite buffer's row length in pixels; the slot
   * padding past the sprite is transparent. False if the sprite could not
   * be shown -- the caller's frame goes on without the unit, and the
   * layer belongs back in a window next time. */
  bool Show(buffer_handle_t sprite, uint64_t id, uint32_t width,
            uint32_t height, uint32_t stride_px, bool premultiplied,
            int32_t x, int32_t y);

  /* Moves the visible sprite. Safe from any thread and cheap on purpose:
   * this is the call that arrives at the mouse's own rate, between frames,
   * and it must never wait for one. Quietly nothing when nothing is
   * shown. */
  void Move(int32_t x, int32_t y);

  /* Takes the sprite off the panel. A duty, not an optimisation -- see
   * the class note. */
  void Hide();

  /* Re-states the current image and position to hardware that has been
   * blanked and lit again: the extension refuses cursor calls while the
   * head is dark, and coming back light restores none of this on its
   * own. */
  void Rearm();

  /* For the dump: what the unit has been asked to do. */
  void AppendDump(std::ostringstream &ss) const;

 private:
  explicit CursorUnit(int dc_fd) : fd_(dc_fd) {}

  bool UploadLocked(buffer_handle_t sprite, uint32_t width, uint32_t height,
                    uint32_t stride_px, bool premultiplied);
  bool PointLocked(int32_t x, int32_t y, bool visible);
  void ReleaseSlotsLocked();

  const int fd_;

  mutable std::mutex lock_;

  struct Slot {
    buffer_handle_t handle = nullptr;
    int mem_fd = -1;
    uint32_t side = 0;
  };
  Slot slots_[2];
  size_t at_ = 0;

  uint64_t image_id_ = 0;
  uint32_t image_flags_ = 0;
  bool visible_ = false;
  int32_t x_ = 0;
  int32_t y_ = 0;

  uint64_t moves_ = 0;
  uint64_t uploads_ = 0;
  uint64_t refused_ = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_CURSOR_UNIT_H
