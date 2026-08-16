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

#include "tegra/CursorPlane.h"

#include <atomic>
#include <cmath>

#include <drm_fourcc.h>

#include "compositor/LayerData.h"
#include "tegra/CursorUnit.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {
std::atomic<uint64_t> asked{0};
std::atomic<uint64_t> taken{0};
std::atomic<uint32_t> last_reason{0};
std::atomic<uint32_t> seen_format{0};
}  // namespace

uint32_t TegraCursorPlane::LastRefusal() {
  return last_reason.load(std::memory_order_relaxed);
}

uint32_t TegraCursorPlane::SeenFormat() {
  return seen_format.load(std::memory_order_relaxed);
}

uint64_t TegraCursorPlane::Asked() {
  return asked.load(std::memory_order_relaxed);
}

uint64_t TegraCursorPlane::Taken() {
  return taken.load(std::memory_order_relaxed);
}

bool TegraCursorPlane::IsValidForLayer(const LayerData *layer) {
  asked.fetch_add(1, std::memory_order_relaxed);
  if (layer == nullptr || !layer->bi || layer->bi->handle == nullptr) {
    last_reason.store(1, std::memory_order_relaxed);
    return false;
  }

  const BufferInfo &bi = *layer->bi;
  const PresentInfo &pi = layer->pi;
  seen_format.store(bi.format, std::memory_order_relaxed);

  /* The unit's colour is four bytes a pixel and nothing else; the sprite
   * the framework draws is exactly that. */
  if (bi.format != DRM_FORMAT_ABGR8888) {
    ALOGV("the cursor unit shows RGBA and not format 0x%x", bi.format);
    last_reason.store(2, std::memory_order_relaxed);
    return false;
  }

  /* Blended over the desktop the way the sprite was drawn --
   * premultiplied -- or plainly opaque. Coverage blending the unit does
   * not speak, and a faded cursor is not a thing the framework makes. */
  if (bi.blend_mode == BufferBlendMode::kCoverage || pi.alpha < 1.0F) {
    ALOGV("the cursor unit cannot fade a sprite");
    last_reason.store(3, std::memory_order_relaxed);
    return false;
  }

  /* Upright only: the unit places, it does not draw. */
  if (pi.transform.hflip || pi.transform.vflip || pi.transform.rotate90) {
    ALOGV("the cursor unit does not turn");
    last_reason.store(4, std::memory_order_relaxed);
    return false;
  }

  if (!pi.display_frame.i_rect) {
    ALOGV("a cursor with nowhere to be");
    last_reason.store(5, std::memory_order_relaxed);
    return false;
  }
  const auto width = pi.display_frame.i_rect->Width();
  const auto height = pi.display_frame.i_rect->Height();
  if (width <= 0 || height <= 0 ||
      static_cast<uint32_t>(width) > hwc::CursorUnit::kMaxSide ||
      static_cast<uint32_t>(height) > hwc::CursorUnit::kMaxSide) {
    ALOGV("a %dx%d sprite is not the unit's size", width, height);
    last_reason.store(6, std::memory_order_relaxed);
    return false;
  }

  /* One-to-one in size, and size alone. The framework floats a pointer
   * at half-pixel positions, which the general machinery counts as
   * phasing and would resample for; the unit's position register is an
   * integer and simply rounds, which is what every hardware cursor has
   * ever done. Judging phasing here would send the pointer back to a
   * full composed frame for half a pixel of truth nobody can see. */
  const float src_w = pi.source_crop.f_rect
                          ? pi.source_crop.f_rect->Width()
                          : static_cast<float>(bi.width);
  const float src_h = pi.source_crop.f_rect
                          ? pi.source_crop.f_rect->Height()
                          : static_cast<float>(bi.height);
  /* Within one pixel, because the frame is the half-pixel position
   * rounded outward: a sprite of forty-four at a half lands in a frame
   * of forty-five, and that is placement, not scaling. The sprite's own
   * size is what the unit is given to show. */
  if (labs(lroundf(src_w) - width) > 1 || labs(lroundf(src_h) - height) > 1) {
    ALOGV("the cursor unit does not resize %gx%g to %dx%d", src_w, src_h,
          width, height);
    last_reason.store(7, std::memory_order_relaxed);
    return false;
  }

  taken.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace android::drm_hwcomposer
