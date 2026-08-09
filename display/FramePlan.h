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

#ifndef DISPLAY_FRAME_PLAN_H
#define DISPLAY_FRAME_PLAN_H

#include <cstdint>
#include <vector>

#include <cutils/native_handle.h>

#include "Geometry.h"

namespace android {
namespace hwc {

/* How a source buffer is combined with what is already on screen. */
enum class BlendMode {
    None,          /* opaque; alpha channel ignored */
    Premultiplied, /* colour has already been multiplied by alpha */
    Coverage,      /* colour has not */
};

/* One source buffer and where it lands on screen. */
struct PlannedLayer {
    buffer_handle_t buffer = nullptr;

    /* Borrowed, never closed here. The layer that produced this entry owns
     * the fence and outlives the plan. The compositor either hands it to the
     * hardware or waits on it, and does neither afterwards. -1 means the
     * buffer is ready now. */
    int acquireFence = -1;

    FRect sourceCrop;    /* region of the buffer to read */
    Rect displayFrame;   /* where it goes on the panel */

    BlendMode blend = BlendMode::None;
    float alpha = 1.0f;  /* plane alpha, applied on top of the blend mode */

    /* Rotations and flips, as the framework's transform bits. Zero is the
     * identity, which is all the composer currently ever produces. */
    int32_t transform = 0;
};

/* Everything the display needs in order to show one frame.
 *
 * Immutable once built, and free of any notion of how the hardware will
 * satisfy it: no window indices, no plane assignments, no format codes. It
 * says what should appear, and the backend decides how. Keeping it that way
 * is what lets the same plan be tested for feasibility and then executed
 * without a second construction pass.
 *
 * A frame planned entirely by the client arrives here as a single layer
 * holding the client target. That is not a special case in this structure,
 * which is the point: when overlays land, the same plan simply carries more
 * entries.
 */
class FramePlan {
public:
    FramePlan() = default;

    void addLayer(const PlannedLayer &layer) { mLayers.push_back(layer); }

    const std::vector<PlannedLayer> &layers() const { return mLayers; }
    bool isEmpty() const { return mLayers.empty(); }
    size_t layerCount() const { return mLayers.size(); }

private:
    /* Bottom first, in the order they should be composited. */
    std::vector<PlannedLayer> mLayers;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_FRAME_PLAN_H
