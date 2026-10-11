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

#pragma once

#include <stdint.h>

namespace android::hwc::vk {

/* A layer's place, as the shader takes it: the destination rectangle in
 * Vulkan's clip space, and the texture coordinate at three of its corners
 * -- top-left, top-right, bottom-left; the fourth follows, the mapping
 * being affine. Kept apart from everything Vulkan so it can be checked on
 * a workstation. */
struct Quad {
  float ndc_left, ndc_top, ndc_right, ndc_bottom;
  float uv_tl[2];
  float uv_tr[2];
  float uv_bl[2];
};

/* `transform` is hflip | vflip << 1 | rotate90 << 2, flips first, then the
 * quarter turn clockwise -- HWC2's meaning. The crop is in the buffer's
 * pixels, the destination in the target's; Vulkan's clip space runs -1 at
 * the target's top-left to +1 at its bottom-right. */
Quad PlaceLayer(float src_left, float src_top, float src_right, float src_bottom,
                uint32_t buffer_width, uint32_t buffer_height, int32_t dst_left,
                int32_t dst_top, int32_t dst_right, int32_t dst_bottom, uint8_t transform,
                uint32_t target_width, uint32_t target_height);

}  // namespace android::hwc::vk
