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

/* The panel's vertical blank, as a stream of timestamps.
 *
 * The framework schedules everything it does off this signal, so two
 * properties matter more than the mechanism behind them. The timestamp must
 * come from the same clock the rest of the system uses, CLOCK_MONOTONIC, or
 * frame pacing drifts against every other timer. And delivery must stop
 * promptly on `disable`: a callback arriving after the framework asked for
 * silence reaches an observer that no longer expects it.
 */
class VSyncSource {
public:
    /* Called once per vertical blank with the time it happened. Runs on the
     * source's own thread, not the caller's. */
    using Callback = std::function<void(int64_t timestampNs)>;

    virtual ~VSyncSource() = default;

    /* Starts delivery. Replaces any previously registered callback.
     * Returns 0 on success or a negative errno. */
    virtual int enable(Callback callback) = 0;

    /* Stops delivery and guarantees that no callback is running or will run
     * once it returns. */
    virtual int disable() = 0;
};

}  // namespace hwc
}  // namespace android

#endif  // DISPLAY_VSYNC_SOURCE_H
