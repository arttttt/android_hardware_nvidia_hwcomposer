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

#include <drm_fourcc.h>

#include "compositor/LayerData.h"
#include "tegra/CursorUnit.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

bool TegraCursorPlane::IsValidForLayer(const LayerData *layer) {
  if (layer == nullptr || !layer->bi || layer->bi->handle == nullptr)
    return false;

  const BufferInfo &bi = *layer->bi;
  const PresentInfo &pi = layer->pi;

  /* The unit's colour is four bytes a pixel and nothing else; the sprite
   * the framework draws is exactly that. */
  if (bi.format != DRM_FORMAT_ABGR8888) {
    ALOGV("the cursor unit shows RGBA and not format 0x%x", bi.format);
    return false;
  }

  /* Blended over the desktop the way the sprite was drawn --
   * premultiplied -- or plainly opaque. Coverage blending the unit does
   * not speak, and a faded cursor is not a thing the framework makes. */
  if (bi.blend_mode == BufferBlendMode::kCoverage || pi.alpha < 1.0F) {
    ALOGV("the cursor unit cannot fade a sprite");
    return false;
  }

  /* One-to-one, upright, whole pixels: the unit places, it does not
   * draw. */
  if (pi.transform.hflip || pi.transform.vflip || pi.transform.rotate90) {
    ALOGV("the cursor unit does not turn");
    return false;
  }
  if (pi.RequireScalingOrPhasing()) {
    ALOGV("the cursor unit does not resize");
    return false;
  }

  if (!pi.display_frame.i_rect) {
    ALOGV("a cursor with nowhere to be");
    return false;
  }
  const auto width = pi.display_frame.i_rect->Width();
  const auto height = pi.display_frame.i_rect->Height();
  if (width <= 0 || height <= 0 ||
      static_cast<uint32_t>(width) > hwc::CursorUnit::kMaxSide ||
      static_cast<uint32_t>(height) > hwc::CursorUnit::kMaxSide) {
    ALOGV("a %dx%d sprite is not the unit's size", width, height);
    return false;
  }

  return true;
}

}  // namespace android::drm_hwcomposer
