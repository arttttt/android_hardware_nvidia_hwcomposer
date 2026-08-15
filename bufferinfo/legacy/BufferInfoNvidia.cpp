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

#include "bufferinfo/legacy/BufferInfoNvidia.h"

#include <cutils/native_handle.h>
#include <sys/stat.h>
#include <drm/drm_fourcc.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <sstream>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/GrallocBufferHandle.h"
#include "bufferinfo/NvGralloc.h"
#include "utils/Logging.h"
#include "tegra/VicProbe.h"
#include "utils/log.h"

/* How this hardware arranges memory, said in the way everything else says it.
 *
 * The tree this builds in carries a copy of the format header from before
 * these were published, so the two that describe this vendor's layouts are
 * spelled out here. The values are the published ones and have not moved.
 *
 * The block-linear code carries the shape of the arrangement in its low bits:
 * how tall a block is, which kind of memory it is, and the generation of the
 * layout. Only the first two are read back here; the rest are what this
 * hardware always uses.
 */
#ifndef DRM_FORMAT_MOD_VENDOR_NVIDIA
#define DRM_FORMAT_MOD_VENDOR_NVIDIA 0x03
#endif

#ifndef DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h)                 \
  fourcc_mod_code(NVIDIA, (0x10 | ((h) & 0xf) | (((k) & 0xff) << 12) |       \
                           (((g) & 0x3) << 20) | (((s) & 0x1) << 22) |       \
                           (((c) & 0x7) << 23)))
#endif

#ifndef DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED fourcc_mod_code(NVIDIA, 1)
#endif

/* Plain rows have been spelled both ways over the years, and the copy of the
 * format header in this tree only knows the older spelling. */
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR DRM_FORMAT_MOD_NONE
#endif

namespace android::drm_hwcomposer {

LEGACY_BUFFER_INFO_GETTER(BufferInfoNvidia);

namespace {

/* How the allocator arranged this buffer, in words rather than in a modifier
 * code. Written into the caller's storage, since this is only ever handed
 * straight to a log line. */
void DescribeLayout(const NvGralloc::Surface &surface, char *out, size_t size) {
  switch (surface.layout) {
    case NvGralloc::kLayoutBlocklinear:
      snprintf(out, size, "blocklinear, block height 2^%u, kind %u",
               static_cast<unsigned>(surface.block_height_log2),
               static_cast<unsigned>(surface.kind));
      break;
    case NvGralloc::kLayoutTiled:
      snprintf(out, size, "tiled");
      break;
    case NvGralloc::kLayoutPitch:
      snprintf(out, size, "pitch-linear");
      break;
    default:
      snprintf(out, size, "layout %u, which nothing here recognises",
               surface.layout);
      break;
  }
}

}  // namespace

auto BufferInfoNvidia::GetBoInfo(buffer_handle_t handle)
    -> std::optional<BufferInfo> {
  auto *gralloc = NvGralloc::GetInstance();
  if (gralloc == nullptr)
    return {};

  if (!gralloc->IsValid(handle)) {
    ALOGE("buffer %p is not one of the allocator's", handle);
    return {};
  }

  const int fd = gralloc->GetMemFd(handle);
  if (fd < 0) {
    ALOGE("buffer %p has no memory descriptor", handle);
    return {};
  }

  /* Which buffer this is, before anything is asked about it: the shape of a
   * buffer is settled at allocation and cannot change while it exists, so a
   * buffer already seen is answered from memory of it -- borrowings aside,
   * which are taken fresh every time. */
  BufferUniqueId unique_id = 0;
  struct stat sb = {};
  if (fstat(fd, &sb) == 0) {
    unique_id = static_cast<BufferUniqueId>(sb.st_ino);
  }

  BufferInfo bi{};

  /* Carried down for the image compositor, which cannot be told about a
   * buffer by descriptor -- the allocator answers about handles. Borrowed:
   * whoever handed the buffer over owns it, and nothing that reads this
   * outlives the frame it was described for. */
  bi.handle = handle;
  bi.prime_fds[0] = fd;
  bi.unique_id = unique_id;

  const auto asked_at = std::chrono::steady_clock::now();
  const auto MicrosSince = [asked_at]() -> uint64_t {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - asked_at)
            .count());
  };

  const auto remembered = unique_id != 0 ? shapes_.find(unique_id)
                                         : shapes_.end();
  if (remembered != shapes_.end()) {
    ++shape_hits_;
    const BufferShape& shape = remembered->second;
    bi.width = shape.width;
    bi.height = shape.height;
    bi.pitches[0] = shape.pitch;
    bi.offsets[0] = shape.offset;
    bi.format = shape.format;
    bi.modifiers[0] = shape.modifier;
    bi.fds_shared = Import(handle);
    shape_hit_us_ += MicrosSince();
    return bi;
  }
  ++shape_misses_;

  /* The last place a layer is still known by the allocator's own handle,
   * and now genuinely the first sight of a buffer rather than every frame's.
   *
   * Everything below reads the description this builds, and the handle is
   * part of it -- see BufferInfo::handle. */
  hwc::VicProbe::Offer(handle);

  NvGralloc::Surface surface{};
  if (!gralloc->DescribeSurface(handle, &surface))
    return {};

  bi.width = surface.width;
  bi.height = surface.height;
  bi.pitches[0] = surface.pitch;
  bi.offsets[0] = surface.offset;

  const int hal_format = gralloc->GetHalFormat(handle);
  bi.format = ConvertHalFormatToDrm(static_cast<uint32_t>(hal_format));
  if (bi.format == DRM_FORMAT_INVALID) {
    ALOGV("Cannot convert hal format to drm format %u", hal_format);
    return {};
  }

  /* Blocklinear is what the GPU renders into by default here, so it is the
   * ordinary case rather than the exotic one. Told to read such memory row by
   * row, a display shows an orderly scramble -- the least helpful way for
   * this to go wrong, which is why it is stated rather than assumed. */
  switch (surface.layout) {
    case NvGralloc::kLayoutBlocklinear:
      bi.modifiers[0] = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(
          0, 0, 0, surface.kind, surface.block_height_log2);
      break;
    case NvGralloc::kLayoutTiled:
      bi.modifiers[0] = DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED;
      break;
    case NvGralloc::kLayoutPitch:
    default:
      bi.modifiers[0] = DRM_FORMAT_MOD_LINEAR;
      break;
  }

  if (unique_id != 0) {
    if (shapes_.size() >= kMostShapesToRemember) {
      shapes_.clear();
    }
    shapes_[unique_id] = BufferShape{.width = bi.width,
                                     .height = bi.height,
                                     .pitch = bi.pitches[0],
                                     .offset = bi.offsets[0],
                                     .format = bi.format,
                                     .modifier = bi.modifiers[0]};
  }

  shape_miss_us_ += MicrosSince();

  /* The buffer as the allocator knows it, carried along with the reading of
   * it. Everything above this is told in the vocabulary the composer shares
   * with a DRM driver -- a descriptor, a pitch, a layout -- and that is enough
   * to hand a frame to the display. It is not enough to ask the allocator to
   * do anything to the buffer, which takes its own handle and nothing else,
   * and one thing does have to be asked of it: undoing the compression this
   * display cannot read.
   *
   * That question is asked much later, once the plan has settled and it is
   * known which buffers the display will actually read. This is the last place
   * that has the handle, so this is where it is kept.
   */
  bi.fds_shared = Import(handle);

  /* What the allocator actually handed over. Said once per buffer rather than
   * once per frame: this runs when a buffer is first seen, and the answer is
   * then remembered for as long as the buffer lives.
   *
   * The layout is the part worth having and the one nothing else reports. The
   * controller's fourth window reads pitch-linear only, so whether anything on
   * this screen arrives pitch-linear decides whether that window can be used
   * at all -- or whether reaching it would cost a copy of the layer every
   * frame, which may well be dearer than letting the GPU take it.
   */
  if (HWC_TRACE_ENABLED && ::android::hwc::ExplanationWanted()) {
    char layout[64];
    DescribeLayout(surface, layout, sizeof(layout));
    HWC_LOGX("buffer %p: %ux%u %c%c%c%c pitch %u offset %u -- %s", handle,
             bi.width, bi.height, static_cast<char>(bi.format),
             static_cast<char>(bi.format >> 8),
             static_cast<char>(bi.format >> 16),
             static_cast<char>(bi.format >> 24), bi.pitches[0], bi.offsets[0],
             layout);
  }

  return bi;
}

auto BufferInfoNvidia::GetUniqueId(buffer_handle_t handle)
    -> std::optional<BufferUniqueId> {
  auto *gralloc = NvGralloc::GetInstance();
  if (gralloc == nullptr || !gralloc->IsValid(handle))
    return {};

  /* Asked of the allocator rather than read off the handle. The descriptor
   * it hands back is the memory the pixels live in, and two handles naming
   * the same memory are the same buffer -- which is what the caller is
   * asking. Borrowed, so nothing is closed here. */
  const int fd = gralloc->GetMemFd(handle);
  if (fd < 0)
    return {};

  struct stat sb = {};
  if (fstat(fd, &sb) != 0)
    return {};

  return static_cast<BufferUniqueId>(sb.st_ino);
}

std::string BufferInfoNvidia::DumpState() {
  std::stringstream ss;
  ss << "Buffer shapes remembered:\n"
     << "  described               : " << shape_misses_ << "\n"
     << "  recognised              : " << shape_hits_ << "\n"
     << "  remembered now          : " << shapes_.size() << "\n";
  if (shape_misses_ > 0) {
    ss << "  describe cost           : " << shape_miss_us_ << " us (mean "
       << shape_miss_us_ / shape_misses_ << ")\n";
  }
  if (shape_hits_ > 0) {
    ss << "  recognise cost          : " << shape_hit_us_ << " us (mean "
       << shape_hit_us_ / shape_hits_ << ")\n";
  }
  return ss.str();
}

}  // namespace android::drm_hwcomposer
