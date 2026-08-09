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

#include "tegra/TegraDevice.h"

#include <memory>
#include <utility>

#include "backend/BackendManager.h"
#include "utils/Logging.h"

#include "tegra/FbDevice.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-device"

namespace android {
namespace hwc {

namespace {

/* How many heads to try.
 *
 * This chip has two, and a head with nothing on it answers no differently
 * from one that is not there -- its framebuffer device either refuses to
 * open or reports no resolution. So they are found by asking rather than by
 * being told, and the count is only a place to stop.
 */
constexpr int kHeadsToProbe = 2;

}  // namespace

std::unique_ptr<TegraDevice> TegraDevice::create() {
    std::unique_ptr<TegraDevice> device(new TegraDevice());

    for (int index = 0; index < kHeadsToProbe; ++index) {
        PanelTiming timing;
        if (readPanelTiming(index, &timing) != 0)
            continue;

        device->mConnectors.push_back(
            std::make_unique<TegraConnector>(static_cast<uint32_t>(index),
                                             timing));
    }

    if (device->mConnectors.empty()) {
        HWC_LOGE("no head has a panel on it");
        return nullptr;
    }

    /* Which backend gets built is decided by name, and a property can say a
     * different name -- which is how the whole composer is put into fully
     * client-composited mode without changing anything here. */
    device->mBackend =
        drm_hwcomposer::BackendManager::GetInstance().CreateBackendForDevice(
            *device);
    if (!device->mBackend) {
        HWC_LOGE("no backend for this hardware");
        return nullptr;
    }

    device->mSink = device->mBackend->CreateAtomicCommitSink();
    if (!device->mSink) {
        HWC_LOGE("backend has nowhere to send frames");
        return nullptr;
    }

    HWC_LOGI("%zu head(s) with a panel", device->mConnectors.size());

    return device;
}

TegraDevice::~TegraDevice() {
    /* Ordered: the sink was made by the backend and the connectors outlive
     * whatever bound to them, so both go before the backend does. */
    mSink.reset();
    mConnectors.clear();
    mBackend.reset();
}

}  // namespace hwc
}  // namespace android
