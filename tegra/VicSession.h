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

#ifndef TEGRA_VIC_SESSION_H
#define TEGRA_VIC_SESSION_H

#include <cstdint>
#include <memory>
#include <vector>

#include <cutils/native_handle.h>

#include "utils/fd.h"

namespace android {
namespace hwc {

/* The image compositor this chip has, and the display controller does not.
 *
 * The controller can show a handful of buffers at once and no more -- three of
 * its windows are usable here, and the buffer anything composited lands in
 * takes one of them, so a frame carries two layers in hardware and everything
 * else has to be merged first. Today that merge is done by SurfaceFlinger on
 * the GPU, which is why a quarter of the pixels of an application opening go
 * through the graphics core.
 *
 * This is the other engine that can do it. VIC is a fixed-function block on
 * the host1x bus, made for exactly this: read several surfaces, scale them,
 * blend them, write one. It is idle on this device -- nothing has ever opened
 * it -- and using it means the graphics core is never woken for a merge.
 *
 * Reached through the libraries the board ships rather than built against
 * them, for the same reason as the allocator in bufferinfo/NvGralloc.h: the
 * vendor blob repository declares them by installation path rather than as
 * modules, so there is nothing for a module here to link to.
 */
class VicSession {
 public:
  /* The compositor, or nullptr if this device has none it can reach, having
   * logged what was missing. */
  static std::unique_ptr<VicSession> Create();

  ~VicSession();

  VicSession(const VicSession &) = delete;
  VicSession &operator=(const VicSession &) = delete;

  /* How many surfaces the engine takes in one pass.
   *
   * Fixed rather than asked, and deliberately. The number the hardware
   * reports lives inside the session structure, which would mean knowing that
   * structure's layout -- the one thing this class is built to avoid. Five is
   * what the interface reserves room for, and a set the engine will not take
   * is refused at configure or execute time anyway, which is the answer we
   * actually act on. */
  static constexpr size_t kMaxLayers = 5;

  /* One thing to draw into the merge. Rectangles are in the coordinates of
   * the buffer being drawn into, which for a merge is the display. */
  struct Layer {
    buffer_handle_t handle;

    float source_left;
    float source_top;
    float source_right;
    float source_bottom;

    int32_t display_left;
    int32_t display_top;
    int32_t display_right;
    int32_t display_bottom;

    /* Whether the colour in the buffer has already been multiplied by its own
     * alpha. The engine is told which, rather than being left to assume, so
     * that a layer arriving either way lands correctly. */
    bool premultiplied;

    /* Applied on top of whatever alpha the pixels carry. One means the layer
     * contributes at its own opacity. */
    float alpha;

    /* Borrowed. The engine is told to wait on it before reading, and does not
     * take ownership -- the caller closes it as it would have anyway. */
    int acquire_fence;
  };

  /* Draws `layers` into `target`, bottom of the list first.
   *
   * `target_ready` says when the target may be written to -- it is the fence
   * of the frame that replaced whatever `target` was last showing, and until
   * it comes due the display is still reading that buffer. Handed to the
   * engine rather than waited for: the engine is perfectly able to wait, it
   * does so without occupying a thread, and it is the only way every buffer
   * can stay in rotation. Waiting on it here instead, or refusing to offer a
   * buffer until it is due, costs exactly the buffers it protects -- which is
   * why the vendor's own composer passes it along the same way, as the
   * acquire fence of its scratch surface.
   *
   * Borrowed like the layers' own: read, not taken.
   *
   * Returns the fence to wait on before the result is read, or an empty one
   * if the engine would not take this set -- which is not a fault and is
   * counted rather than complained about: it is the signal that the merge has
   * to go somewhere else. Nothing has been written to `target` in that case.
   *
   * More than kMaxLayers layers is refused the same way.
   */
  /* `width` and `height` bound the region actually drawn, measured from
   * the buffer's origin; nought means the whole buffer. The caller that
   * shows only a corner of the target has no reason to pay for the rest of
   * it being written. */
  drm_hwcomposer::SharedFd Compose(buffer_handle_t target,
                                   const std::vector<Layer> &layers,
                                   uint32_t width = 0, uint32_t height = 0,
                                   int target_ready = -1);

  /* Draws one layer into `target` turned by `transform` -- the engine's own
   * transform code, which the caller owes to the stock translation table,
   * not to arithmetic of its own. The write is bounded to `width` by
   * `height` from the origin, which the caller sizes to the turned crop.
   *
   * The copy is verbatim: blending is told to ignore alpha, so the pixels
   * arrive in the intermediate exactly as they left the source, and the
   * layer's own alpha and premultiplication are honoured later, by the pass
   * that composes the turned copy. This is how the stock blit copied too.
   *
   * Fences as in Compose: the layer's acquire is handed to the engine, the
   * returned fence says when the turned copy may be read. `target_ready`
   * is usually not needed at all -- the channel serialises our passes, so
   * a pass submitted after the group that read this buffer runs after it. */
  drm_hwcomposer::SharedFd ComposeRotated(buffer_handle_t target,
                                          const Layer &layer,
                                          uint32_t transform,
                                          uint32_t width, uint32_t height,
                                          int target_ready = -1);

  /* How many sets the engine has refused, and how many it has taken. What
   * decides whether a third way of merging is worth building at all. */
  uint64_t composed() const { return composed_; }
  uint64_t refused() const { return refused_; }

 private:
  VicSession() = default;

  bool Open();
  bool Resolve();

  /* Their settings structure, held as a plain run of bytes.
   *
   * Its fields are never touched here. The library has its own calls for
   * filling them -- one for the target, one per source, one for blending --
   * so what this class needs is somewhere to put the answer, not knowledge of
   * where each answer goes. That is not laziness but the only safe reading:
   * the structure belongs to the library and has no promise of stability, and
   * the allocator on this same device has already been caught having grown a
   * field since the last published version of its own (see NvGralloc.h).
   *
   * Sized well above what the published layout adds up to, and zeroed before
   * every use, so that a structure which has since grown is still written
   * inside memory that is ours. */
  static constexpr size_t kConfigBytes = 16384;

  void *nvrm_library_ = nullptr;
  void *graphics_library_ = nullptr;
  void *vic_library_ = nullptr;

  /* Opaque on purpose -- see kConfigBytes. */
  void *rm_device_ = nullptr;
  void *session_ = nullptr;
  std::vector<uint8_t> config_;

  uint64_t composed_ = 0;
  uint64_t refused_ = 0;

  /* libnvrm.
   *
   * The opener is the one without a device number. The interface has both,
   * and the one this device exports is the newer -- which on its own settles
   * that the compositor shipped alongside these libraries could not be
   * reused: it was built against the older name, which is gone. */
  int (*rm_open_)(void **) = nullptr;
  void (*rm_close_)(void *) = nullptr;

  /* libnvrm_graphics -- turning the engine's own fences into descriptors the
   * rest of the system understands, and back. */
  int (*fence_from_fd_)(int32_t, void *, uint32_t *) = nullptr;
  int (*fence_to_fd_)(const char *, const void *, uint32_t, int32_t *) = nullptr;

  /* libnvddk_vic.
   *
   * The session maker takes three arguments here and only two in the last
   * source drop published: a channel to the engine was added between the
   * device and the answer. Passing zero for it is asking the library to open
   * its own, which is what we want and what its own callers do.
   *
   * Getting this wrong was not a failure to compile but a failure to work.
   * The second argument was the address of the answer, which is never zero,
   * so the library took it for a channel that was already open, never opened
   * one, and issued its commands against a descriptor read out of a stack
   * address -- which came out as zero, hence a stream of them against
   * standard input. Established by decompiling the shipped library. */
  int (*create_session_)(void *, int, void **) = nullptr;
  void (*free_session_)(void *) = nullptr;
  int (*configure_)(void *, void *) = nullptr;
  int (*execute_)(void *, void *, void *, uint32_t, void *) = nullptr;
  int (*configure_source_)(void *, void *, uint32_t, const void *, uint32_t,
                           const void *, const void *) = nullptr;
  int (*configure_target_)(void *, void *, const void *, uint32_t,
                           const void *) = nullptr;
  void (*configure_blending_)(void *, void *, uint32_t, int, float) = nullptr;
  int (*configure_clear_rects_)(void *, void *) = nullptr;
  int (*configure_transform_)(void *, void *, uint32_t) = nullptr;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_VIC_SESSION_H
