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

#include "TegraCompositor.h"

#include <errno.h>

#include <vector>

#include <tegra_dc_ext.h>

#include "bufferinfo/BufferInfo.h"
#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-compositor"

namespace android {
namespace hwc {

namespace {

uint32_t blendFor(BlendMode mode) {
    switch (mode) {
    case BlendMode::Premultiplied:
        return TEGRA_DC_EXT_BLEND_PREMULT;
    case BlendMode::Coverage:
        return TEGRA_DC_EXT_BLEND_COVERAGE;
    case BlendMode::None:
    default:
        return TEGRA_DC_EXT_BLEND_NONE;
    }
}

}  // namespace

TegraCompositor::TegraCompositor(DcHead &head, uint32_t panelWidth)
    : mHead(head), mPanelWidth(panelWidth) {}

int TegraCompositor::ensureWindows(size_t count) {
    for (size_t i = mClaimedWindows; i < count; ++i) {
        int err = mHead.claimWindow(static_cast<uint32_t>(i));
        if (err) {
            /* Whatever was claimed before this stays claimed and usable, so
             * the count reflects reality rather than the request. */
            mClaimedWindows = i;
            return err;
        }
    }

    if (count > mClaimedWindows)
        mClaimedWindows = count;
    return 0;
}

int TegraCompositor::describeWindow(const PlannedLayer &layer, int32_t index,
                                    uint32_t z, DcHead::Window *outWindow) {
    BufferInfo info;
    int err = describeBuffer(layer.buffer, mPanelWidth, &info);
    if (err)
        return err;

    *outWindow = DcHead::Window{};

    outWindow->index = index;
    outWindow->bufferFd = info.fd;
    outWindow->offset = info.offset;
    outWindow->stride = info.strideBytes;
    outWindow->pixelFormat = info.format;
    outWindow->flags = info.flags;

    outWindow->sourceX = layer.sourceCrop.left;
    outWindow->sourceY = layer.sourceCrop.top;
    outWindow->sourceWidth = layer.sourceCrop.width();
    outWindow->sourceHeight = layer.sourceCrop.height();

    outWindow->outX = layer.displayFrame.left;
    outWindow->outY = layer.displayFrame.top;
    outWindow->outWidth = layer.displayFrame.width();
    outWindow->outHeight = layer.displayFrame.height();

    outWindow->z = z;
    outWindow->blend = blendFor(layer.blend);

    /* Borrowed. The plan does not own it and neither does the window: the
     * kernel waits on it during the flip and the layer closes it later. */
    outWindow->preFence = layer.acquireFence;

    return 0;
}

int TegraCompositor::test(const FramePlan &plan) {
    if (plan.isEmpty())
        return -EINVAL;

    /* Every layer must be describable to the hardware. A buffer in a format
     * the controller cannot scan out is the one refusal worth making here,
     * and making it now rather than mid-flip is the point of asking.
     *
     * How many windows the head actually has is deliberately not checked
     * against a constant. The compile-time maximum for this chip counts
     * windows across the controller, while a head owns whichever the board
     * wired to it; the authority is the hardware, which refuses to hand over
     * a window that is not there. So the limit is discovered by claiming
     * rather than assumed, and present reports what it managed to get.
     *
     * Nor is there a dry run yet. The controller can also refuse a plan over
     * scaling ratios, per-window format and rotation limits, or bandwidth,
     * and the honest way to ask is the proposed-configuration ioctl. Until
     * layers are handed to windows at all, every plan here is one full-screen
     * buffer and there is nothing for it to weigh.
     */
    for (const PlannedLayer &layer : plan.layers()) {
        BufferInfo info;
        int err = describeBuffer(layer.buffer, mPanelWidth, &info);
        if (err)
            return err;
    }

    return 0;
}

int TegraCompositor::present(const FramePlan &plan, UniqueFd *outPresentFence) {
    if (plan.isEmpty())
        return -EINVAL;

    /* No test() here. It answers the same question by describing every
     * buffer, and the loop below describes them again to build the windows;
     * running both would do that work twice for every frame. Whatever test
     * would have refused, describeWindow refuses on the same grounds, before
     * anything has been posted. */
    int err = ensureWindows(plan.layerCount());
    if (err) {
        HWC_LOGE("only %zu of %zu window(s) claimed", mClaimedWindows,
                 plan.layerCount());
        return err;
    }

    std::vector<DcHead::Window> windows;
    windows.reserve(plan.layerCount());

    for (size_t i = 0; i < plan.layers().size(); ++i) {
        DcHead::Window window;
        err = describeWindow(plan.layers()[i], static_cast<int32_t>(i),
                             static_cast<uint32_t>(i), &window);
        if (err) {
            HWC_LOGE("layer %zu cannot be shown: %d", i, err);
            return err;
        }
        windows.push_back(window);
    }

    return mHead.flip(windows, outPresentFence);
}

}  // namespace hwc
}  // namespace android
