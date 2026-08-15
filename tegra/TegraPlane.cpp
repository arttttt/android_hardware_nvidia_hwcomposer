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

#include "tegra/TegraPlane.h"

#include <tegra_dc_ext.h>

#include "compositor/LayerData.h"
#include "tegra/TegraFormat.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

bool TegraPlane::IsValidForLayer(const LayerData *layer) {
  if (layer == nullptr || !layer->bi) {
    ALOGE("%s: no buffer to judge", __func__);
    return false;
  }

  const BufferInfo &bi = *layer->bi;
  const PresentInfo &pi = layer->pi;

  /* Judged by the engine's reach rather than the window's, because the window
   * will be shown a buffer of our own making and not this one. The engine
   * reads memory arranged in blocks, resizes and blends; what it cannot do
   * without is the allocator's own name for the buffer, since that is the
   * only description of it anyone can be trusted to hand over. */
  if (merging_) {
    if (bi.handle == nullptr) {
      ALOGV("plane %u: no handle to describe this buffer with", index_);
      return false;
    }
    return true;
  }

  /* Every refusal below is logged at the quietest level on purpose: this is
   * asked of every plane for every layer, and a plane saying no is the
   * ordinary way a plan is arrived at, not a fault. */

  const uint32_t format = TegraFormatFromDrm(bi.format);
  if (format == 0) {
    ALOGV("plane %u: nothing here reads format 0x%x", index_, bi.format);
    return false;
  }

  if ((caps_.formats & (1ULL << format)) == 0) {
    ALOGV("plane %u does not read format 0x%x", index_, format);
    return false;
  }

  uint32_t flags = 0;
  uint8_t block_height_log2 = 0;
  if (!TegraLayoutFromModifier(bi.modifiers[0], &flags, &block_height_log2)) {
    ALOGV("plane %u: unknown memory arrangement", index_);
    return false;
  }

  if ((flags & TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR) != 0 &&
      !caps_.blocklinearLayout) {
    ALOGV("plane %u does not read memory arranged in blocks", index_);
    return false;
  }

  if ((flags & TEGRA_DC_EXT_FLIP_FLAG_TILED) != 0 && !caps_.tiledLayout) {
    ALOGV("plane %u does not read tiled memory", index_);
    return false;
  }

  if (flags == 0 && !caps_.pitchLayout) {
    ALOGV("plane %u does not read memory arranged in rows", index_);
    return false;
  }

  if (pi.RequireScalingOrPhasing() && !caps_.scaling) {
    ALOGV("plane %u cannot resize", index_);
    return false;
  }

  /* The window's resizing limits, and one thing the driver's tables do not
   * say. The limits first: past them the hardware does not refuse, it
   * silently clamps its stepping and reads memory at the wrong stride --
   * underflow, on exactly the frames heavy enough to ask. The comparison is
   * deliberately no stricter than the hardware: plain ratios with the
   * boundary included, since the stepping arithmetic itself works on
   * (in - 1) / (out - 1).
   *
   * Then the unwritten rule, bug 1515812 out of the stock composer: the
   * controller does not filter alpha on scaled overlays, so a translucent
   * layer that also resizes shows its seams. The stock composer refused the
   * pairing outright; so does this one. */
  if (pi.source_crop.f_rect && pi.display_frame.i_rect) {
    const auto &src = *pi.source_crop.f_rect;
    const auto &dst = *pi.display_frame.i_rect;
    const float src_w = src.Width();
    const float src_h = src.Height();
    const auto dst_w = static_cast<float>(dst.Width());
    const auto dst_h = static_cast<float>(dst.Height());

    if (src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0) {
      if (src_w > dst_w * static_cast<float>(caps_.maxDownH) ||
          src_h > dst_h * static_cast<float>(caps_.maxDownV) ||
          dst_w > src_w * static_cast<float>(caps_.maxUpH) ||
          dst_h > src_h * static_cast<float>(caps_.maxUpV)) {
        ALOGV("plane %u will not resize %gx%g to %gx%g", index_, src_w,
              src_h, dst_w, dst_h);
        return false;
      }

      const bool scaled = src_w != dst_w || src_h != dst_h;
      const bool translucent = bi.blend_mode == BufferBlendMode::kPreMult ||
                               bi.blend_mode == BufferBlendMode::kCoverage ||
                               pi.alpha < 1.0F;
      if (scaled && translucent) {
        ALOGV("plane %u cannot filter alpha while resizing", index_);
        return false;
      }
    }
  }

  if ((pi.transform.hflip && !caps_.invertH) ||
      (pi.transform.vflip && !caps_.invertV) ||
      (pi.transform.rotate90 && !caps_.scanColumn)) {
    ALOGV("plane %u cannot turn the layer that way", index_);
    return false;
  }

  /* The panel's own limits, which the controller states per window and
   * enforces on the flip. Checking them here turns a refused frame into a
   * layer the framework draws instead. */
  if (pi.display_frame.i_rect) {
    const auto &dst = *pi.display_frame.i_rect;
    const auto width = static_cast<uint32_t>(dst.Width());
    const auto height = static_cast<uint32_t>(dst.Height());

    if (width < caps_.minWidth || width > caps_.maxWidth ||
        height < caps_.minHeight || height > caps_.maxHeight) {
      ALOGV("plane %u will not show %ux%u", index_, width, height);
      return false;
    }
  }

  return true;
}

}  // namespace android::drm_hwcomposer
