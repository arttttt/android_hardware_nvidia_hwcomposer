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
 *
 * The colour attributes are left at the base class's answer -- absent. That
 * one is temporary rather than permanent: this controller has a colour
 * management unit, a matrix and two lookup tables, which is precisely what
 * those attributes describe. Nothing fills them in yet.
 */
class TegraCrtc : public drm_hwcomposer::Crtc {
public:
    explicit TegraCrtc(uint32_t index) : mIndex(index) {}

    uint32_t GetId() const override { return mIndex; }
    uint32_t GetIndexInResArray() const override { return mIndex; }

private:
    const uint32_t mIndex;
};

}  // namespace hwc
}  // namespace android
