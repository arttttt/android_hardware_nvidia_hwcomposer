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
#include <utility>

#include "display/FbIdHandle.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

/* What this controller accepts in place of a buffer: the buffer itself.
 *
 * There is nothing to register and nothing to remember -- a flip names the
 * memory by its descriptor, and the driver resolves it there and then. So the
 * whole of this is a descriptor kept alive for as long as anyone might still
 * be showing what it points at, which is exactly the promise the interface
 * asks for and the only reason this object exists rather than a bare number.
 *
 * The descriptor is shared rather than owned outright: the same buffer can be
 * on screen and queued for the next frame at once, and whoever lets go last
 * closes it.
 */
class TegraFbIdHandle : public FbIdHandle {
 public:
  explicit TegraFbIdHandle(SharedFd fd) : fd_(std::move(fd)) {
  }

  auto GetFbId [[nodiscard]] () const -> uint32_t override {
    return fd_ ? static_cast<uint32_t>(*fd_) : 0;
  }

 private:
  const SharedFd fd_;
};

}  // namespace android::drm_hwcomposer
