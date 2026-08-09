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
#include <unistd.h>

#include <tegra_dc_ext.h>

#include <utils/Log.h>

#undef  LOG_TAG
#define LOG_TAG "hwc-vsync"

namespace android {
namespace hwc {

std::unique_ptr<TegraVSyncSource> TegraVSyncSource::create(uint32_t headHandle) {
    std::unique_ptr<DcControl> control = DcControl::open();
    if (!control)
        return nullptr;

    return std::unique_ptr<TegraVSyncSource>(
        new TegraVSyncSource(std::move(control), headHandle));
}

TegraVSyncSource::~TegraVSyncSource() {
    disable();
}

int TegraVSyncSource::enable(Callback callback) {
    if (!callback)
        return -EINVAL;

    /* Replacing the callback while running would leave the reader thread
     * calling whichever it happened to read, so restart instead. */
    disable();

    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) < 0) {
        int err = -errno;
        ALOGE("stop pipe: %s", strerror(-err));
        return err;
    }
    mStopReadFd.reset(pipeFds[0]);
    mStopWriteFd.reset(pipeFds[1]);

    int err = mControl->setEventMask(TEGRA_DC_EXT_EVENT_VBLANK);
    if (err) {
        mStopReadFd.reset();
        mStopWriteFd.reset();
        return err;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCallback = std::move(callback);
        mRunning = true;
    }

    mThread = std::thread(&TegraVSyncSource::run, this);
    return 0;
}

int TegraVSyncSource::disable() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mRunning)
            return 0;
        mRunning = false;
    }

    signalStop();

    if (mThread.joinable())
        mThread.join();

    /* Only now, with the reader stopped for certain, is it safe to say that
     * no callback is in flight -- which is what the interface promises. */
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCallback = nullptr;
    }

    mControl->setEventMask(0);

    mStopReadFd.reset();
    mStopWriteFd.reset();
    return 0;
}

int TegraVSyncSource::signalStop() {
    if (!mStopWriteFd)
        return 0;

    const char byte = 1;
    ssize_t written = TEMP_FAILURE_RETRY(write(mStopWriteFd.get(), &byte, 1));
    if (written < 0) {
        int err = -errno;
        ALOGE("stop write: %s", strerror(-err));
        return err;
    }
    return 0;
}

void TegraVSyncSource::run() {
    struct pollfd fds[2];
    fds[0].fd = mControl->fd();
    fds[0].events = POLLIN;
    fds[1].fd = mStopReadFd.get();
    fds[1].events = POLLIN;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning)
                return;
        }

        fds[0].revents = 0;
        fds[1].revents = 0;

        int ready = TEMP_FAILURE_RETRY(poll(fds, 2, -1));
        if (ready < 0) {
            ALOGE("poll: %s", strerror(errno));
            return;
        }

        /* Checked before the event stream: asked to stop, stop, even if a
         * blank arrived in the same wakeup. */
        if (fds[1].revents & POLLIN)
            return;

        if (!(fds[0].revents & POLLIN))
            continue;

        DcControl::Event event;
        int err = mControl->readEvent(&event);
        if (err) {
            ALOGE("readEvent: %s", strerror(-err));
            return;
        }

        if (event.type != DcControl::EventType::VBlank)
            continue;
        if (event.handle != mHeadHandle)
            continue;

        /* Copied under the lock and called outside it. Holding a lock across
         * a callback into the composer invites a deadlock the first time
         * that callback wants to touch this object. */
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning)
                return;
            callback = mCallback;
        }

        if (callback)
            callback(event.timestampNs);
    }
}

}  // namespace hwc
}  // namespace android
