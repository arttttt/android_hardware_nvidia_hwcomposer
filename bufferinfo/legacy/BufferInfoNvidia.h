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

#include <cutils/native_handle.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "bufferinfo/BufferInfoGetter.h"

namespace android::drm_hwcomposer {

/* Describes a buffer allocated by the NVIDIA allocator.
 *
 * One of a family: every board here has one of these, and they differ only in
 * how the allocator is asked. This one asks through the library's own
 * functions rather than by reading its handle, because the handle's layout
 * has changed between builds while the function names have not.
 */
class BufferInfoNvidia : public LegacyBufferInfoGetter {
 public:
  using LegacyBufferInfoGetter::LegacyBufferInfoGetter;

  auto GetBoInfo(buffer_handle_t handle) -> std::optional<BufferInfo> override;

  /* Which buffer this is, told apart from every other one.
   *
   * Answered here because the general way of answering it does not work on
   * this allocator's handles. It takes the handle's first descriptor to be
   * the memory the pixels live in and identifies the buffer by which file
   * that is; here the first descriptor is something else, and the question
   * comes back unanswered for every buffer of every frame.
   *
   * What is lost by that is the recognition of a buffer already seen. A layer
   * draws into a small ring of them in turn, and everything read out of one
   * -- its size, its arrangement, the identifier the display knows it by --
   * holds for as long as the buffer does. Unrecognised, all of it is read
   * again on every frame for every layer.
   */
  auto GetUniqueId(buffer_handle_t handle)
      -> std::optional<BufferUniqueId> override;

  std::string DumpState() override;

 private:
  /* The shape of a buffer -- size, pitch, format, arrangement -- is settled
   * at allocation and cannot change while the buffer exists, so it is asked
   * of the allocator once and remembered by the buffer's identity. What is
   * NOT remembered is everything handed back alongside: the descriptor, the
   * caller's handle, the object holding them alive. Those are borrowed for
   * the frame they were asked in and taken again every time -- the lesson
   * of the attempt before this one, which kept them and watched the picture
   * come apart into rows. */
  struct BufferShape {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t offset;
    uint32_t format;
    uint64_t modifier;
  };

  /* Cleared whole when full rather than evicted one by one: filling up is
   * rare -- it takes this many distinct live buffers -- and re-describing a
   * handful once is cheaper to reason about than eviction order. */
  static constexpr size_t kMostShapesToRemember = 64;

  std::map<BufferUniqueId, BufferShape> shapes_;
  uint64_t shape_hits_ = 0;
  uint64_t shape_misses_ = 0;
  uint64_t shape_hit_us_ = 0;
  uint64_t shape_miss_us_ = 0;
};

}  // namespace android::drm_hwcomposer
