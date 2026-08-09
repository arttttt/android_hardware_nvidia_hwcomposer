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

/* Adapted from drm-hwcomposer's backend/ClientBackend.cpp.
 *
 * A composer that puts every layer through the client. Upstream ships it for
 * hardware whose planes are not worth planning around; here it is the thing
 * hardware composition gets measured against, and it is reached the same way
 * -- by name, from a property, with nothing rebuilt.
 *
 * Changed in one word: what it inherits from. Upstream's generic backend
 * assembles a pipeline out of DRM objects, and this one exists only to swap
 * the planner, so it inherits from whichever backend does know how to
 * assemble one here.
 */

#include <memory>

#include "backend/BackendManager.h"
#include "compositor/CompositionPlanner.h"
#include "compositor/ICompositorDisplay.h"
#include "display/Device.h"
#include "tegra/TegraBackend.h"

namespace android::drm_hwcomposer {
namespace {
class ClientCompositionPlanner : public CompositionPlanner {
 public:
  auto ValidateDisplay(const ICompositorDisplay* display)
      -> ValidationResult override {
    return {.composition = GetFlattenedComposition(display
                                                       ->GetOrderLayersByZPos(),
                                                   FlattenReason::kNone),
            .short_circuited = false};
  }
};

class ClientBackend : public TegraBackend {
 public:
  explicit ClientBackend(Device& device) : TegraBackend(device) {
  }

  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override {
    return std::make_unique<ClientCompositionPlanner>();
  }
};

// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<ClientBackend> kRegisterClient("client");

}  // namespace
}  // namespace android::drm_hwcomposer
