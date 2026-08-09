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

#include "TegraVSyncSource.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tegra_dc_ext.h>

#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-vsync"

namespace android {
namespace hwc {

namespace {

/* How long to wait for a blank before deciding they have stopped.
 *
 * A wait with no limit is what the hardware invites: a panel that has been
 * powered down reports nothing and would hold the thread for ever. Ten
 * blanks at sixty hertz is long enough that a running panel never reaches
 * it, and short enough that a stopped one is noticed. Giving up is not a
 * failure -- the caller answers it by timing the blanks itself.
 */
constexpr int kWaitTimeoutMs = 166;

constexpr int64_t kOneSecondNs = 1'000'000'000;

int64_t now() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * kOneSecondNs + ts.tv_nsec;
}

}  // namespace

std::unique_ptr<TegraVSyncSource> TegraVSyncSource::create(DcHead &head,
                                                           uint32_t headHandle) {
    std::unique_ptr<DcControl> control = DcControl::open();
    if (!control)
        return nullptr;

    /* Subscribed once and left subscribed. This says which of the events the
     * driver produces are to be handed to this reader; it does not make the
     * driver produce any. Whether blanks are passed on to the framework is
     * decided above this; here they are only read. */
    int err = control->setEventMask(TEGRA_DC_EXT_EVENT_VBLANK);
    if (err)
        return nullptr;

    return std::unique_ptr<TegraVSyncSource>(
        new TegraVSyncSource(std::move(control), head, headHandle));
}

TegraVSyncSource::~TegraVSyncSource() {
    /* Both halves let go, and in this order. Reporting first, so that the
     * controller stops producing blanks before the only reader of them goes
     * away, and its interrupt is not left unmasked for nobody. */
    mHead.setVBlankReporting(false);
    mControl->setEventMask(0);
}

int TegraVSyncSource::waitForVSync(int64_t *outTimestampNs) {
    /* Asked for again whenever they are not arriving, rather than once and
     * assumed to hold. The driver drops the request when the head is turned
     * off and says nothing about having done so, so the only honest reading
     * of a silent stream is that the request may no longer be in force. When
     * it is, this costs one call the driver answers from a flag. */
    if (!mReporting)
        mHead.setVBlankReporting(true);

    struct pollfd fd = {};
    fd.fd = mControl->fd();
    fd.events = POLLIN;

    /* Loops because the stream carries more than this display's blanks, and
     * an event that is not the one waited for is not an answer. */
    while (true) {
        fd.revents = 0;

        int ready = poll(&fd, 1, kWaitTimeoutMs);

        if (ready < 0) {
            /* Handed straight back rather than retried: a wait interrupted
             * by a signal is how the caller's thread is asked to look at
             * whether it should still be running. */
            return -errno;
        }

        if (ready == 0) {
            if (mReporting) {
                mReporting = false;
                HWC_LOGW("head %u stopped reporting blanks; asking again on "
                         "every wait from here", mHeadHandle);
            }
            return -ETIMEDOUT;
        }

        if (!(fd.revents & POLLIN))
            continue;

        DcControl::Event event;
        int err = mControl->readEvent(&event);
        if (err) {
            HWC_LOGE("readEvent: %s", strerror(-err));
            return err;
        }

        if (event.type != DcControl::EventType::VBlank)
            continue;
        if (event.handle != mHeadHandle)
            continue;

        if (!mReporting) {
            mReporting = true;
            HWC_LOGI("head %u is reporting blanks", mHeadHandle);
        }

        /* The driver stamps the event in the interrupt that saw the blank,
         * against the clock everything else here is scheduled by, so the
         * blank is dated when it happened rather than when this thread got
         * round to it. A zero means it did not say, and then the best that
         * can be claimed is now, within one wakeup of the truth. */
        *outTimestampNs = event.timestampNs != 0 ? event.timestampNs : now();
        return 0;
    }
}

}  // namespace hwc
}  // namespace android
