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

#include "governor/GovernorLibrary.h"

#include <dlfcn.h>

#include "utils/log.h"

namespace android {
namespace hwc {

namespace {

/* Fixed by name: which library runs is not a per-device choice. Looked for
 * beside the composer first, by full path: a vendor process's linker
 * searches the vendor library directory itself, not its module
 * subdirectory, and the composer's own modules live in the latter. */
constexpr const char *libraryName = "hwcgovernor.tegra.so";
constexpr const char *libraryPaths[] = {"/vendor/lib/hw/hwcgovernor.tegra.so",
                                        "hwcgovernor.tegra.so"};

template <typename Fn>
bool Resolve(void *handle, const char *symbol, Fn **out) {
  *out = reinterpret_cast<Fn *>(dlsym(handle, symbol));
  if (*out == nullptr) {
    ALOGE("composition governor: %s lacks %s", libraryName, symbol);
    return false;
  }
  return true;
}

}  // namespace

std::unique_ptr<GovernorLibrary> GovernorLibrary::Open() {
  void *handle = nullptr;
  for (const char *path : libraryPaths) {
    handle = dlopen(path, RTLD_NOW);
    if (handle != nullptr)
      break;
  }
  if (handle == nullptr) {
    /* The usual answer on a device that ships no policy, so said quietly. */
    ALOGI("composition governor: no %s (%s), running without", libraryName,
          dlerror());
    return nullptr;
  }

  auto library = std::unique_ptr<GovernorLibrary>(new GovernorLibrary(handle));
  if (!library->Resolve())
    return nullptr;
  return library;
}

bool GovernorLibrary::Resolve() {
  if (!hwc::Resolve(handle_, "hwc_governor_api_version", &version_) ||
      !hwc::Resolve(handle_, "hwc_governor_create", &create_) ||
      !hwc::Resolve(handle_, "hwc_governor_destroy", &destroy_))
    return false;

  const uint32_t offered = version_();
  if (offered != governor::apiVersion) {
    ALOGE("composition governor: %s speaks API %u, this composer %u",
          libraryName, offered, governor::apiVersion);
    return false;
  }
  return true;
}

GovernorLibrary::~GovernorLibrary() {
  if (handle_ != nullptr)
    dlclose(handle_);
}

governor::Governor *GovernorLibrary::Create(governor::GovernorHost *host) {
  governor::Governor *governor = create_(host);
  if (governor == nullptr)
    ALOGE("composition governor: %s declined to start", libraryName);
  return governor;
}

void GovernorLibrary::Destroy(governor::Governor *governor) {
  destroy_(governor);
}

}  // namespace hwc
}  // namespace android
