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

/* Asked, not waited for.
 *
 * This runs inside the assembling of a frame, and a frame is the one thing
 * here that must not be made to wait. With three buffers a slot comes round
 * again two frames after it was shown, so by the time it is wanted the display
 * has long finished with it and the question is answered immediately -- but
 * "almost always immediately" is not the same as "never blocks", and on the
 * path a frame takes only the second one will do.
 *
 * Every other implementation is built the same way round: Samsung moves
 * framebuffer teardown, buffer release and reallocation onto three separate
 * threads for exactly this reason, and Intel hands its own teardown to the
 * compositor thread. None of them lets the caller of a commit wait for
 * housekeeping.
 *
 * So the slot is polled. Still busy means the merge is skipped for this frame
 * and the layers go their ordinary way, which is what the caller already does
 * with a refusal -- a frame late by one composition, rather than a frame late
 * by however long the wait took.
 */
constexpr int kNoWait = 0;

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
  /* Whichever is free, rather than whichever is next.
   *
   * Taking them strictly in turn means asking for one particular buffer and
   * accepting whatever state it happens to be in -- so a slot that went out
   * two frames ago and has long since come back is passed over in favour of
   * the one that was shown last, purely because its number came up. Starting
   * from the one after the last used keeps them evenly worn when they are all
   * free, which is the ordinary case; the rest of the round is there for when
   * they are not.
   */
  for (size_t tried = 0; tried < slots_.size(); ++tried) {
    const size_t at = (at_ + 1 + tried) % slots_.size();
    Slot &slot = slots_[at];

    /* What is on screen is not a candidate at any price. Nothing has replaced
     * it yet, so there is no fence that could say when it comes free, and
     * drawing into it would be drawing into the picture. */
    if (slot.showing)
      continue;

    /* Asked and not waited for -- see kNoWait. A slot whose replacement has
     * not reached the panel is simply not this frame's slot. */
    if (slot.freed_when && sync_wait(*slot.freed_when, kNoWait) < 0)
      continue;

    slot.freed_when = {};
    at_ = at;
    return slot.handle;
  }

  /* Every buffer this pool has is still somewhere between here and the panel.
   * One is being shown and one may be waiting behind it, so a third that is
   * also unavailable means the display is further behind than it can be while
   * anything here is working -- not a slow frame but a stuck one.
   *
   * Nothing good can be done with it. Writing into a buffer the display is
   * reading would tear it, and waiting for one would only add this frame to a
   * queue that is already too long. The caller drops the frame, which is the
   * one response that lets the pipeline drain.
   */
  ALOGE("all %zu scratch buffers are still on their way to the panel",
        slots_.size());
  return nullptr;
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
  if (anything_showing_ && showing_ < slots_.size()) {
    Slot &previous = slots_[showing_];
    previous.showing = false;
    previous.freed_when = fence;
  }

  slots_[at_].showing = true;
  slots_[at_].freed_when = {};
  showing_ = at_;
  anything_showing_ = true;
}

}  // namespace hwc
}  // namespace android
