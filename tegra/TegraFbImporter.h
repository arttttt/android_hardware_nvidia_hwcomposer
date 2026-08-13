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

#include <inttypes.h>
#include <memory>
#include <unistd.h>

#include <map>

#include <cutils/properties.h>

#include "bufferinfo/BufferInfo.h"
#include "display/FbImporter.h"
#include "tegra/TegraFbIdHandle.h"
#include "utils/fd.h"
#include "utils/log.h"

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

    /* Got before it is created, which is what the name promises.
     *
     * Recognised by the buffer, which is how Samsung and Intel both do it --
     * Samsung keys a framebuffer cache by buffer id, format and secure flag,
     * Intel by the handle it re-imports. Neither keys on the address of
     * something that describes the buffer, and this used to: the object it
     * looked at is built afresh every time a buffer is described, so the key
     * was new on every frame and the cache never once hit. A copy of the
     * descriptor was made for every layer of every frame.
     *
     * A buffer that cannot say which one it is stays out of the cache rather
     * than joining every other such buffer under nought.
     */
    const uint64_t key = bo->unique_id;
    if (key != 0) {
      auto it = fbs_.find(key);
      if (it != fbs_.end()) {
        if (auto held = it->second.lock()) {
          /* Said only when asked for, and then about every buffer of every
           * frame -- this is a question about what the cache is actually
           * doing, and the answer is only useful in full. */
          if (diagnose_)
            ALOGD("buf %" PRIu64 " hit: fd %d (cached %d) %ux%u pitch %u "
                  "offset %u mod %" PRIx64,
                  key, bo->prime_fds[0],
                  static_cast<int>(held->GetFbId()), bo->width, bo->height,
                  bo->pitches[0], bo->offsets[0], bo->modifiers[0]);
          return held;
        }

        /* Nothing holds it any more, so the buffer it belonged to is gone.
         * The identity may since have been reissued to another. */
        fbs_.erase(it);
      }
    }

    /* Duplicated rather than taken: the description this came from does not
     * own its descriptor and may be read again for another frame. */
    const int fd = ::dup(bo->prime_fds[0]);
    if (fd < 0)
      return {};

    auto fb = std::make_shared<TegraFbIdHandle>(MakeSharedFd(fd));

    if (diagnose_)
      ALOGD("buf %" PRIu64 " miss: fd %d -> %d, %ux%u pitch %u offset %u "
            "mod %" PRIx64 ", handle %p",
            key, bo->prime_fds[0], fd, bo->width, bo->height, bo->pitches[0],
            bo->offsets[0], bo->modifiers[0], bo->handle);

    if (key != 0)
      fbs_[key] = fb;

    return fb;
  }

 private:
  /* Weakly, so that a buffer nobody is showing any more takes its copy of the
   * descriptor with it. What is left behind is an entry that will not lock,
   * which is removed when its identity comes round again. */
  std::map<uint64_t, std::weak_ptr<FbIdHandle>> fbs_;

  /* Says what every buffer of every frame did here. Off unless asked for --
   * it is a question, not a running commentary. */
  const bool diagnose_ = property_get_bool("vendor.hwc.bufdiag", 0) != 0;
};

}  // namespace android::drm_hwcomposer
