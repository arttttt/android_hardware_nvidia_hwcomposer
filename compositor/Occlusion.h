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

#include <optional>
#include <vector>

#include "compositor/Rect.h"

namespace android::drm_hwcomposer::occlusion {

/* What of each layer can be seen once everything above it is in place.
 *
 * A layer under an opaque one is read from memory for nothing: by a display
 * window, by the image engine for a merge, by the GPU for the client range
 * -- sometimes by two of them in one frame. SurfaceFlinger takes part of
 * this away itself: it never sends a layer that an opaque one covers
 * whole, and it sends each layer's visible region. But it counts a layer
 * with rounded corners as not opaque, and on this device every app window
 * an animation moves has them; it is the composer that draws the corners,
 * and the composer knows that inside them the layer is as opaque as any.
 *
 * So the composer works it out again, with SurfaceFlinger's region as the
 * starting point: a layer hidden whole is not shown at all, and a layer
 * seen only in part is shown cropped to the bounds of that part. Bounds
 * rather than the exact region -- every reader takes one rectangle. */

/* One layer, in the display's space, bottom of the stack first. */
struct Layer {
  /* Where the layer lands. */
  IRect frame{};

  /* SurfaceFlinger's visible region for it; empty for the whole frame,
   * which is also what a layer SurfaceFlinger has not described yet gets. */
  std::vector<IRect> visible;

  /* The layer's pixels cover its frame whole: no alpha in the buffer that
   * blending reads, full plane alpha. Only an opaque layer hides others. */
  bool opaque = false;

  /* Rounded corners, when the radius is above nought: the crop they round,
   * in the display's space. Inside it, away from the corners, an opaque
   * layer is still opaque. */
  IRect corner_crop{};
  float radius = 0.F;

  /* Never hidden and never cropped: something of it reaches past its frame
   * -- a shadow, a stretch -- or it is not to be touched, like a cursor. It
   * still hides what is under it when it is opaque. */
  bool keep = false;
};

struct Shown {
  /* Nothing of the layer can be seen. */
  bool hidden = false;

  /* The bounds of what can; the frame itself when all of it can. */
  IRect bounds{};
};

/* For each layer, in the order given: what of it can be seen. */
std::vector<Shown> Resolve(const std::vector<Layer> &bottom_first);

/* The part of `source` that lands in `shown`, for a layer drawn from
 * `source` into `frame` -- flipped first, as HWC2 means it. Nothing for a
 * quarter turn, which is shown uncropped, or for a `shown` that is not
 * inside `frame`. */
std::optional<FRect> CropSource(const FRect &source, const IRect &frame,
                                const IRect &shown, bool hflip, bool vflip,
                                bool rotate90);

}  // namespace android::drm_hwcomposer::occlusion
