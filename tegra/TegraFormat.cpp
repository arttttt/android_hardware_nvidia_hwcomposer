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

#include "tegra/TegraFormat.h"

#include <drm/drm_fourcc.h>

#include <tegra_dc_ext.h>

namespace android::drm_hwcomposer {

namespace {

/* The vendor's own arrangements, spelled out because the format header in
 * this tree predates them. The values are the published ones. */
constexpr uint64_t kVendorNvidia = 0x03;

constexpr uint64_t NvidiaModifier(uint64_t code) {
  return (kVendorNvidia << 56) | (code & 0x00ffffffffffffffULL);
}

constexpr uint64_t kTegraTiled = NvidiaModifier(1);

/* A block-linear modifier carries the shape of the arrangement in its low
 * bits: the bottom four say how tall a block is, and the bit above them
 * marks it as this family rather than the older one. */
constexpr uint64_t kBlockLinearMarker = 0x10;
constexpr uint64_t kBlockHeightMask = 0x0f;

bool IsBlockLinear(uint64_t modifier) {
  return (modifier >> 56) == kVendorNvidia &&
         (modifier & kBlockLinearMarker) != 0;
}

}  // namespace

uint32_t TegraFormatFromDrm(uint32_t drm_format) {
  switch (drm_format) {
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      return TEGRA_DC_EXT_FMT_R8G8B8A8;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      return TEGRA_DC_EXT_FMT_B8G8R8A8;
    case DRM_FORMAT_BGR565:
      return TEGRA_DC_EXT_FMT_B5G6R5;
    default:
      return 0;
  }
}

bool TegraLayoutFromModifier(uint64_t modifier, uint32_t *out_flags,
                             uint8_t *out_block_height_log2) {
  *out_flags = 0;
  *out_block_height_log2 = 0;

  if (IsBlockLinear(modifier)) {
    *out_flags = TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR;
    *out_block_height_log2 = static_cast<uint8_t>(modifier & kBlockHeightMask);
    return true;
  }

  if (modifier == kTegraTiled) {
    *out_flags = TEGRA_DC_EXT_FLIP_FLAG_TILED;
    return true;
  }

  /* Both spellings of "plain rows" reach here. Anything else is an
   * arrangement this controller has never been told about, and reading it as
   * rows would show a scramble rather than an error. */
  return modifier == DRM_FORMAT_MOD_LINEAR ||
         modifier == DRM_FORMAT_MOD_NONE;
}

}  // namespace android::drm_hwcomposer
