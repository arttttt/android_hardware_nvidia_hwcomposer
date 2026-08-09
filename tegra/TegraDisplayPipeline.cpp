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

#include "TegraDisplayPipeline.h"

#include <errno.h>
#include <stdio.h>

#include <utility>

#include "utils/Logging.h"

#include "tegra/FbDevice.h"
#include "tegra/TegraCompositor.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-pipeline"

namespace android {
namespace hwc {

std::unique_ptr<TegraDisplayPipeline> TegraDisplayPipeline::create(int index) {
    DisplayMode mode;
    int err = readDisplayMode(index, &mode);
    if (err)
        return nullptr;

    std::unique_ptr<DcHead> head = DcHead::open(index);
    if (!head)
        return nullptr;

    /* The head index doubles as the event handle: the controller reports
     * blanks against the same numbering the device nodes use. */
    std::unique_ptr<TegraVSyncSource> vsync =
        TegraVSyncSource::create(static_cast<uint32_t>(index));
    if (!vsync)
        return nullptr;

    return std::unique_ptr<TegraDisplayPipeline>(new TegraDisplayPipeline(
        index, std::move(head), std::move(vsync), mode));
}

TegraDisplayPipeline::TegraDisplayPipeline(int index,
                                           std::unique_ptr<DcHead> head,
                                           std::unique_ptr<TegraVSyncSource> vsync,
                                           const DisplayMode &mode)
    : mIndex(index),
      mHead(std::move(head)),
      mVSync(std::move(vsync)),
      mCompositor(new TegraCompositor(*mHead)),
      mModes{mode} {}

TegraDisplayPipeline::~TegraDisplayPipeline() {
    /* Ordered: the vertical blank reader must stop before the devices it
     * reads from go away. Destroying members in reverse declaration order
     * would take the head first, which is harmless today and would not stay
     * harmless once the compositor holds it. */
    mVSync.reset();
    mCompositor.reset();
    mHead.reset();
}

std::string TegraDisplayPipeline::name() const {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "tegra-dc-%d", mIndex);
    return buffer;
}

int TegraDisplayPipeline::setActiveMode(size_t index) {
    /* A fixed panel has one timing, so the only valid choice is the one
     * already in use. Refusing anything else is more useful than silently
     * accepting a change that will not happen. */
    return index == 0 ? 0 : -EINVAL;
}

int TegraDisplayPipeline::setPowerMode(PowerMode mode) {
    return setPanelPowered(mIndex, mode == PowerMode::On);
}

void TegraDisplayPipeline::setCompositor(std::unique_ptr<Compositor> compositor) {
    if (!compositor) {
        HWC_LOGE("refusing a null compositor; keeping the current one");
        return;
    }
    mCompositor = std::move(compositor);
}

}  // namespace hwc
}  // namespace android
