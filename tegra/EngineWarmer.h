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

#include "tegra/VicSession.h"

namespace android {
namespace hwc {

/* One tiny pass on the engine, so that a powered-down engine pays for
 * coming up now rather than on the merge that follows.
 *
 * Eight by eight pixels copied between two buffers of the composer's own,
 * cut from the zone once at creation: the engine's coming up costs the
 * same whatever the job's size, so the job is as small as the zone's row
 * grain lets it be. The source is read by the words the library built
 * when the buffer was born -- the `vendor` seat of a layer, which this is
 * the first caller of.
 */
class EngineWarmer {
 public:
  /* `vic` is borrowed and must outlive this. Null, having said what
   * failed, if the zone would not give the two buffers. Allocated on the
   * calling thread, deliberately: the zone opens its device on the first
   * allocation, and two threads asking at once would race for it. */
  static std::unique_ptr<EngineWarmer> Create(VicSession *vic);

  EngineWarmer(const EngineWarmer &) = delete;
  EngineWarmer &operator=(const EngineWarmer &) = delete;

  /* Runs the pass. Returns its fence, owned by the caller, or -1 if the
   * engine would not take it. Safe from any thread: the session serialises
   * its callers. */
  int Run();

 private:
  EngineWarmer(VicSession *vic, std::unique_ptr<VendorBuffer> source,
               std::unique_ptr<VendorBuffer> target);

  static constexpr uint32_t side = 8;

  VicSession *const vic_;
  std::unique_ptr<VendorBuffer> source_;
  std::unique_ptr<VendorBuffer> target_;
};

}  // namespace hwc
}  // namespace android
