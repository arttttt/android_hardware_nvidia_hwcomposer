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

#include "HwcLayer.h"

namespace android {
namespace hwc {

void HwcLayer::setBuffer(buffer_handle_t buffer, int acquireFence) {
    mBuffer = buffer;

    /* The framework hands over the descriptor and does not close it, so the
     * one already held has to go. A layer can be given a new buffer several
     * times before a frame is presented -- every one of those replacements
     * would otherwise leak a fence, which is exactly the failure this
     * composer exists to avoid. */
    mAcquireFence.reset(acquireFence);
}

}  // namespace hwc
}  // namespace android
