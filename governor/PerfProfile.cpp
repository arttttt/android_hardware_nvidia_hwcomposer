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

#include "governor/PerfProfile.h"

#include <stdlib.h>

#include <cutils/properties.h>

namespace android::hwc::governor::tegra {

int PerfProfile::Read(int64_t now_ns, int64_t poll_ns) {
  if (read_ns_ != 0 && now_ns - read_ns_ < poll_ns)
    return profile_;

  read_ns_ = now_ns;
  char value[PROPERTY_VALUE_MAX] = {};
  profile_ = property_get("sys.perf.profile", value, "") > 0 ? atoi(value)
                                                             : -1;
  return profile_;
}

}  // namespace android::hwc::governor::tegra
