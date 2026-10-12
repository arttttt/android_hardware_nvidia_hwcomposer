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

#include "utils/fence.h"

#include <sync/sync.h>

namespace android::drm_hwcomposer {

FenceState QueryFence(int fd) {
  if (fd < 0)
    return FenceState::kNone;
  struct sync_file_info *info = sync_file_info(fd);
  if (info == nullptr)
    return FenceState::kUnknown;
  const FenceState state = info->status == 1 ? FenceState::kSignaled
                                             : FenceState::kPending;
  sync_file_info_free(info);
  return state;
}

}  // namespace android::drm_hwcomposer
