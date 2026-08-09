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

#include <map>

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

    /* Got before it is created, which is what the name has always promised
     * and what this did not do: it made a fresh copy of the descriptor for
     * every layer of every frame, and a layer showing the same buffer for a
     * second is a hundred of them.
     *
     * Recognised by the object holding the buffer alive alongside its
     * description, rather than by the descriptor number -- numbers are handed
     * out again after they are closed, and the same number twice would be two
     * different buffers. That object is one per buffer and lives exactly as
     * long as the description does.
     */
    const auto *key = bo->fds_shared.get();
    if (key != nullptr) {
      auto it = fbs_.find(key);
      if (it != fbs_.end()) {
        if (auto held = it->second.lock())
          return held;

        /* The buffer went away and took its copy with it. Whatever is at
         * this address now is something else. */
        fbs_.erase(it);
      }
    }

    /* Duplicated rather than taken: the description this came from does not
     * own its descriptor and may be read again for another frame. */
    const int fd = ::dup(bo->prime_fds[0]);
    if (fd < 0)
      return {};

    auto fb = std::make_shared<TegraFbIdHandle>(MakeSharedFd(fd));

    if (key != nullptr)
      fbs_[key] = fb;

    return fb;
  }

 private:
  /* Weakly, so that a buffer nobody is showing any more takes its copy of the
   * descriptor with it. What is left behind is an entry that will not lock,
   * which is removed when its address comes round again. */
  std::map<const void *, std::weak_ptr<FbIdHandle>> fbs_;
};

}  // namespace android::drm_hwcomposer
