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

#include <stdint.h>

namespace android::hwc::governor::tegra {

/* The performance profile the power service publishes as sys.perf.profile,
 * read at most once per polling interval: a property read is a lookup in
 * shared memory, but one per frame is still one per frame. */
class PerfProfile {
 public:
  /* The power service's number for saving power. */
  static constexpr int powerSave = 0;

  /* The profile, or -1 if the property is unset or unreadable. */
  int Read(int64_t now_ns, int64_t poll_ns);

 private:
  int64_t read_ns_ = 0;
  int profile_ = -1;
};

}  // namespace android::hwc::governor::tegra
