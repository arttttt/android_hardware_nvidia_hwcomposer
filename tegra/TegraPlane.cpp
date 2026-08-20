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

#include <utility>

#include <cutils/properties.h>
#include <tegra_dc_ext.h>

#include "compositor/LayerData.h"
#include "tegra/TegraFormat.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

std::atomic<uint64_t> TegraPlane::transform_refusals_{0};
std::atomic<uint64_t> TegraPlane::scale_refusals_{0};
uint32_t TegraPlane::turn_reach_{0};

bool TegraPlane::BeyondEngineReach(float src_w, float src_h, float dst_w,
                                   float dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
    return false;
  const float ratio_w = src_w > dst_w ? src_w / dst_w : dst_w / src_w;
  const float ratio_h = src_h > dst_h ? src_h / dst_h : dst_h / src_h;
  return ratio_w > kEngineScaleReach || ratio_h > kEngineScaleReach;
}

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

    /* The geometry the merge will actually draw, missing rectangles
     * filled the way the merge itself fills them -- a whole buffer to a
     * whole buffer. Judging anything narrower leaves a layer the merge
     * would resize slipping past the judge unmeasured. */
    float src_w = pi.source_crop.f_rect ? pi.source_crop.f_rect->Width()
                                        : static_cast<float>(bi.width);
    float src_h = pi.source_crop.f_rect ? pi.source_crop.f_rect->Height()
                                        : static_cast<float>(bi.height);
    if (pi.transform.rotate90)
      std::swap(src_w, src_h);
    const float dst_w = pi.display_frame.i_rect
                            ? static_cast<float>(
                                  pi.display_frame.i_rect->Width())
                            : static_cast<float>(bi.width);
    const float dst_h = pi.display_frame.i_rect
                            ? static_cast<float>(
                                  pi.display_frame.i_rect->Height())
                            : static_cast<float>(bi.height);

    if (pi.transform.hflip || pi.transform.vflip || pi.transform.rotate90) {
      static const bool turn_in_merge =
          property_get_bool("vendor.hwc.merge.rotate", 1) != 0;
      /* A turned member is taken now: it is drawn turned into an
       * intermediate by a pass of the engine's own before the group
       * composes, the way the stock composer turned layers on the way
       * into its scratch. The switch puts the refusal back -- turned
       * layers go to a window that turns them, or the GPU -- for an A/B
       * on one binary; the counter keeps the tally of what the refusal
       * costs. */
      if (!turn_in_merge) {
        transform_refusals_.fetch_add(1, std::memory_order_relaxed);
        ALOGV("plane %u: the engine will not turn a layer", index_);
        return false;
      }

      /* A crop collapsed to nothing cannot be turned into anything --
       * the stock blit skipped such layers on the same strict
       * comparison. Nothing upstream ever sends one; should one arrive,
       * it goes where whole layers go, not into an ask for a zero-sized
       * copy. */
      if (src_w <= 0 || src_h <= 0) {
        ALOGV("plane %u: nothing to turn in a %gx%g crop", index_, src_w,
              src_h);
        return false;
      }

      /* And a turned copy has to land somewhere: the intermediates are
       * cut no larger than the reach says, and a copy that fits none of
       * them is a group refused at execute time, every frame, which no
       * ladder walks back. Windows and the GPU turn such a layer
       * natively -- it goes to them. */
      if (turn_reach_ != 0 && (src_w > static_cast<float>(turn_reach_) ||
                               src_h > static_cast<float>(turn_reach_))) {
        scale_refusals_.fetch_add(1, std::memory_order_relaxed);
        ALOGV("plane %u: no intermediate holds a turned %gx%g", index_,
              src_w, src_h);
        return false;
      }
    }

    /* The engine's reach on resizing, judged here and not at execute time,
     * because the two refusals are not the same kind of failure. A layer
     * refused here falls to the next plane and the plan is re-weighed --
     * the ordinary path. A set refused by the engine at execute time is a
     * frame already promised and now dropped, and the next plan is the
     * same plan: the refusal repeats every frame for as long as the scene
     * stands. Measured against the turned axes, since the turned copy is
     * what the group will resize. Not gated on the turning switch -- the
     * reach binds turned and straight members alike. */
    if (BeyondEngineReach(src_w, src_h, dst_w, dst_h)) {
      scale_refusals_.fetch_add(1, std::memory_order_relaxed);
      ALOGV("plane %u: the engine will not resize %gx%g to %gx%g", index_,
            src_w, src_h, dst_w, dst_h);
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

  /* First of the geometry checks, and the order is load-bearing: this is
   * what keeps every resizing layer -- translucent ones included, since
   * the stock pairing refusal was retired -- out of windows that have no
   * filter to resize with. Anything moved below it must not let a scaled
   * layer reach a filterless window first. */
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
   * There used to be a second rule here, bug 1515812 out of the stock
   * composer: "dc does not filter alpha channel", so a translucent layer
   * that also resized was refused outright, and every fading animation
   * paid the graphics core for it. Nobody had re-examined that word since
   * 2013 -- so it was put to the panel. The register map has no bit that
   * would starve the filter of alpha; upstream's driver has scaled blended
   * planes on four generations without a word of this; and on this very
   * device the pairing went to a window carrying worst-case content --
   * single-pixel stripes under a tenfold-slowed fade -- and the eye found
   * nothing the GPU reference did not also show. The refusal died of the
   * evidence. If seams ever do surface on some content this test missed,
   * the vendor's own remedy is on record: a half-pixel inset of the crop
   * when the filter runs, as their blit library does. */
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

    }
  }

  if ((pi.transform.hflip && !caps_.invertH) ||
      (pi.transform.vflip && !caps_.invertV) ||
      (pi.transform.rotate90 && !caps_.scanColumn)) {
    ALOGV("plane %u cannot turn the layer that way", index_);
    return false;
  }

  /* A quarter turn reads the surface column-first, which demands a
   * block-encoded surface. The kernel does not check this on the show
   * path -- only the window's capabilities are checked there, and no
   * layout check lives in it -- so a pitch-linear layer asked to turn
   * would reach the flip and fall there. Refused here, where refusal
   * still steers the layer down the ladder. Our own turned intermediate
   * is pitch-linear by construction and must never reach a window for
   * this reason. */
  if (pi.transform.rotate90 &&
      (flags & TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR) == 0) {
    ALOGV("plane %u: a quarter turn needs a block-encoded surface", index_);
    return false;
  }

  /* And only unscaled layers may be turned on a window. The kernel's
   * scale check compares buffer width to on-screen width, and under a
   * turn those are different axes -- a legitimate turn of a wide buffer
   * would trip that rule and be refused. So a layer that both turns and
   * resizes is refused here instead, where the ladder still has somewhere
   * to send it. Temporary; lifted on field data.
   *
   * The stock composer also capped the source's height at 2560 in the
   * same path; our panel is 1536x2048, so a source is already shorter and
   * that check would be unreachable -- left out. */
  const bool turned =
      pi.transform.hflip || pi.transform.vflip || pi.transform.rotate90;
  const bool scaled = pi.source_crop.f_rect && pi.display_frame.i_rect &&
                      (pi.source_crop.f_rect->Width() !=
                           static_cast<float>(
                               pi.display_frame.i_rect->Width()) ||
                       pi.source_crop.f_rect->Height() !=
                           static_cast<float>(
                               pi.display_frame.i_rect->Height()));
  if (turned && scaled) {
    ALOGV("plane %u: a turned layer cannot also resize", index_);
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
