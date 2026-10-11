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
#include <memory>
#include <optional>
#include <vector>

#include <cutils/native_handle.h>

#include "utils/fd.h"
#include "vk/HwcVk.h"

struct alloc_device_t;

namespace android {
namespace hwc {

/* The composer's own GPU executor, hwcvk.tegra.so, and the buffers it draws
 * into.
 *
 * What the display windows and VIC cannot take is drawn here instead of by
 * SurfaceFlinger: the layers the plan leaves to the client are composed on
 * the GPU into one of three panel-sized buffers of the composer's own, and
 * that buffer goes where the client target would have gone. The buffers are
 * gralloc's, made with a client target's usage, so the display takes them
 * as it takes SurfaceFlinger's and the GPU imports them with no copy.
 *
 * The buffers rotate strictly in turn, as the merge's scratch pool does, and
 * for the same reason: whether the display has finished with one is a
 * question for the fence handed out beside it, which the GPU waits on.
 */
class GpuExecutor {
 public:
  /* The library, a compositor and the buffers; null if any of the three is
   * missing, having said why. */
  static std::unique_ptr<GpuExecutor> Open(uint32_t width, uint32_t height);

  ~GpuExecutor();

  GpuExecutor(const GpuExecutor &) = delete;
  GpuExecutor &operator=(const GpuExecutor &) = delete;

  struct Drawn {
    buffer_handle_t buffer;
    /* Comes due when the drawing is done. */
    drm_hwcomposer::SharedFd fence;
  };

  /* Draws `layers`, bottom first, into the next buffer. Nothing when the
   * library refused the work; the buffer's turn is then given back. */
  std::optional<Drawn> Compose(std::vector<vk::Layer> &layers);

  /* A frame went up against `fence`; see ScratchPool::Presented. */
  void Presented(const drm_hwcomposer::SharedFd &fence);

  /* Brings the GPU up ahead of a composition; never waits. */
  bool Warm();

  /* Imports `layer`'s buffer on the executor's thread, ahead of the frame
   * that may draw it; never waits. */
  void Prepare(const vk::Layer &layer) { compositor_->Prepare(layer); }

  /* The buffer most recently drawn, or the first if none has been: what
   * stands in as the client target while a plan is weighed, before any
   * frame of this one has been drawn. */
  buffer_handle_t Current() const { return slots_[at_].buffer; }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  GpuExecutor() = default;

  struct Slot {
    buffer_handle_t buffer = nullptr;
    int stride = 0;
    drm_hwcomposer::SharedFd freed_when;
  };

  void *library_ = nullptr;
  void (*destroy_)(vk::Compositor *) = nullptr;
  vk::Compositor *compositor_ = nullptr;
  std::unique_ptr<vk::Host> host_;

  alloc_device_t *alloc_ = nullptr;
  std::array<Slot, 3> slots_{};
  size_t at_ = 0;
  size_t showing_ = 0;
  bool anything_showing_ = false;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace hwc
}  // namespace android
