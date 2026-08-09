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

#ifndef HWC_HWC_LAYER_H
#define HWC_HWC_LAYER_H

#include <cstdint>

#include <cutils/native_handle.h>

#include "display/FramePlan.h"
#include "display/Geometry.h"
#include "utils/UniqueFd.h"

namespace android {
namespace hwc {

/* What the framework has told us about one layer.
 *
 * A layer is set up field by field, over many calls, and only read when the
 * frame is validated. So this is a record with setters, not an object with
 * behaviour: the composition decision belongs to whatever plans the frame,
 * and putting any of it here would scatter that decision across as many
 * places as there are layers.
 *
 * The one piece of behaviour that does belong here is the acquire fence,
 * because it is the only field the layer owns rather than copies.
 */
class HwcLayer {
public:
    /* What the composer decided to do with this layer. Client means the
     * framework must draw it into the target buffer itself; device means the
     * display hardware will show it. */
    enum class Composition {
        Invalid,
        Client,
        Device,
    };

    void setBuffer(buffer_handle_t buffer, int acquireFence);

    void setSourceCrop(const FRect &crop) { mSourceCrop = crop; }
    void setDisplayFrame(const Rect &frame) { mDisplayFrame = frame; }
    void setBlendMode(BlendMode blend) { mBlend = blend; }
    void setPlaneAlpha(float alpha) { mAlpha = alpha; }
    void setTransform(int32_t transform) { mTransform = transform; }
    void setZOrder(uint32_t z) { mZOrder = z; }

    /* What the framework asked for, and what we answered. They differ only
     * between validate and accept, which is precisely the window in which
     * the framework is told what changed. */
    void setRequestedComposition(Composition type) { mRequested = type; }
    Composition requestedComposition() const { return mRequested; }

    void setActualComposition(Composition type) { mActual = type; }
    Composition actualComposition() const { return mActual; }

    buffer_handle_t buffer() const { return mBuffer; }
    const FRect &sourceCrop() const { return mSourceCrop; }
    const Rect &displayFrame() const { return mDisplayFrame; }
    BlendMode blendMode() const { return mBlend; }
    float planeAlpha() const { return mAlpha; }
    int32_t transform() const { return mTransform; }
    uint32_t zOrder() const { return mZOrder; }

    /* Borrowed for the duration of the frame. The layer keeps ownership so
     * that a plan can name the fence without inheriting the duty to close
     * it, and so that a layer replaced mid-frame cannot leave a descriptor
     * behind. -1 when the buffer needs no wait. */
    int acquireFence() const { return mAcquireFence.get(); }

    /* Handed to the framework once the frame is on screen, telling it when
     * this layer's buffer may be reused. Ownership passes to the caller. */
    void setReleaseFence(UniqueFd fence) { mReleaseFence = std::move(fence); }
    UniqueFd takeReleaseFence() { return std::move(mReleaseFence); }

private:
    buffer_handle_t mBuffer = nullptr;
    UniqueFd mAcquireFence;
    UniqueFd mReleaseFence;

    FRect mSourceCrop;
    Rect mDisplayFrame;
    BlendMode mBlend = BlendMode::None;
    float mAlpha = 1.0f;
    int32_t mTransform = 0;
    uint32_t mZOrder = 0;

    Composition mRequested = Composition::Invalid;
    Composition mActual = Composition::Invalid;
};

}  // namespace hwc
}  // namespace android

#endif  // HWC_HWC_LAYER_H
