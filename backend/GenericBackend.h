/*
 * Copyright (C) 2025 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's backend/GenericBackend.h.
 *
 * The part of a backend that is not about any particular hardware: how a
 * buffer is to be read, and which planner to install.
 *
 * Upstream builds the pipeline here too, generically, out of the DRM objects
 * a card reports. There is nothing generic about assembling one here -- what
 * a pipeline is made of is the whole of what a backend knows about its
 * hardware -- so that one and the commit sink are left to whoever inherits
 * this, and what remains is the two decisions that are the same everywhere.
 */

#pragma once

#include <memory>

#include "backend/Backend.h"
#include "compositor/CompositionPlanner.h"

namespace android::drm_hwcomposer {

class BufferInfoGetter;
class Device;

class GenericBackend : public Backend {
 public:
  explicit GenericBackend(Device &device);

  // Create a default BufferInfoGetter. By default this will create a
  // BufferInfoMapperMetadata, and fall back to LegacyBufferInfoGetter if that
  // fails.
  std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter() override;

 protected:
  // Create a new GenericCompositionPlanner. Subclasses can override
  // this to create different composition planners.
  virtual std::unique_ptr<CompositionPlanner> CreateCompositionPlanner();
};

}  // namespace android::drm_hwcomposer
