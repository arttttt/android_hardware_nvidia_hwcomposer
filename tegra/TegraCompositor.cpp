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
#include <inttypes.h>
#include <time.h>

#include <vector>

#include <android/sync.h>

#include <tegra_dc_ext.h>

#include "bufferinfo/BufferInfo.h"
#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-compositor"

namespace android {
namespace hwc {

namespace {

/* How long to wait for a buffer before giving up. Matches what the driver
 * allows itself, so a comparison between the two is like for like. */
constexpr int kAcquireWaitMs = 5000;

/* Read straight from the clock rather than through libutils: one timestamp
 * is not worth a library. */
int64_t monotonicUs() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

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

int TegraCompositor::describeWindow(const PlannedLayer &layer, uint32_t index,
                                    uint32_t z, DcHead::Window *outWindow) {
    BufferInfo info;
    int err = describeBuffer(layer.buffer, mPanelWidth, &info);
    if (err)
        return err;

    *outWindow = DcHead::Window{};

    outWindow->index = static_cast<int32_t>(index);
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

    if (HWC_WAIT_ACQUIRE_IN_USERSPACE && layer.acquireFence >= 0) {
        /* Wait here instead of handing the fence down.
         *
         * The driver waits on it for five seconds and gives up, every frame,
         * which stalls everything behind it -- buffers come back late and the
         * screen advances once per timeout. Waiting here answers whether the
         * fence signals at all: if this returns quickly the fence is sound
         * and the driver's wait is the problem, and if it times out too then
         * nothing is ever signalling it and the fault is further up.
         *
         * The cost of keeping this is real -- it puts the GPU and the display
         * in lockstep instead of letting the hardware overlap them -- so it
         * is a measurement, not a fix.
         */
        const int64_t before = monotonicUs();
        const int waited = sync_wait(layer.acquireFence, kAcquireWaitMs);
        const int64_t elapsedUs = monotonicUs() - before;

        if (waited < 0)
            HWC_LOGE("acquire fence %d did not signal in %d ms",
                     layer.acquireFence, kAcquireWaitMs);
        else
            HWC_LOGD("acquire fence %d signalled after %" PRId64 " us",
                     layer.acquireFence, elapsedUs);

        outWindow->preFence = -1;
    } else {
        /* Borrowed. The plan does not own it and neither does the window: the
         * kernel waits on it during the flip and the layer closes it later. */
        outWindow->preFence = layer.acquireFence;
    }

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

    int err = mHead.flip(windows, outPresentFence);
    if (err)
        return err;

    traceFrameLanded(*outPresentFence);
    return 0;
}

void TegraCompositor::traceFrameLanded(const UniqueFd &postFence) {
    if (!HWC_TRACE_ENABLED)
        return;

    /* Do the frames actually reach the panel?
     *
     * The fence a flip hands back says nothing about that flip. It is built
     * one step beyond the counter the flip advances, so it fires when the
     * *next* flip finishes -- which is precisely the moment the buffer just
     * posted stops being read and becomes free to draw into again. That makes
     * it the right answer to hand the framework, and a useless one to wait on
     * here: a frame's own fence cannot report on that frame.
     *
     * Two flips back it can. That fence was due when the flip before this one
     * finished, which was a frame ago, so asking now without waiting
     * separates a display that is quietly taking every frame from one that is
     * accepting flips and doing nothing with them.
     */
    if (mPostFences[1]) {
        if (sync_wait(mPostFences[1].get(), 0) == 0)
            HWC_LOGD("the frame before last is on the panel");
        else
            HWC_LOGE("the frame before last never reached the panel");
    }

    mPostFences[1] = std::move(mPostFences[0]);
    mPostFences[0] = postFence.dup();
}

}  // namespace hwc
}  // namespace android
