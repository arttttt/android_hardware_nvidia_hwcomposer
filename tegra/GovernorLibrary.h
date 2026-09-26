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

#include "governor/HwcGovernor.h"

namespace android {
namespace hwc {

/* The policy library, found by name and held open.
 *
 * Only the loading: which file, which symbols, whether its version is the
 * one this composer speaks. What the library is then asked to do is the
 * host's business. Null if the file is absent or answers with another
 * version -- the ordinary state of a device that ships no policy.
 */
class GovernorLibrary {
 public:
  static std::unique_ptr<GovernorLibrary> Open();

  ~GovernorLibrary();

  GovernorLibrary(const GovernorLibrary &) = delete;
  GovernorLibrary &operator=(const GovernorLibrary &) = delete;

  governor::Governor *Create(governor::GovernorHost *host);

  /* Synchronous, by the library's contract: returns once the governor
   * has stopped whatever it runs and closed what it was handed. */
  void Destroy(governor::Governor *governor);

 private:
  explicit GovernorLibrary(void *handle) : handle_(handle) {}

  bool Resolve();

  void *handle_;
  uint32_t (*version_)() = nullptr;
  governor::Governor *(*create_)(governor::GovernorHost *) = nullptr;
  void (*destroy_)(governor::Governor *) = nullptr;
};

}  // namespace hwc
}  // namespace android
