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

#include <map>
#include <optional>

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

 private:
  /* What a buffer looks like, which is settled when it is allocated.
   *
   * Size, arrangement and the format the display knows it by do not change
   * while a buffer exists, and a layer draws into a small ring of buffers in
   * turn -- so asking the allocator about the same handful over and over, once
   * per layer per frame, is the same question answered the same way sixty
   * times a second.
   *
   * Only this. What the allocator hands back alongside -- the descriptor of
   * the memory, and the caller's own handle -- is borrowed for the frame it
   * was asked in, and is taken again every time. Keeping those was tried and
   * took the picture apart; keeping only the shape cannot, because nothing
   * here reaches the hardware.
   */
  struct Shape {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t offset;
    uint32_t format;
    uint64_t modifier;
  };

  /* Bounded, so that a layer handed a new buffer every frame cannot remember
   * all of them for as long as it lives. Larger than any ring this composer
   * will meet, and cheap to throw away whole. */
  static constexpr size_t kMostToRemember = 32;
  std::map<BufferUniqueId, Shape> shapes_;
};

}  // namespace android::drm_hwcomposer
