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

TegraCompositor::TegraCompositor(DcHead &head): mHead(head) {}

int TegraCompositor::describeWindow(const PlannedLayer &layer, uint32_t index,
                                    uint32_t z, DcHead::Window *outWindow) {
    BufferInfo info;
    int err = describeBuffer(layer.buffer, &info);
    if (err)
        return err;

    *outWindow = DcHead::Window{};

    outWindow->index = static_cast<int32_t>(index);
    outWindow->bufferFd = info.fd;
    outWindow->offset = info.offset;
    outWindow->stride = info.strideBytes;
    outWindow->pixelFormat = info.format;
    outWindow->flags = info.flags;
    outWindow->blockHeightLog2 = info.blockHeightLog2;

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

    /* One window per layer, and the head has as many as it has.
     *
     * There is no dry run beyond this yet. The controller can also refuse a
     * plan over scaling ratios, per-window format and rotation limits, or
     * bandwidth, and the honest way to ask is the proposed-configuration
     * ioctl. Until layers are handed to windows at all, every plan here is
     * one full-screen buffer and there is nothing for it to weigh.
     */
    const size_t available = mHead.windows().size();
    if (plan.layerCount() > available) {
        HWC_LOGE("%zu layer(s) will not fit in %zu window(s)",
                 plan.layerCount(), available);
        return -EINVAL;
    }

    /* Every layer must be describable to the hardware. A buffer in a format
     * the controller cannot scan out is the one refusal worth making here,
     * and making it now rather than mid-flip is the point of asking. */
    for (const PlannedLayer &layer : plan.layers()) {
        BufferInfo info;
        int err = describeBuffer(layer.buffer, &info);
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
    const std::vector<uint32_t> &available = mHead.windows();
    if (plan.layerCount() > available.size()) {
        HWC_LOGE("%zu layer(s) will not fit in %zu window(s)",
                 plan.layerCount(), available.size());
        return -EINVAL;
    }

    /* Every window of the head goes into every flip, not just the ones with
     * something to show.
     *
     * A window keeps whatever it was last given until it is told otherwise,
     * and a flip only touches the windows it names. Leaving one out therefore
     * does not clear it: it stays enabled with its old buffer, and if its
     * depth puts it above ours it covers the frame we just posted. Whoever
     * set it up -- the kernel's own framebuffer, or the composer before a
     * layer moved off an overlay -- is not around to take it down.
     *
     * So the unused ones are sent as well, with no buffer, which is how the
     * driver is told to switch a window off. That is what a default-built
     * Window already is, hence the loop only fills in the ones a layer
     * claimed.
     */
    std::vector<DcHead::Window> windows(available.size());

    for (size_t i = 0; i < available.size(); ++i) {
        windows[i].index = static_cast<int32_t>(available[i]);

        if (i >= plan.layerCount())
            continue;

        int err = describeWindow(plan.layers()[i], available[i],
                                 static_cast<uint32_t>(i), &windows[i]);
        if (err) {
            HWC_LOGE("layer %zu cannot be shown: %d", i, err);
            return err;
        }
    }

    UniqueFd postFence;
    int err = mHead.flip(windows, &postFence);
    if (err)
        return err;

    /* The fence handed out is the one the flip before this got, not this
     * flip's.
     *
     * The driver builds a flip's fence one step past the counter that flip
     * advances, so it does not come due until the following flip finishes.
     * Read literally that is a fence for a frame that has not been asked for
     * yet, and handing it over as this frame's is a deadlock rather than a
     * pessimism: the framework attaches it to the buffer it drew the previous
     * frame into, then waits on it before drawing the next one -- into that
     * same buffer, there being only two. The frame that would release it is
     * the frame that cannot start.
     *
     * Shifting by one says what the framework is actually asking. The
     * previous flip's fence comes due exactly when this flip finishes, which
     * is the moment this frame is on the panel and the one before it is no
     * longer being read.
     *
     * The first frame has no predecessor and so hands back nothing, which is
     * read as already presented. It is: there was no earlier frame to wait
     * for.
     */
    *outPresentFence = std::move(mPreviousPostFence);
    mPreviousPostFence = std::move(postFence);

    return 0;
}

}  // namespace hwc
}  // namespace android
