/*
 * Copyright (C) 2022 The Android Open Source Project
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
#include <memory>

#include <cutils/native_handle.h>

namespace android::drm_hwcomposer {

constexpr int kBufferMaxPlanes = 4;

enum class BufferColorEncoding : int32_t {
  kUndefined,
  kItuRec601,
  kItuRec709,
  kItuRec2020,
};

enum class BufferSampleRange : int32_t {
  kUndefined,
  kFullRange,
  kLimitedRange,
};

enum class BufferBlendMode : int32_t {
  kUndefined,
  kNone,
  kPreMult,
  kCoverage,
};

class PrimeFdsSharedBase {
 public:
  virtual ~PrimeFdsSharedBase() = default;
};

struct BufferInfo {
  uint32_t width;
  uint32_t height;
  uint32_t format; /* DRM_FORMAT_* from drm_fourcc.h */

  /* Which buffer this is, told apart from every other one that has ever
   * lived: the inode of the memory the pixels live in, which the kernel
   * hands out from a counter and never reissues. Zero when the platform
   * could not say -- such a buffer joins no cache rather than joining every
   * other unknown under nought. */
  uint64_t unique_id = 0;
  uint32_t pitches[kBufferMaxPlanes];
  uint32_t offsets[kBufferMaxPlanes];
  /* sizes[] is used only by mapper@4 metadata getter for internal purposes */
  uint32_t sizes[kBufferMaxPlanes];
  int prime_fds[kBufferMaxPlanes];
  uint64_t modifiers[kBufferMaxPlanes];

  BufferColorEncoding color_encoding;
  BufferSampleRange sample_range;
  BufferBlendMode blend_mode;

  /* prime_fds field require valid file descriptors. While their lifecycle is
   * managed elsewhere. The shared_ptr is used to ensure that the fds are not
   * closed while the BufferInfo is still in use. */
  std::shared_ptr<PrimeFdsSharedBase> fds_shared;

  /* What the allocator calls this buffer, for platforms that have to go back
   * and ask it something.
   *
   * Everything else here is the answer to "what does this memory look like",
   * which is all the display controller ever wants. A platform that composes
   * with a second engine wants more than that -- it has to describe the
   * buffer to that engine, and the only description that can be trusted is
   * the allocator's own, which is fetched by handle and not by descriptor.
   *
   * Borrowed, never owned. It belongs to whoever handed the buffer over and
   * is valid for exactly as long as they say; nothing here outlives a frame.
   * Null on platforms that never look. */
  buffer_handle_t handle = nullptr;
};

}  // namespace android::drm_hwcomposer
