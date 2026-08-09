/*
 * Copyright (C) 2015 - 2023 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's drm/DrmFbImporter.h, where this is called
 * IDrmFbIdHandle. Renamed: nothing in this composer speaks to a display
 * through DRM, and a name that says otherwise would send every later reader
 * looking for something that is not here. The four places that referred to it
 * were changed with it.
 */

#pragma once

#include <cstdint>

namespace android::drm_hwcomposer {

/* What a display accepts in place of a buffer.
 *
 * A display does not take memory, it takes a number that stands for memory,
 * and getting one usually means asking the driver to remember the buffer
 * first. Holding this is what keeps that arrangement alive: let go of it
 * while the buffer is still on screen and the picture blinks.
 *
 * On this controller the number is the buffer's own descriptor and there is
 * nothing to arrange, but the promise is the same one -- hold it while the
 * frame is up -- and stating it here keeps the code above indifferent to
 * which kind of display it is talking to.
 */
class FbIdHandle {
 public:
  FbIdHandle() = default;
  virtual ~FbIdHandle() = default;
  FbIdHandle(FbIdHandle &&) = delete;
  FbIdHandle(const FbIdHandle &) = delete;
  auto operator=(const FbIdHandle &) = delete;
  auto operator=(FbIdHandle &&) = delete;

  virtual auto GetFbId [[nodiscard]] () const -> uint32_t = 0;
};

}  // namespace android::drm_hwcomposer
