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

#include "tegra/EngineWarmer.h"

#include "utils/fd.h"
#include "utils/log.h"

namespace android {
namespace hwc {

std::unique_ptr<EngineWarmer> EngineWarmer::Create(VicSession *vic) {
  if (vic == nullptr)
    return nullptr;

  std::unique_ptr<VendorBuffer> source = vic->AllocateZoneTarget(side, side);
  std::unique_ptr<VendorBuffer> target = vic->AllocateZoneTarget(side, side);
  if (!source || !target) {
    ALOGW("engine warmer: the zone would not give the two buffers; the "
          "engine will not be warmed");
    return nullptr;
  }

  return std::unique_ptr<EngineWarmer>(
      new EngineWarmer(vic, std::move(source), std::move(target)));
}

EngineWarmer::EngineWarmer(VicSession *vic,
                           std::unique_ptr<VendorBuffer> source,
                           std::unique_ptr<VendorBuffer> target)
    : vic_(vic), source_(std::move(source)), target_(std::move(target)) {
}

int EngineWarmer::Run() {
  /* Verbatim, unturned, nothing to wait on. */
  VicSession::Layer layer = {};
  layer.handle = nullptr;
  layer.vendor = source_.get();
  layer.source_right = static_cast<float>(side);
  layer.source_bottom = static_cast<float>(side);
  layer.display_right = static_cast<int32_t>(side);
  layer.display_bottom = static_cast<int32_t>(side);
  layer.premultiplied = true;
  layer.alpha = 1.0F;
  layer.acquire_fence = -1;

  const drm_hwcomposer::SharedFd done =
      vic_->CopyLayer(*target_, layer, side, side);
  if (!done)
    return -1;
  return drm_hwcomposer::DupFd(done);
}

}  // namespace hwc
}  // namespace android
