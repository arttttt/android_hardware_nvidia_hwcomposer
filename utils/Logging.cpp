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

#include "utils/Logging.h"

#include <cutils/properties.h>

namespace android {
namespace hwc {

bool TracingWanted() {
    /* Once. The trace it guards runs several times a frame, and a question
     * asked sixty times a second is a cost of its own however small the
     * answer -- which is the whole reason this switch exists. */
    static const bool wanted = property_get_bool("vendor.hwc.trace", 0) != 0;
    return wanted;
}

}  // namespace hwc
}  // namespace android
