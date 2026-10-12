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

#include "utils/fd.h"

namespace android::drm_hwcomposer {

/* Where a fence stands, asked and not waited for. No fence at all is its
 * own answer, and so is one the kernel would not describe: each caller
 * reads those two its own way -- a buffer handed over without a fence is
 * ready, a present fence that cannot be asked about says nothing. */
enum class FenceState { kNone, kSignaled, kPending, kUnknown };

FenceState QueryFence(int fd);

inline FenceState QueryFence(const SharedFd &fd) {
  return fd ? QueryFence(*fd) : FenceState::kNone;
}

}  // namespace android::drm_hwcomposer
