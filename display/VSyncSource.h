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

#ifndef DISPLAY_VSYNC_SOURCE_H
#define DISPLAY_VSYNC_SOURCE_H

#include <cstdint>
#include <functional>

namespace android {
namespace hwc {

/* One wait for the panel's next vertical blank.
 *
 * Deliberately no more than that. Everything else a composer does with
 * blanks -- tracking the period, predicting when the next one falls,
 * correcting the estimate against the fence of the frame just shown,
 * starting and stopping delivery -- is the same whatever the hardware, and
 * upstream has all of it in VSyncWorker. This is the one step of that which
 * cannot be written once for everybody, so it is the only step here.
 *
 * The timestamp must come from the same clock the rest of the system uses,
 * CLOCK_MONOTONIC, or frame pacing drifts against every other timer.
 *
 * Called on the worker's thread and expected to block. Returning an error is
 * a normal outcome rather than a fault -- a panel that has been powered down
 * reports nothing -- and the caller answers it by falling back to a timer,
 * so an implementation must return rather than block for ever.
 */
class VSyncSource {
public:
    virtual ~VSyncSource() = default;

    /* Waits for the next vertical blank and reports when it happened.
     * Returns 0, or a negative errno if no blank was seen. */
    virtual int waitForVSync(int64_t *outTimestampNs) = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_VSYNC_SOURCE_H
