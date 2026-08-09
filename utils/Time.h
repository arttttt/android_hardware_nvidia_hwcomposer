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
#include <ctime>

namespace android::drm_hwcomposer {

/* The clock everything in a composer is timed against.
 *
 * Upstream keeps this as a static on ResourceManager, which is otherwise
 * entirely about enumerating DRM resources -- so it is the one part of that
 * class this composer needs and the only part that has nothing to do with
 * DRM. It is here instead, and their call sites lose the class name.
 *
 * CLOCK_MONOTONIC and nothing else: the framework paces frames against it,
 * and a timestamp from any other clock drifts against every timer in the
 * system.
 */
inline int64_t GetTimeMonotonicNs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);

  constexpr int64_t kNsInSec = 1000000000LL;
  return (int64_t(ts.tv_sec) * kNsInSec) + int64_t(ts.tv_nsec);
}

}  // namespace android::drm_hwcomposer
