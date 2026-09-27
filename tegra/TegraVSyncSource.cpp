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
 * it, and short enough that a stopped one is noticed.
 *
 * It is paid once, at the moment blanks stop, and not again on every wait
 * after that. Giving up is not a failure -- the caller answers it by timing
 * the blanks itself -- but giving up slowly, over and over, is: it turns a
 * display with no blanks to read into a display running at whatever rate
 * this source fails at, which is a sixth of the panel's and looks from the
 * outside like every part of the system being slow at once.
 */
constexpr int kWaitTimeoutMs = 166;

/* And how long to wait once they are known to have stopped: not at all.
 * The stream is still looked at, because a blank may have arrived since the
 * last look, and that is how reporting is noticed to have resumed. */
constexpr int kNoWaitMs = 0;

constexpr int64_t kOneSecondNs = 1'000'000'000;

/* Older than this, a blank is not an answer to a wait. The freshest honest
 * blank is younger than a period plus the interrupt's wakeup: under 17 ms at
 * the panel's rate, under 34 at half of it. Three periods is past both with
 * room, and short of anything that could only have been queued while nobody
 * was reading -- an interrupt already raised when reporting was turned off,
 * a tail not read dry -- and would otherwise be taken for the present. */
constexpr int64_t kStaleNs = 50'000'000;

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
     * it is, this costs one call the driver answers from a flag.
     *
     * The driver answers the request itself: granted means the head is on
     * and blanks are coming, so this wait is worth its timeout; refused
     * means the head is off, and the wait only looks. A grant is trusted
     * until a wait times out on one; from then on a blank has to be seen
     * first, because the driver's answer can be stale (see the flag). */
    if (!mReporting)
        mReporting = mHead.setVBlankReporting(true) == 0 &&
                     !mTimedOutWhileGranted;

    struct pollfd fd = {};
    fd.fd = mControl->fd();
    fd.events = POLLIN;

    bool found = false;
    int64_t timestampNs = 0;

    /* Loops because the stream carries more than this display's blanks, and
     * an event that is not the one waited for is not an answer.
     *
     * And loops on past the first answer, because the stream may hold more
     * than one. Every blank since the last read is queued, and after a
     * spell of not reading -- the framework asks for blanks only while it
     * has a use for them -- the queue holds a tail of them, each dated when
     * it happened. Handed out one per wait, the tail would arrive within a
     * few milliseconds as a burst of blanks from the past, and the
     * framework's model of the panel's phase, which locks on the first few
     * it sees, would lock on them and wake the compositor on the latch
     * instead of ahead of it. So once a blank is in hand the queue is read
     * dry without waiting, and the newest is the answer: the rest happened,
     * but are not news. */
    while (true) {
        fd.revents = 0;

        int ready = poll(&fd, 1, found || !mReporting ? kNoWaitMs
                                                      : kWaitTimeoutMs);

        if (ready < 0) {
            if (found)
                break;
            /* Handed straight back rather than retried: a wait interrupted
             * by a signal is how the caller's thread is asked to look at
             * whether it should still be running. */
            return -errno;
        }

        if (ready == 0) {
            if (found)
                break;
            if (mReporting) {
                mReporting = false;
                mTimedOutWhileGranted = true;
                HWC_LOGW("head %u stopped reporting blanks; asking again on "
                         "every wait from here", mHeadHandle);
            }
            return -ETIMEDOUT;
        }

        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* A descriptor that only reports trouble would otherwise be
             * polled without pause. */
            if (found)
                break;
            HWC_LOGE("head %u: control node reports 0x%x", mHeadHandle,
                     fd.revents);
            return -EIO;
        }

        if (!(fd.revents & POLLIN))
            continue;

        DcControl::Event event;
        int err = mControl->readEvent(&event);
        if (err) {
            if (found)
                break;
            HWC_LOGE("readEvent: %s", strerror(-err));
            return err;
        }

        if (event.type != DcControl::EventType::VBlank)
            continue;
        if (event.handle != mHeadHandle)
            continue;
        if (event.timestampNs != 0 && event.timestampNs < now() - kStaleNs)
            continue;

        mTimedOutWhileGranted = false;
        if (!mReporting) {
            mReporting = true;
            HWC_LOGI("head %u is reporting blanks", mHeadHandle);
        }

        /* The driver stamps the event in the interrupt that saw the blank,
         * against the clock everything else here is scheduled by, so the
         * blank is dated when it happened rather than when this thread got
         * round to it. A zero means it did not say, and then the best that
         * can be claimed is now, within one wakeup of the truth. */
        timestampNs = event.timestampNs != 0 ? event.timestampNs : now();
        found = true;
    }

    *outTimestampNs = timestampNs;
    return 0;
}

void TegraVSyncSource::stop() {
    /* Reporting first, so that the controller stops producing blanks before
     * the queue is emptied, and the queue is not left with a blank that
     * slipped in between. */
    mHead.setVBlankReporting(false);
    mReporting = false;
    discardQueued();
}

void TegraVSyncSource::discardQueued() {
    struct pollfd fd = {};
    fd.fd = mControl->fd();
    fd.events = POLLIN;

    while (true) {
        fd.revents = 0;
        if (poll(&fd, 1, kNoWaitMs) <= 0 || !(fd.revents & POLLIN))
            return;

        DcControl::Event event;
        if (mControl->readEvent(&event))
            return;
    }
}

}  // namespace hwc
}  // namespace android
