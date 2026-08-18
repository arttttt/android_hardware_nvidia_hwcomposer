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

#ifndef TEGRA_SCRATCH_BUFFER_H
#define TEGRA_SCRATCH_BUFFER_H

#include <cstdint>
#include <memory>

#include <cutils/native_handle.h>

#include "utils/fd.h"

namespace android {
namespace hwc {

/* What the engine needs to read one buffer: either a gralloc handle to
 * ask the allocator about, or a surface descriptor already built for a
 * carveout buffer of ours. A snapshot rather than a handle to anything:
 * it is a value, safe to keep for the length of a frame no matter what
 * the pool it came from does with its own storage in between. */
struct SurfaceView {
  static SurfaceView Gralloc(buffer_handle_t handle) {
    return SurfaceView{handle, nullptr, 0, 0};
  }

  buffer_handle_t handle;
  const uint32_t *carveout_words;
  uint32_t carveout_width;
  uint32_t carveout_height;
};

/* One scratch buffer, owned: the pool slot's whole payload.
 *
 * Either a gralloc handle -- the fallback, when the zone could not give
 * one -- or a carveout buffer of ours, which is the dma-buf, the memory
 * handle the engine reads through, and the surface descriptor built
 * from them. Gralloc handles are freed by the pool that allocated them,
 * as before; a carveout buffer frees itself: the memory handle is
 * released through the allocator and the descriptors close with it. */
class ScratchBuffer {
 public:
  enum class Origin { kGralloc, kCarveout };

  ScratchBuffer() = default;
  ScratchBuffer(ScratchBuffer &&) = default;
  ScratchBuffer &operator=(ScratchBuffer &&);
  ~ScratchBuffer();

  ScratchBuffer(const ScratchBuffer &) = delete;
  ScratchBuffer &operator=(const ScratchBuffer &) = delete;

  static ScratchBuffer FromGralloc(buffer_handle_t handle);
  static ScratchBuffer FromCarveout(SharedFd fd, void *mem_handle,
                                    std::unique_ptr<uint32_t[]> surface,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch);

  Origin origin() const { return origin_; }

  /* What the engine is given to read. */
  SurfaceView View() const;

  /* Gralloc half, valid when origin is kGralloc. */
  buffer_handle_t handle() const { return handle_; }

  /* Carveout half, valid when origin is kCarveout. */
  int fd() const { return fd_ != nullptr ? fd_->get() : -1; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t pitch() const { return pitch_; }

 private:
  Origin origin_ = Origin::kGralloc;
  buffer_handle_t handle_ = nullptr;

  SharedFd fd_;
  void *mem_handle_ = nullptr;
  std::unique_ptr<uint32_t[]> surface_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t pitch_ = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_SCRATCH_BUFFER_H
