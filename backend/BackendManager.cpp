/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "BackendManager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "backend/Backend.h"
#include "display/Connector.h"
#include "display/Device.h"
#include "display/DisplayPipeline.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {
// List of devices that should default to client composition.
// NOLINTNEXTLINE(cert-err58-cpp)
const std::vector<std::string> kClientDevices = {
    "kirin",
    "mediatek-drm",
    "pl111",
};
}  // namespace

BackendManager &BackendManager::GetInstance() {
  static BackendManager backend_manager;
  return backend_manager;
}

void BackendManager::RegisterCreator(const std::string &name,
                                     BackendCreator creator) {
  if (creators_.count(name) != 0) {
    ALOGE("Backend creator for %s already registered.", name.c_str());
    return;
  }
  creators_[name] = std::move(creator);
}

std::unique_ptr<Backend> BackendManager::CreateBackendForDevice(
    Device &device) {
  if (creators_.empty()) {
    ALOGE("No backends are registered");
    return nullptr;
  }

  std::string name = Properties::GetBackendOverride();
  if (name.empty()) {
    name = device.GetName();
  }

  auto it = creators_.find(name);
  if (it == creators_.end()) {
    auto client_it = std::find(kClientDevices.begin(), kClientDevices.end(),
                               name);
    if (client_it == kClientDevices.end()) {
      /* This used to fall through to "generic", which nothing registers;
       * operator[] then minted an empty function and calling it aborted
       * the composer. A typo in a property deserves a log line, not a
       * crash loop. */
      ALOGE("No backend is registered under '%s'", name.c_str());
      return nullptr;
    }
    it = creators_.find("client");
    if (it == creators_.end()) {
      ALOGE("The client backend is not registered");
      return nullptr;
    }
    name = "client";
  }

  ALOGI("Creating backend '%s' for device '%s'", name.c_str(),
        device.GetName().c_str());
  return it->second(device);
}

}  // namespace android::drm_hwcomposer
