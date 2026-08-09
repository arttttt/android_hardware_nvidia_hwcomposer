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

#include <optional>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/GrallocBufferHandle.h"
#include "bufferinfo/NvGralloc.h"
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

auto BufferInfoNvidia::GetBoInfo(buffer_handle_t handle)
    -> std::optional<BufferInfo> {
  auto *gralloc = NvGralloc::GetInstance();
  if (gralloc == nullptr)
    return {};

  if (!gralloc->IsValid(handle)) {
    ALOGE("buffer %p is not one of the allocator's", handle);
    return {};
  }

  NvGralloc::Surface surface{};
  if (!gralloc->DescribeSurface(handle, &surface))
    return {};

  const int fd = gralloc->GetMemFd(handle);
  if (fd < 0) {
    ALOGE("buffer %p has no memory descriptor", handle);
    return {};
  }

  BufferInfo bi{};

  bi.width = surface.width;
  bi.height = surface.height;
  bi.prime_fds[0] = fd;
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

}  // namespace android::drm_hwcomposer
