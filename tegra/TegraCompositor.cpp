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

/* Long enough that a display running at sixty frames a second has had two
 * chances to take the frame, short enough that asking costs nothing. */
constexpr int kPresentProbeMs = 100;

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

    err = mHead.flip(windows, outPresentFence);
    if (err)
        return err;

    if (HWC_TRACE_ENABLED && *outPresentFence) {
        /* Does the frame actually reach the panel?
         *
         * Everything downstream hangs on this one fence. It signals when the
         * display has taken the frame, and the framework will not hand a
         * buffer back to whoever drew it until then -- so if it never fires,
         * the next frame has nothing to draw into, its own fence never fires
         * either, and the whole pipeline settles into one frame per timeout.
         * That is the shape of the stall we are looking at, and this is the
         * measurement that says whether the display is the reason.
         *
         * On a duplicate and with a short timeout, so the answer costs a
         * fraction of a frame and the fence handed to the framework is
         * untouched.
         */
        UniqueFd probe = outPresentFence->dup();
        if (probe) {
            const int64_t before = monotonicUs();
            const int signalled = sync_wait(probe.get(), kPresentProbeMs);
            const int64_t elapsedUs = monotonicUs() - before;

            if (signalled < 0)
                HWC_LOGE("present fence still unsignalled after %d ms",
                         kPresentProbeMs);
            else
                HWC_LOGD("present fence signalled after %" PRId64 " us",
                         elapsedUs);
        }
    }

    return 0;
}

}  // namespace hwc
}  // namespace android
