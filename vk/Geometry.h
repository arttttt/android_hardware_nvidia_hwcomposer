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

#include <stddef.h>
#include <stdint.h>

namespace android::hwc::vk {

/* A layer's place, as the shader takes it: three corners of the
 * destination in Vulkan's clip space -- top-left, top-right, bottom-left --
 * and the texture coordinate at each; the fourth corner follows, the
 * mapping being affine, so the place is any parallelogram: a rectangle, or
 * a rectangle turned. Kept apart from everything Vulkan so it can be
 * checked on a workstation. */
struct Quad {
  float pos_tl[2];
  float pos_tr[2];
  float pos_bl[2];
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

/* A layer turned by any angle: the crop, in the buffer's pixels, drawn
 * where its top-left, top-right and bottom-left corners land -- `corners`,
 * x and y each, in the target's pixels. No transform: the corners already
 * say which way the crop lies. */
Quad PlaceTurnedLayer(float src_left, float src_top, float src_right, float src_bottom,
                      uint32_t buffer_width, uint32_t buffer_height, const float corners[6],
                      uint32_t target_width, uint32_t target_height);

/* A rectangle of the target's pixels, right and bottom exclusive. */
struct Box {
  int32_t left, top, right, bottom;
};

/* Does the union of `boxes` cover all of `area`? */
bool Covers(const Box &area, const Box *boxes, size_t count);

}  // namespace android::hwc::vk
