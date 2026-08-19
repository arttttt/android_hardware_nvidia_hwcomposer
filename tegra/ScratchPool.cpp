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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>

#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>

#include "bufferinfo/NvGralloc.h"
#include "tegra/VicSession.h"
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

/* The colour a vendor-born slot is painted before its first use, as the
 * thirty-two bit word the buffer is made of, opaque. Which of the two end
 * colours reads as red and which as blue depends on the reader's swizzle
 * and does not matter: what matters is that a slot the engine never wrote
 * stands on the panel whole and solid, and tells itself apart from its
 * neighbours. Three slots, three colours. */
constexpr uint32_t kSlotColours[] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u};
constexpr size_t kSlotColourCount =
    sizeof(kSlotColours) / sizeof(kSlotColours[0]);

/* The stripe step, in pixels: wide enough to read on the panel, narrow
 * enough that a wrong row pitch skews the stripes visibly and a wrong
 * layout scatters them. */
constexpr uint32_t kStripeStep = 64;

/* The colour word a stripe alternates with. */
constexpr uint32_t kStripeDark = 0xff000000u;

/* The descriptor words a target is judged by. Read by index rather than
 * through a declared structure for the same reason the allocator's own
 * reader does (bufferinfo/NvGralloc.cpp): the structure belongs to the
 * vendor and has no promise of stability, and the indices are the ones
 * established against the library on this device. */
enum SurfaceWord {
  kWordWidth = 0,
  kWordHeight = 1,
  kWordFormat = 2,
  kWordLayout = 4,
  kWordPitch = 5,
  kWordKind = 8,
  kWordBlockHeightLog2 = 9,
  kWordSize = 14,
  /* The memory handle the engine's reloc is built from. */
  kWordMemHandle = 6,
};

/* Vertical stripes over the slot's own colour, painted before the slot's
 * first use. A slot the engine never wrote shows whole stripes; a slot
 * written with a wrong row pitch shows them skewed, a wrong layout shows
 * them scattered -- the shape of the mistake is readable straight off the
 * panel, not just the fact of it. */
void PaintSlot(VendorBuffer *buffer, size_t index) {
  const uint32_t colour = kSlotColours[index % kSlotColourCount];
  const size_t row_words = buffer->pitch / 4;

  std::vector<uint32_t> row(row_words);
  for (size_t x = 0; x < row_words; ++x)
    row[x] = (x / kStripeStep) % 2 == 0 ? colour : kStripeDark;

  auto *pixels = reinterpret_cast<uint8_t *>(buffer->pixels);
  for (uint32_t y = 0; y < buffer->height; ++y)
    memcpy(pixels + static_cast<size_t>(y) * buffer->pitch, row.data(),
           buffer->pitch);
}

}  // namespace

std::unique_ptr<ScratchPool> ScratchPool::Create(uint32_t width,
                                                 uint32_t height,
                                                 size_t count,
                                                 VicSession *vic) {
  if (width == 0 || height == 0 || count == 0)
    return nullptr;

  auto pool = std::unique_ptr<ScratchPool>(new ScratchPool());
  pool->width_ = width;
  pool->height_ = height;
  pool->slots_.resize(count);

  /* 1 asks the vendor library's own allocator for the slots, through the
   * engine session's resource manager. The whole pool or nothing: a refusal
   * partway through is a pool half-born of a library that turned out not to
   * have the room, so the whole pool is let go and asked of gralloc instead
   * -- one origin per pool, one answer in the dump. */
  const int zone = property_get_int32("persist.vendor.hwc.zone", 0);
  if (zone == 1 && vic != nullptr && vic->OffersVendorBuffers()) {
    bool whole = true;
    for (size_t i = 0; i < count; i++) {
      auto buffer = vic->AllocateTarget(width, height);
      if (!buffer) {
        ALOGE("the vendor allocator gave %zu of %zu %ux%u; taking the pool "
              "to gralloc", i, count, width, height);
        whole = false;
        break;
      }

      /* Painted before its first use: a slot the engine never wrote shows
       * its stripes on the panel, a slot written wrong shows them skewed
       * or scattered. */
      PaintSlot(buffer.get(), i);

      pool->slots_[i].view.vendor = buffer.get();
      pool->slots_[i].vendor = std::move(buffer);
    }
    if (whole) {
      pool->vendor_ = true;
      pool->stride_ = pool->slots_[0].vendor->pitch / 4;
      ALOGI("%zu vendor-born buffers, %ux%u, pitch %u", count, width, height,
            pool->slots_[0].vendor->pitch);
      return pool;
    }
    for (auto &slot : pool->slots_) {
      slot.vendor.reset();
      slot.view = {};
    }
  } else if (zone != 0) {
    ALOGI("zone %d is not offered here; the pool is gralloc's", zone);
  }

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
    pool->slots_[i].view.handle = pool->slots_[i].handle;

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
  /* A vendor-born slot frees itself: the mapping and the library's handle
   * go with the buffer. */
}

const ScratchPool::Slot *ScratchPool::Next(drm_hwcomposer::SharedFd *ready) {
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
  SlotStorage &slot = slots_[at_];

  if (ready != nullptr)
    *ready = slot.freed_when;

  return &slot.view;
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

std::string ScratchPool::DumpSlots() const {
  std::string out;
  char line[256];

  for (size_t i = 0; i < slots_.size(); ++i) {
    const SlotStorage &slot = slots_[i];

    /* The descriptor as whoever allocated the buffer filled it: a
     * vendor-born slot carries the words the library's own builder wrote,
     * a gralloc-born one is asked of the allocator -- and both are read at
     * the same indices, so the two dumps can be set side by side. Beside
     * the descriptor, the slot's dma-buf number and the memory handle it
     * was born with: the kernel's pin log is matched against these, and a
     * slot whose number never appears there while merges run was never
     * handed to the engine. */
    auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
    const uint32_t *words = nullptr;
    int slot_fd = -1;
    void *born_handle = nullptr;
    if (slot.vendor != nullptr) {
      words = slot.vendor->surface.data();
      slot_fd = slot.vendor->mem_fd();
      born_handle = slot.vendor->mem_handle;
    } else if (slot.handle != nullptr) {
      const void *raw = nullptr;
      size_t count = 0;
      if (gralloc != nullptr) {
        slot_fd = gralloc->GetMemFd(slot.handle);
        if (gralloc->GetRawSurfaces(slot.handle, &raw, &count) && count == 1)
          words = static_cast<const uint32_t *>(raw);
      }
    }

    if (words != nullptr) {
      snprintf(line, sizeof(line),
               "  slot %zu: fd %d born %p; desc w%u h%u fmt 0x%08x "
               "layout %u pitch %u kind 0x%02x bh %u size %u handle 0x%08x\n",
               i, slot_fd, born_handle, words[kWordWidth],
               words[kWordHeight], words[kWordFormat], words[kWordLayout],
               words[kWordPitch], words[kWordKind],
               words[kWordBlockHeightLog2], words[kWordSize],
               words[kWordMemHandle]);
    } else {
      snprintf(line, sizeof(line), "  slot %zu: fd %d born %p; "
               "no descriptor\n", i, slot_fd, born_handle);
    }
    out += line;

    /* The pixels, through the processor's own view of the buffer: the
     * vendor-born slot's standing mapping, a gralloc-born slot locked for
     * the moment of the read. */
    const uint32_t *pixels = nullptr;
    size_t row_words = 0;
    buffer_handle_t locked = nullptr;
    if (slot.vendor != nullptr) {
      pixels = slot.vendor->pixels;
      row_words = slot.vendor->pitch / 4;
    } else if (slot.handle != nullptr) {
      auto &mapper = GraphicBufferMapper::get();
      void *base = nullptr;
      if (mapper.lock(slot.handle, GRALLOC_USAGE_SW_READ_OFTEN,
                      Rect(static_cast<int32_t>(width_),
                           static_cast<int32_t>(height_)),
                      &base) == NO_ERROR &&
          base != nullptr) {
        pixels = static_cast<const uint32_t *>(base);
        row_words = stride_;
        locked = slot.handle;
      }
    }

    if (pixels == nullptr || row_words < 8) {
      out += "    (unreadable)\n";
    } else {
      out += "    row0:";
      for (size_t w = 0; w < 8; ++w) {
        snprintf(line, sizeof(line), " %08x", pixels[w]);
        out += line;
      }

      const size_t centre =
          (height_ / 2) * row_words + (width_ / 2);
      if (centre + 8 <= height_ * row_words) {
        out += "  centre:";
        for (size_t w = 0; w < 8; ++w) {
          snprintf(line, sizeof(line), " %08x", pixels[centre + w]);
          out += line;
        }
      }
      out += "\n";
    }

    if (locked != nullptr)
      GraphicBufferMapper::get().unlock(locked);
  }
  return out;
}

}  // namespace hwc
}  // namespace android
