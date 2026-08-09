/*
 * Copyright (C) 2023 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's drm/DrmFbImporter.h.
 *
 * Turns a described buffer into whatever this hardware wants named in a
 * frame. On a DRM display that is a registration -- the buffer is handed to
 * the driver, which returns an identifier and remembers the association until
 * it is told to forget. Elsewhere it may be nothing of the sort.
 *
 * Only the interface is here. Their implementation caches registrations and
 * tracks their lifetime, because registering is expensive and forgetting too
 * early breaks a frame still on screen; where there is nothing to register,
 * there is nothing to cache either.
 */

#pragma once

#include <memory>

#include "display/FbIdHandle.h"

namespace android::drm_hwcomposer {

struct BufferInfo;

class FbImporter {
 public:
  virtual ~FbImporter() = default;

  virtual auto GetOrCreateFbId(BufferInfo *bo)
      -> std::shared_ptr<FbIdHandle> = 0;
};

}  // namespace android::drm_hwcomposer
