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
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>

#include "bufferinfo/NvGralloc.h"
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

/* Written by the processor on the rare sprite change, read by the display
 * continuously. */
constexpr uint64_t kSlotUsage = GRALLOC_USAGE_HW_COMPOSER |
                                GRALLOC_USAGE_SW_WRITE_OFTEN |
                                GRALLOC_USAGE_SW_READ_OFTEN;

}  // namespace

std::unique_ptr<CursorUnit> CursorUnit::Claim(int dc_fd) {
  if (ioctl(dc_fd, kGetCursor) != 0) {
    ALOGI("the cursor is not ours to claim: %s", strerror(errno));
    return nullptr;
  }

  ALOGI("cursor unit claimed");
  return std::unique_ptr<CursorUnit>(new CursorUnit(dc_fd));
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
  auto &allocator = GraphicBufferAllocator::get();
  for (auto &slot : slots_) {
    if (slot.handle != nullptr)
      allocator.free(slot.handle);
    slot = {};
  }
}

bool CursorUnit::UploadLocked(buffer_handle_t sprite, uint32_t width,
                              uint32_t height, uint32_t stride_px,
                              bool premultiplied) {
  if (stride_px < width)
    return false;
  const uint32_t side = SideFor(width, height);
  if (side == 0)
    return false;

  auto &allocator = GraphicBufferAllocator::get();
  Slot &slot = slots_[at_ ^ 1];

  if (slot.handle != nullptr && slot.side != side) {
    allocator.free(slot.handle);
    slot = {};
  }

  if (slot.handle == nullptr) {
    uint32_t stride = 0;
    const status_t err = allocator.allocate(side, side,
                                            PIXEL_FORMAT_RGBA_8888, 1,
                                            kSlotUsage, &slot.handle, &stride,
                                            0, "hwc-cursor");
    if (err != NO_ERROR || slot.handle == nullptr) {
      ALOGE("cannot allocate a %ux%u sprite slot: %d", side, side, err);
      slot = {};
      return false;
    }

    /* The unit reads rows with no padding at all. An allocator that pads
     * the stride would shear the sprite silently -- refused here instead,
     * loudly, once. */
    if (stride != side) {
      ALOGE("sprite slot stride %u for width %u; the unit reads dense rows",
            stride, side);
      allocator.free(slot.handle);
      slot = {};
      return false;
    }

    auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
    slot.mem_fd = gralloc != nullptr ? gralloc->GetMemFd(slot.handle) : -1;
    if (slot.mem_fd < 0) {
      ALOGE("a sprite slot with no memory descriptor");
      allocator.free(slot.handle);
      slot = {};
      return false;
    }
    slot.side = side;
  }

  /* The copy. The layer's pixels come in the allocator's RGBA order; the
   * unit wants the same channels with alpha in the low byte, which is the
   * same word read the other way around. Rows past the sprite are cleared
   * to transparent, so a sprite smaller than its slot ends at its own
   * edge. */
  auto &mapper = GraphicBufferMapper::get();
  void *src_ptr = nullptr;
  void *dst_ptr = nullptr;
  if (mapper.lock(sprite, GRALLOC_USAGE_SW_READ_OFTEN,
                  Rect(static_cast<int32_t>(width),
                       static_cast<int32_t>(height)),
                  &src_ptr) != NO_ERROR ||
      src_ptr == nullptr) {
    ALOGE("cannot read the sprite's pixels");
    return false;
  }
  if (mapper.lock(slot.handle, GRALLOC_USAGE_SW_WRITE_OFTEN,
                  Rect(static_cast<int32_t>(side),
                       static_cast<int32_t>(side)),
                  &dst_ptr) != NO_ERROR ||
      dst_ptr == nullptr) {
    mapper.unlock(sprite);
    ALOGE("cannot write the sprite slot");
    return false;
  }

  const auto *src = static_cast<const uint32_t *>(src_ptr);
  auto *dst = static_cast<uint32_t *>(dst_ptr);
  for (uint32_t row = 0; row < side; ++row) {
    for (uint32_t col = 0; col < side; ++col) {
      const uint32_t word = row < height && col < width
                                ? src[row * stride_px + col]
                                : 0;
      dst[row * side + col] = __builtin_bswap32(word);
    }
  }

  mapper.unlock(slot.handle);
  mapper.unlock(sprite);

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
                      bool premultiplied, int32_t x, int32_t y) {
  std::lock_guard<std::mutex> guard(lock_);

  if (id != image_id_ || image_id_ == 0) {
    if (!UploadLocked(sprite, width, height, stride_px, premultiplied)) {
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
  if (slot.handle == nullptr)
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
  if (moves_ == 0 && uploads_ == 0 && refused_ == 0)
    return;
  ss << "Cursor unit:\n"
     << "  async moves served      : " << moves_ << "\n"
     << "  sprite uploads          : " << uploads_ << "\n"
     << "  refusals                : " << refused_ << "\n"
     << "  showing                 : " << (visible_ ? "yes" : "no") << "\n";
}

}  // namespace hwc
}  // namespace android
