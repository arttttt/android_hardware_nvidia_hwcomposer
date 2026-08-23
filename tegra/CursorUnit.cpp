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

#include "tegra/CursorUnit.h"

#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <sys/ioctl.h>

#include <hardware/gralloc.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include <sync/sync.h>

#include "bufferinfo/NvGralloc.h"
#include "tegra/VicSession.h"
#include "utils/log.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-cursor"

namespace android {
namespace hwc {

namespace {

/* The extension's cursor interface, declared here the way the engine's
 * structures are in VicSession: a handful of plain fields, taken verbatim
 * from the kernel's own header rather than through an include that varies
 * by tree. */
struct DcCursorImage {
  struct {
    uint8_t r, g, b;
  } foreground, background;
  uint32_t buff_id;
  uint32_t flags;
  int16_t x, y;
  uint32_t vis;
  uint32_t mode;
};

struct DcCursor {
  int16_t x, y;
  uint32_t flags;
};

constexpr unsigned long kGetCursor = _IO('D', 0x04);
constexpr unsigned long kPutCursor = _IO('D', 0x05);
const unsigned long kSetCursorImage = _IOW('D', 0x06, DcCursorImage);
const unsigned long kSetCursor = _IOW('D', 0x07, DcCursor);

constexpr uint32_t kVisible = 1U << 0;

/* Size class in bits 0..2, format in bits 16..18 -- the kernel's own
 * encoding. */
uint32_t SizeFlag(uint32_t side) {
  switch (side) {
    case 32: return 1;
    case 64: return 2;
    case 128: return 3;
    default: return 4;
  }
}

uint32_t FormatFlag(bool premultiplied) {
  return (premultiplied ? 3U : 1U) << 16;
}

/* The smallest hardware size that holds the sprite. */
uint32_t SideFor(uint32_t width, uint32_t height) {
  const uint32_t need = width > height ? width : height;
  for (uint32_t side : {32U, 64U, 128U, 256U})
    if (need <= side)
      return side;
  return 0;
}

}  // namespace

std::unique_ptr<CursorUnit> CursorUnit::Claim(int dc_fd, VicSession *vic) {
  if (ioctl(dc_fd, kGetCursor) != 0) {
    ALOGI("the cursor is not ours to claim: %s", strerror(errno));
    return nullptr;
  }

  ALOGI("cursor unit claimed");
  return std::unique_ptr<CursorUnit>(new CursorUnit(dc_fd, vic));
}

CursorUnit::~CursorUnit() {
  /* Hidden before it is handed back -- the unit draws independently of
   * frames, and a sprite nobody hides outlives its owner. */
  std::lock_guard<std::mutex> guard(lock_);
  PointLocked(0, 0, false);
  ioctl(fd_, kPutCursor);
  ReleaseSlotsLocked();
}

void CursorUnit::ReleaseSlotsLocked() {
  /* Each buffer frees itself with its slot: the mapping, the library's
   * handle and the zone's memory go together. */
  for (auto &slot : slots_)
    slot = {};
  staging_.reset();
  staging_side_ = 0;
  staging_stride_px_ = 0;
}

const VendorBuffer *CursorUnit::LinearizeLocked(buffer_handle_t sprite,
                                                uint32_t width,
                                                uint32_t height,
                                                int acquire_fence,
                                                uint32_t *stride_px) {
  if (vic_ == nullptr)
    return nullptr;

  const uint32_t side = SideFor(width, height);
  if (side == 0)
    return nullptr;

  if (staging_ && staging_side_ < side)
    staging_.reset();

  if (!staging_) {
    staging_ = vic_->AllocateZoneTarget(side, side);
    if (!staging_) {
      ALOGE("the zone would not give a %ux%u staging surface", side, side);
      return nullptr;
    }
    staging_side_ = side;
    staging_stride_px_ = staging_->pitch / 4;
  }

  /* One engine pass, unturned, lays the block-arranged sprite out in
   * rows; the fence is waited on here because the next reader is the
   * processor, which no channel orders. */
  VicSession::Layer layer = {};
  layer.handle = sprite;
  layer.source_right = static_cast<float>(width);
  layer.source_bottom = static_cast<float>(height);
  layer.display_right = static_cast<int32_t>(width);
  layer.display_bottom = static_cast<int32_t>(height);
  layer.premultiplied = true;
  layer.alpha = 1.0F;
  layer.acquire_fence = acquire_fence;

  auto done = vic_->CopyLayer(*staging_, layer, width, height);
  if (!done) {
    ALOGE("the engine would not lay the sprite flat");
    return nullptr;
  }
  sync_wait(*done, 100);

  *stride_px = staging_stride_px_;
  return staging_.get();
}

bool CursorUnit::UploadLocked(buffer_handle_t sprite, uint32_t width,
                              uint32_t height, uint32_t stride_px,
                              bool premultiplied, int acquire_fence) {
  if (stride_px < width)
    return false;
  const uint32_t side = SideFor(width, height);
  if (side == 0)
    return false;

  Slot &slot = slots_[at_ ^ 1];

  if (slot.buffer && slot.side != side)
    slot = {};

  if (!slot.buffer) {
    /* Dense rows: the unit scans side times four bytes from the buffer's
     * very start, with no layout, pitch or offset register to tell it
     * otherwise. The zone is asked for exactly that grain -- and the ask
     * is checked, because a rounded-up row would shear the sprite
     * silently. */
    slot.buffer = vic_->AllocateZoneTarget(side, side, 4);
    if (!slot.buffer) {
      ALOGE("the zone would not give a %ux%u sprite slot", side, side);
      slot = {};
      return false;
    }
    if (slot.buffer->pitch != side * 4) {
      ALOGE("sprite slot pitch %u for width %u; the unit reads dense rows",
            slot.buffer->pitch, side);
      slot = {};
      return false;
    }

    slot.mem_fd = slot.buffer->mem_fd();
    if (slot.mem_fd < 0) {
      ALOGE("a sprite slot with no memory descriptor");
      slot = {};
      return false;
    }

    /* Kept for the dump in the shape the allocator's own answer had:
     * rows, dense, from the start. */
    slot_desc_[0] = drm_hwcomposer::NvGralloc::kLayoutPitch;
    slot_desc_[1] = slot.buffer->pitch;
    slot_desc_[2] = 0;
    slot.side = side;
  }

  /* Set when the sprite had to be laid flat first: then the pixels come
   * from our staging buffer rather than from the sprite itself. */
  const VendorBuffer *flattened = nullptr;

  /* The sprite's own arrangement: a processor lock of memory arranged in
   * blocks reads structure, not pixels -- the framework draws its
   * pointer on the graphics core, and block-arranged is what that
   * produces. Such a sprite takes one engine pass through the staging
   * surface first, which is how the stock composer read it too. */
  {
    auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
    drm_hwcomposer::NvGralloc::Surface described{};
    if (gralloc != nullptr && gralloc->DescribeSurface(sprite, &described)) {
      sprite_desc_[0] = described.layout;
      sprite_desc_[1] = described.pitch;
      sprite_desc_[2] = described.offset;
      if (described.layout != drm_hwcomposer::NvGralloc::kLayoutPitch) {
        flattened = LinearizeLocked(sprite, width, height, acquire_fence,
                                    &stride_px);
        if (flattened == nullptr)
          return false;
      } else if (described.pitch >= 4) {
        stride_px = described.pitch / 4;
      }
    }
  }

  /* The copy. The layer's pixels come in the allocator's RGBA order; the
   * unit wants the same channels with alpha in the low byte, which is the
   * same word read the other way around. Rows past the sprite are cleared
   * to transparent, so a sprite smaller than its slot ends at its own
   * edge. */
  /* The pixels to read: either the sprite as the framework drew it, or
   * the flat copy the engine laid out in the staging surface -- which is
   * one of ours, and read through its own standing mapping rather than
   * through a lock. */
  auto &mapper = GraphicBufferMapper::get();
  const uint32_t *src = nullptr;
  buffer_handle_t locked = nullptr;
  if (flattened != nullptr) {
    src = flattened->pixels;
  } else {
    void *src_ptr = nullptr;
    if (mapper.lock(sprite, GRALLOC_USAGE_SW_READ_OFTEN,
                    Rect(static_cast<int32_t>(width),
                         static_cast<int32_t>(height)),
                    &src_ptr) != NO_ERROR ||
        src_ptr == nullptr) {
      ALOGE("cannot read the sprite's pixels");
      return false;
    }
    src = static_cast<const uint32_t *>(src_ptr);
    locked = sprite;
  }

  auto *dst = slot.buffer->pixels;

  for (uint32_t row = 0; row < side; ++row) {
    for (uint32_t col = 0; col < side; ++col) {
      const uint32_t word = row < height && col < width
                                ? src[row * stride_px + col]
                                : 0;
      dst[row * side + col] = __builtin_bswap32(word);
    }
  }

  if (locked != nullptr)
    mapper.unlock(locked);

  DcCursorImage image = {};
  image.buff_id = static_cast<uint32_t>(slot.mem_fd);
  image.flags = SizeFlag(side) | FormatFlag(premultiplied);
  if (ioctl(fd_, kSetCursorImage, &image) != 0) {
    ALOGE("the unit refused the sprite: %s", strerror(errno));
    return false;
  }

  at_ ^= 1;
  image_flags_ = image.flags;
  uploads_++;
  return true;
}

bool CursorUnit::PointLocked(int32_t x, int32_t y, bool visible) {
  DcCursor point = {};
  point.x = static_cast<int16_t>(
      x < INT16_MIN ? INT16_MIN : (x > INT16_MAX ? INT16_MAX : x));
  point.y = static_cast<int16_t>(
      y < INT16_MIN ? INT16_MIN : (y > INT16_MAX ? INT16_MAX : y));
  point.flags = visible ? kVisible : 0;
  return ioctl(fd_, kSetCursor, &point) == 0;
}

bool CursorUnit::Show(buffer_handle_t sprite, uint64_t id, uint32_t width,
                      uint32_t height, uint32_t stride_px,
                      bool premultiplied, int acquire_fence, int32_t x,
                      int32_t y, bool position_fresh) {
  std::lock_guard<std::mutex> guard(lock_);

  /* A position the pointer has already moved past is not re-stated: the
   * freshest holder is this object, whose coordinates the overtaking
   * move set. While nothing is shown there was no such move -- a hidden
   * unit drops them -- and the plan's word stands. */
  if (!position_fresh && visible_) {
    x = x_;
    y = y_;
  }

  /* The frame that changed nothing asks for nothing: same sprite, same
   * place, already showing. */
  if (visible_ && id == image_id_ && x == x_ && y == y_)
    return true;

  if (id != image_id_ || image_id_ == 0) {
    if (!UploadLocked(sprite, width, height, stride_px, premultiplied,
                      acquire_fence)) {
      refused_++;
      return false;
    }
    image_id_ = id;
  }

  if (!PointLocked(x, y, true)) {
    refused_++;
    return false;
  }
  visible_ = true;
  x_ = x;
  y_ = y;
  return true;
}

void CursorUnit::Move(int32_t x, int32_t y) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!visible_)
    return;
  if (PointLocked(x, y, true)) {
    x_ = x;
    y_ = y;
    moves_++;
  }
}

void CursorUnit::Hide() {
  std::lock_guard<std::mutex> guard(lock_);
  if (!visible_)
    return;
  PointLocked(0, 0, false);
  visible_ = false;
}

void CursorUnit::Rearm() {
  std::lock_guard<std::mutex> guard(lock_);
  if (!visible_ || image_id_ == 0)
    return;

  /* The image is still in the slot the hardware was last told about --
   * only the telling has to happen again. */
  const Slot &slot = slots_[at_];
  if (!slot.buffer)
    return;

  DcCursorImage image = {};
  image.buff_id = static_cast<uint32_t>(slot.mem_fd);
  image.flags = image_flags_;
  if (ioctl(fd_, kSetCursorImage, &image) != 0)
    ALOGE("cannot re-state the sprite after a blank: %s", strerror(errno));
  PointLocked(x_, y_, true);
}

void CursorUnit::AppendDump(std::ostream &ss) const {
  std::lock_guard<std::mutex> guard(lock_);
  ss << "Cursor unit (claimed):\n"
     << "  async moves served      : " << moves_ << "\n"
     << "  sprite uploads          : " << uploads_ << "\n"
     << "  refusals                : " << refused_ << "\n"
     << "  showing                 : " << (visible_ ? "yes" : "no") << "\n"
     << "  slot lay/pitch/offset   : " << slot_desc_[0] << "/"
     << slot_desc_[1] << "/" << slot_desc_[2] << "\n"
     << "  sprite lay/pitch/offset : " << sprite_desc_[0] << "/"
     << sprite_desc_[1] << "/" << sprite_desc_[2] << "\n";
}

}  // namespace hwc
}  // namespace android
