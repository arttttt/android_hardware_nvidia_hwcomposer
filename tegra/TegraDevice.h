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

#ifndef TEGRA_DEVICE_H
#define TEGRA_DEVICE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "backend/Backend.h"
#include "display/AtomicCommitSink.h"
#include "display/Device.h"
#include "tegra/TegraConnector.h"

namespace android {
namespace hwc {

/* The display controller, as the thing the displays hang off.
 *
 * Upstream's equivalent is a card under /dev/dri, and most of its file is
 * about asking that card what it has. There is no such question to ask here:
 * this controller's heads are found by trying them, and what is on the end of
 * one is found from its framebuffer device.
 *
 * This owns the connectors, because a connector outlives the pipeline bound
 * to it -- a display can be taken apart and put back together without the
 * panel going anywhere.
 */
class TegraDevice : public drm_hwcomposer::Device {
public:
    /* Finds the heads that have a panel on them. Returns null if none has,
     * which is not a situation worth carrying on from. */
    static std::unique_ptr<TegraDevice> create();

    ~TegraDevice() override;

    /* What picks the backend, and what a property override overrides. */
    std::string GetName() const override { return "tegra"; }

    drm_hwcomposer::Backend &GetBackend() const override { return *mBackend; }

    drm_hwcomposer::AtomicCommitSink &GetAtomicCommitSink() override {
        return *mSink;
    }

    /* Nothing to hand over.
     *
     * Upstream allocates a black buffer to scan out while a timing changes,
     * because what was on screen was drawn for the timing being left behind.
     * The panels here run one timing each and never leave it, so there is no
     * crossing to cover.
     */
    std::optional<drm_hwcomposer::BufferInfo> CreateBufferForModeset(
        uint32_t /*width*/, uint32_t /*height*/) override {
        return std::nullopt;
    }

    /* The panels found at start-up, in head order. What builds a display is
     * handed one of these; the pipeline it builds binds to it. */
    const std::vector<std::unique_ptr<drm_hwcomposer::Connector>>
        &GetConnectors() const override {
        return mConnectors;
    }

private:
    TegraDevice() = default;

    std::vector<std::unique_ptr<drm_hwcomposer::Connector>> mConnectors;

    std::unique_ptr<drm_hwcomposer::Backend> mBackend;
    std::unique_ptr<drm_hwcomposer::AtomicCommitSink> mSink;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DEVICE_H
