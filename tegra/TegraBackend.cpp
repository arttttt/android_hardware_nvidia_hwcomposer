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

#include "tegra/TegraBackend.h"

#include <memory>

#include "backend/BackendManager.h"
#include "display/AtomicCommitSink.h"
#include "display/Connector.h"
#include "display/DisplayPipeline.h"
#include "tegra/TegraAtomicCommitSink.h"
#include "tegra/TegraConnector.h"
#include "tegra/TegraDevice.h"
#include "tegra/TegraDisplayPipeline.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

std::unique_ptr<DisplayPipeline> TegraBackend::CreatePipeline(
    Connector &connector) {
  /* Safe: the only connectors in existence were made by the device this
   * backend was built for. */
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto &tegra_connector = static_cast<hwc::TegraConnector &>(connector);

  auto pipeline = hwc::TegraDisplayPipeline::create(tegra_connector);
  if (!pipeline)
    return nullptr;

  pipeline->device = &GetDevice();
  pipeline->importer = &static_cast<hwc::TegraDevice &>(GetDevice()).importer();
  pipeline->planner = CreateCompositionPlanner();

  return pipeline;
}

std::unique_ptr<AtomicCommitSink> TegraBackend::CreateAtomicCommitSink() {
  return std::make_unique<TegraAtomicCommitSink>();
}

// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<TegraBackend> kRegisterTegra("tegra");

}  // namespace android::drm_hwcomposer
