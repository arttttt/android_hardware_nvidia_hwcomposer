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

#include "tegra/NvMapAllocator.h"
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

void TurnPool::FreeSlot(Slot &slot) {
  if (slot.buffer.origin() == ScratchBuffer::Origin::kGralloc) {
    auto &allocator = GraphicBufferAllocator::get();
    if (slot.buffer.handle() != nullptr)
      allocator.free(slot.buffer.handle());
  }
  slot.buffer = ScratchBuffer();
}

TurnPool::~TurnPool() {
  for (auto &slot : slots_)
    FreeSlot(slot);
}

SurfaceView TurnPool::Take(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > max_width_ ||
      height > max_height_)
    return {};

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
    return best->buffer.View();
  }

  if (slots_.size() >= cap_) {
    /* Every slot is either spoken for this frame or too small. A pool
     * full of popup-sized slots must not starve a video's turn for as
     * long as the scene stands -- that is the execute-refusal storm worn
     * as a memory hat. The largest idle slot is useless to this ask and
     * to the small asks that filled the pool, so it is given back and
     * the ask allocated fresh; an idle slot has no future reader, by the
     * same invariant that lets the trim free everything. Only when every
     * slot is taken by this very frame is the ask truly refused. */
    size_t evict = slots_.size();
    for (size_t i = 0; i < slots_.size(); i++) {
      if (slots_[i].taken)
        continue;
      if (evict == slots_.size() || slots_[i].bytes > slots_[evict].bytes)
        evict = i;
    }
    if (evict == slots_.size())
      return {};

    FreeSlot(slots_[evict]);
    slots_.erase(slots_.begin() + static_cast<long>(evict));
  }

  Slot slot;
  slot.width = RoundUp(width, max_width_);
  slot.height = RoundUp(height, max_height_);

  /* The zone first; gralloc catches what it will not give. The refusal
   * is counted, so the dump can tell a pool that never needs the
   * fallback from one that lives on it. */
  auto *zone = NvMapAllocator::GetInstance();
  if (zone != nullptr) {
    auto buffer = zone->Allocate(slot.width, slot.height);
    if (buffer != nullptr) {
      slot.buffer = std::move(*buffer);
      slot.bytes = slot.buffer.pitch() * slot.height;
      slot.taken = true;
      slots_.push_back(std::move(slot));
      ALOGI("intermediate %zu of %zu, %ux%u, carveout", slots_.size(), cap_,
            slot.width, slot.height);
      return slots_.back().buffer.View();
    }
    zone_refusals_++;
  }

  uint32_t stride = 0;
  buffer_handle_t handle = nullptr;
  auto &allocator = GraphicBufferAllocator::get();
  const status_t err = allocator.allocate(slot.width, slot.height,
                                          PIXEL_FORMAT_RGBA_8888, 1, kUsage,
                                          &handle, &stride, 0, "hwc-turn");
  if (err != NO_ERROR || handle == nullptr) {
    ALOGE("cannot allocate a %ux%u intermediate: %d", slot.width,
          slot.height, err);
    return {};
  }

  slot.buffer = ScratchBuffer::FromGralloc(handle);
  slot.bytes = stride * slot.height * 4;
  slot.taken = true;
  slots_.push_back(std::move(slot));
  ALOGI("intermediate %zu of %zu, %ux%u", slots_.size(), cap_, slot.width,
        slot.height);
  return slots_.back().buffer.View();
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
    for (auto &slot : slots_)
      FreeSlot(slot);
    slots_.clear();
    idle_frames_ = 0;
    trims_++;
    ALOGI("idle, intermediates given back");
  }
}

size_t TurnPool::held_bytes() const {
  size_t total = 0;
  for (const auto &slot : slots_)
    total += slot.bytes;
  return total;
}

size_t TurnPool::carved_slots() const {
  size_t total = 0;
  for (const auto &slot : slots_)
    if (slot.buffer.origin() == ScratchBuffer::Origin::kCarveout)
      total++;
  return total;
}

size_t TurnPool::gralloc_slots() const {
  return slots_.size() - carved_slots();
}

}  // namespace hwc
}  // namespace android
