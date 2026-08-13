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

#include <memory>
#include <unistd.h>

#include "bufferinfo/BufferInfo.h"
#include "display/FbImporter.h"
#include "tegra/TegraFbIdHandle.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

/* Nothing is imported, because there is nothing to import into.
 *
 * On a DRM display a buffer must be registered with the driver before a frame
 * can name it, and the registration is expensive enough that upstream caches
 * it and tracks its lifetime. This controller takes the descriptor itself: a
 * flip names the memory and the driver resolves it there and then.
 *
 * So the whole of this is a copy of the descriptor, kept alive for as long as
 * anyone might still be showing what it points at. The copy matters -- the
 * one in the buffer description is borrowed and outlives nothing.
 */
class TegraFbImporter : public FbImporter {
 public:
  auto GetOrCreateFbId(BufferInfo *bo) -> std::shared_ptr<FbIdHandle> override {
    if (bo == nullptr || bo->prime_fds[0] < 0)
      return {};

    /* A copy of the descriptor, per frame.
     *
     * Keying this on the buffer's identity so that the same copy could be
     * handed back was tried and reverted: it takes hold of what the hardware
     * scans out, and got it wrong -- the picture came apart into rows within
     * seconds and SurfaceFlinger fell over. What is wrong about it is not yet
     * understood, and until it is, the cheap thing is not worth the display.
     *
     * The saving that was after lives one level up instead, in what the
     * allocator is asked about a buffer, which does not touch the hardware at
     * all. See BufferInfoNvidia.
     */
    const int fd = ::dup(bo->prime_fds[0]);
    if (fd < 0)
      return {};

    return std::make_shared<TegraFbIdHandle>(MakeSharedFd(fd));
  }
};

}  // namespace android::drm_hwcomposer
