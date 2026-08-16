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

#include "tegra/TurnPool.h"

#include <hardware/gralloc.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>

#include "utils/log.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-turn"

namespace android {
namespace hwc {

namespace {

/* Rows, for the same reason the show pool asks for rows -- see the note
 * there. The engine reads and writes either arrangement; rows are what this
 * allocator gives a buffer the processor claims to read, and what keeps the
 * edge beyond a crop a neighbouring row instead of a tile's leavings. */
constexpr uint64_t kUsage = GRALLOC_USAGE_HW_COMPOSER |
                            GRALLOC_USAGE_SW_READ_OFTEN;

/* Slots are cut to the asked size rounded up to this, so that the turned
 * popup of one frame fits the slot of the last one without a fresh
 * allocation, and without holding panel-sized memory for it. */
constexpr uint32_t kGrain = 256;

/* Frames without a single turn before the slots are given back. Ten-odd
 * seconds at the panel's rate: longer than any pause inside a session of
 * turning, far shorter than the gaps between sessions. */
constexpr uint32_t kIdleFrames = 600;

uint32_t RoundUp(uint32_t v, uint32_t max) {
  const uint32_t rounded = ((v + kGrain - 1) / kGrain) * kGrain;
  return rounded < max ? rounded : max;
}

}  // namespace

TurnPool::TurnPool(uint32_t max_width, uint32_t max_height, size_t cap)
    : max_width_(max_width), max_height_(max_height), cap_(cap) {}

TurnPool::~TurnPool() {
  auto &allocator = GraphicBufferAllocator::get();
  for (auto &slot : slots_)
    if (slot.handle != nullptr)
      allocator.free(slot.handle);
}

buffer_handle_t TurnPool::Take(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > max_width_ ||
      height > max_height_)
    return nullptr;

  /* The smallest idle slot that fits, so a popup's turn does not squat in
   * the one full-screen slot a video will want. */
  Slot *best = nullptr;
  for (auto &slot : slots_) {
    if (slot.taken || slot.width < width || slot.height < height)
      continue;
    if (best == nullptr || slot.bytes < best->bytes)
      best = &slot;
  }
  if (best != nullptr) {
    best->taken = true;
    return best->handle;
  }

  if (slots_.size() >= cap_)
    return nullptr;

  Slot slot;
  slot.width = RoundUp(width, max_width_);
  slot.height = RoundUp(height, max_height_);

  uint32_t stride = 0;
  auto &allocator = GraphicBufferAllocator::get();
  const status_t err = allocator.allocate(slot.width, slot.height,
                                          PIXEL_FORMAT_RGBA_8888, 1, kUsage,
                                          &slot.handle, &stride, 0,
                                          "hwc-turn");
  if (err != NO_ERROR || slot.handle == nullptr) {
    ALOGE("cannot allocate a %ux%u intermediate: %d", slot.width,
          slot.height, err);
    return nullptr;
  }

  slot.bytes = stride * slot.height * 4;
  slot.taken = true;
  slots_.push_back(slot);
  ALOGI("intermediate %zu of %zu, %ux%u", slots_.size(), cap_, slot.width,
        slot.height);
  return slot.handle;
}

void TurnPool::FrameEnd(bool turned_any) {
  for (auto &slot : slots_)
    slot.taken = false;

  if (turned_any) {
    idle_frames_ = 0;
    return;
  }
  if (slots_.empty())
    return;

  if (++idle_frames_ >= kIdleFrames) {
    /* Nothing has turned for hundreds of frames, so nothing will read these
     * again: the only reader an intermediate ever has is the group pass of
     * its own frame, and the channel finished those long ago. */
    auto &allocator = GraphicBufferAllocator::get();
    for (auto &slot : slots_)
      if (slot.handle != nullptr)
        allocator.free(slot.handle);
    slots_.clear();
    idle_frames_ = 0;
    ALOGI("idle, intermediates given back");
  }
}

size_t TurnPool::held_bytes() const {
  size_t total = 0;
  for (const auto &slot : slots_)
    total += slot.bytes;
  return total;
}

}  // namespace hwc
}  // namespace android
