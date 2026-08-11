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
#include <sync/sync.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>

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

/* Long enough that reaching it means something is wrong rather than slow. The
 * display is at sixty frames a second; a buffer still held after a fifth of a
 * second is not late, it is stuck. */
constexpr int kWaitMs = 200;

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

  auto &allocator = GraphicBufferAllocator::get();

  for (size_t i = 0; i < count; i++) {
    uint32_t stride = 0;
    const status_t err = allocator.allocate(width, height,
                                            PIXEL_FORMAT_RGBA_8888, 1, kUsage,
                                            &pool->slots_[i].handle, &stride,
                                            0, "hwc-scratch");
    if (err != NO_ERROR || pool->slots_[i].handle == nullptr) {
      ALOGE("cannot allocate scratch %zu of %zu at %ux%u: %d", i + 1, count,
            width, height, err);
      return nullptr;
    }

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

ScratchPool::~ScratchPool() {
  auto &allocator = GraphicBufferAllocator::get();
  for (auto &slot : slots_)
    if (slot.handle != nullptr)
      allocator.free(slot.handle);
}

buffer_handle_t ScratchPool::Next() {
  at_ = (at_ + 1) % slots_.size();
  Slot &slot = slots_[at_];

  if (slot.busy_until) {
    const int err = sync_wait(*slot.busy_until, kWaitMs);
    if (err < 0) {
      /* Nothing good can be done here. Writing anyway would tear whatever is
       * still on the panel, and skipping the merge is what the caller does
       * with a null. */
      ALOGE("scratch %zu still held after %d ms", at_, kWaitMs);
      return nullptr;
    }
    slot.busy_until = {};
  }

  return slot.handle;
}

void ScratchPool::Presented(const drm_hwcomposer::SharedFd &fence) {
  slots_[at_].busy_until = fence;
}

}  // namespace hwc
}  // namespace android
