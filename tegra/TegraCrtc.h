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

#include <cstdint>

#include "display/Crtc.h"

namespace android {
namespace hwc {

/* One head of the display controller, as the part that assembles a screen.
 *
 * A head is asked two things and answers both from its number, because on
 * this controller a head has no identity beyond which one it is.
 */
class TegraCrtc : public drm_hwcomposer::Crtc {
public:
    explicit TegraCrtc(uint32_t index) : mIndex(index) {}

    uint32_t GetId() const override { return mIndex; }
    uint32_t GetIndexInResArray() const override { return mIndex; }

    /* The head ends in a colour management unit -- a degamma table, a 3x3
     * matrix, a regamma table -- and the backend programs the pipeline from
     * the frame's colour transform, so a transform no longer costs the
     * frame its hardware composition. The coefficient field's sign was
     * demonstrated on this silicon by writing -1.0 into the red row and
     * watching red die to black rather than triple: ten bits of two's
     * complement, as on this block's descendants. Offsets are taken too:
     * the uniform offset of the framework's inversion family runs as a
     * flipped regamma table, the backend degrading what it cannot represent
     * rather than refusing it -- with the skip capability claimed there is
     * nobody left to refuse to. */
    bool SupportsCtm() const override { return true; }
    bool SupportsSignedCtm() const override { return true; }
    bool SupportsCtmOffset() const override { return true; }

private:
    const uint32_t mIndex;
};

}  // namespace hwc
}  // namespace android
