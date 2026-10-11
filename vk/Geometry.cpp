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

#include "vk/Geometry.h"

#include <utility>

namespace android::hwc::vk {

namespace {

struct Uv {
  float u, v;
};

}  // namespace

Quad PlaceLayer(float src_left, float src_top, float src_right, float src_bottom,
                uint32_t buffer_width, uint32_t buffer_height, int32_t dst_left,
                int32_t dst_top, int32_t dst_right, int32_t dst_bottom, uint8_t transform,
                uint32_t target_width, uint32_t target_height) {
  Quad q{};
  const float tw = static_cast<float>(target_width);
  const float th = static_cast<float>(target_height);
  q.ndc_left = 2.f * static_cast<float>(dst_left) / tw - 1.f;
  q.ndc_right = 2.f * static_cast<float>(dst_right) / tw - 1.f;
  q.ndc_top = 2.f * static_cast<float>(dst_top) / th - 1.f;
  q.ndc_bottom = 2.f * static_cast<float>(dst_bottom) / th - 1.f;

  const float bw = static_cast<float>(buffer_width);
  const float bh = static_cast<float>(buffer_height);
  const float u0 = src_left / bw, u1 = src_right / bw;
  const float v0 = src_top / bh, v1 = src_bottom / bh;

  /* The crop's corners clockwise from the top-left; each operation says
   * which source corner lands on each destination corner. */
  Uv c[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}; /* TL TR BR BL */
  if (transform & 1) { /* mirrored left to right */
    std::swap(c[0], c[1]);
    std::swap(c[3], c[2]);
  }
  if (transform & 2) { /* mirrored top to bottom */
    std::swap(c[0], c[3]);
    std::swap(c[1], c[2]);
  }
  if (transform & 4) { /* a quarter turn clockwise: what was bottom-left
                        * is now top-left, and round */
    const Uv tl = c[3], tr = c[0], br = c[1], bl = c[2];
    c[0] = tl;
    c[1] = tr;
    c[2] = br;
    c[3] = bl;
  }
  q.uv_tl[0] = c[0].u;
  q.uv_tl[1] = c[0].v;
  q.uv_tr[0] = c[1].u;
  q.uv_tr[1] = c[1].v;
  q.uv_bl[0] = c[3].u;
  q.uv_bl[1] = c[3].v;
  return q;
}

}  // namespace android::hwc::vk
