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

#include "tegra/ScratchPool.h"

#include <hardware/gralloc.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>

#include "tegra/NvMapAllocator.h"
#include "utils/log.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-scratch"

namespace android {
namespace hwc {

namespace {

/* Read often by the processor, and shown by the controller.
 *
 * The first of those is the only way to ask this allocator for rows rather
 * than blocks -- it has no flag for the arrangement itself, only the rule
 * that a buffer the processor reads often is laid out in rows. See the note
 * on the class for why rows are what the merge wants. */
constexpr uint64_t kUsage = GRALLOC_USAGE_HW_COMPOSER |
                            GRALLOC_USAGE_SW_READ_OFTEN;

}  // namespace

std::unique_ptr<ScratchPool> ScratchPool::Create(uint32_t width,
                                                 uint32_t height,
                                                 size_t count) {
  if (width == 0 || height == 0 || count == 0)
    return nullptr;

  auto pool = std::unique_ptr<ScratchPool>(new ScratchPool());
  pool->width_ = width;
  pool->height_ = height;
  pool->slots_.resize(count);

  /* The zone first, the whole pool or nothing: a refusal partway
   * through is a pool half-carved from a zone that turned out not to
   * have the room, so the whole pool is let go and asked of gralloc
   * instead -- one origin per pool, one answer in the dump. */
  auto *zone = NvMapAllocator::GetInstance();
  if (zone != nullptr) {
    bool whole = true;
    for (size_t i = 0; i < count; i++) {
      auto buffer = zone->Allocate(width, height);
      if (!buffer) {
        ALOGE("the zone gave %zu of %zu %ux%u; taking the pool to gralloc",
              i, count, width, height);
        whole = false;
        break;
      }
      pool->slots_[i].buffer = std::move(*buffer);
    }
    if (whole) {
      pool->origin_ = ScratchBuffer::Origin::kCarveout;
      pool->stride_ = pool->slots_[0].buffer.pitch() / 4;
      ALOGI("%zu carveout buffers, %ux%u, pitch %u", count, width, height,
            pool->slots_[0].buffer.pitch());
      return pool;
    }

    /* The refused half must be given back before the pool is rebuilt. */
    for (auto &slot : pool->slots_)
      slot.buffer = ScratchBuffer();
  }

  auto &allocator = GraphicBufferAllocator::get();

  for (size_t i = 0; i < count; i++) {
    uint32_t stride = 0;
    buffer_handle_t handle = nullptr;
    const status_t err = allocator.allocate(width, height,
                                            PIXEL_FORMAT_RGBA_8888, 1, kUsage,
                                            &handle, &stride, 0,
                                            "hwc-scratch");
    if (err != NO_ERROR || handle == nullptr) {
      ALOGE("cannot allocate scratch %zu of %zu at %ux%u: %d", i + 1, count,
            width, height, err);
      return nullptr;
    }
    pool->slots_[i].buffer = ScratchBuffer::FromGralloc(handle);

    /* All of them come out the same shape or the pool is not a pool. */
    if (i == 0)
      pool->stride_ = stride;
    else if (stride != pool->stride_) {
      ALOGE("scratch %zu has stride %u against %u", i, stride, pool->stride_);
      return nullptr;
    }
  }

  ALOGI("%zu scratch buffers, %ux%u, stride %u", count, width, height,
        pool->stride_);
  return pool;
}

void ScratchPool::FreeSlots() {
  auto &allocator = GraphicBufferAllocator::get();
  for (auto &slot : slots_)
    if (slot.buffer.origin() == ScratchBuffer::Origin::kGralloc &&
        slot.buffer.handle() != nullptr)
      allocator.free(slot.buffer.handle());
}

ScratchPool::~ScratchPool() {
  FreeSlots();
}

ScratchBuffer *ScratchPool::Next(drm_hwcomposer::SharedFd *ready) {
  if (slots_.empty())
    return nullptr;

  /* Strictly in turn, over every buffer there is.
   *
   * Nothing is held back and nothing is inspected: the buffer three frames
   * ago is the buffer whose turn it is, and whether the display has finished
   * with it is a question for the fence handed out beside it rather than for
   * this call. That is what keeps all three in the rotation.
   *
   * Choosing instead -- passing over what is on screen, testing the rest and
   * taking the first whose fence is due -- looks safer and is not: a pool of
   * three then lends out two, and it turns a frame that would merely have
   * been drawn a little later into a frame that is not drawn at all.
   */
  at_ = (at_ + 1) % slots_.size();
  Slot &slot = slots_[at_];

  if (ready != nullptr)
    *ready = slot.freed_when;

  return &slot.buffer;
}

void ScratchPool::Rewind() {
  if (!slots_.empty())
    at_ = (at_ + slots_.size() - 1) % slots_.size();
}

void ScratchPool::Presented(const drm_hwcomposer::SharedFd &fence) {
  /* This frame replaces whatever was on screen, so that one is finished with
   * as soon as this frame appears -- which is what its fence says.
   *
   * What stood here gave the buffer this frame was drawn into the fence of
   * the very frame it belongs to, which reads as "free the moment it goes
   * up". It survived only because the driver signalled that fence late enough
   * for the mistake not to show; once the fence came due at the vblank it
   * belongs to, the pool started handing back the buffer the display was
   * reading and the engine drew over the picture.
   */
  if (anything_showing_ && showing_ < slots_.size())
    slots_[showing_].freed_when = fence;

  /* Nothing can say when this one comes free: the frame that will replace it
   * has not been posted. It is answered three turns from now, by which time
   * the frame above will have named it. */
  slots_[at_].freed_when = {};
  showing_ = at_;
  anything_showing_ = true;
}

}  // namespace hwc
}  // namespace android
