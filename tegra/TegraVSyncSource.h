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

#ifndef TEGRA_VSYNC_SOURCE_H
#define TEGRA_VSYNC_SOURCE_H

#include <memory>
#include <mutex>
#include <thread>

#include "display/VSyncSource.h"
#include "tegra/DcControl.h"
#include "utils/UniqueFd.h"

namespace android {
namespace hwc {

/* Vertical blank, read from the display controller's event stream.
 *
 * Holds its own open of the control node rather than sharing one. Event
 * subscription is a property of the open file and a read consumes an event
 * for whoever else might be waiting, so a shared descriptor would mean two
 * readers stealing each other's events. One owner, one reader, no contention.
 *
 * Only vertical blank is subscribed to. Hotplug exists in the same stream and
 * is ignored: the panel on this board is soldered down, and a display that
 * cannot leave has nothing to report.
 */
class TegraVSyncSource : public VSyncSource {
public:
    /* `headHandle` selects which display's blanks are passed on; events for
     * any other are dropped. Returns null if the control node will not open. */
    static std::unique_ptr<TegraVSyncSource> create(uint32_t headHandle);

    ~TegraVSyncSource() override;

    int enable(Callback callback) override;
    int disable() override;

private:
    TegraVSyncSource(std::unique_ptr<DcControl> control, uint32_t headHandle)
        : mControl(std::move(control)), mHeadHandle(headHandle) {}

    void run();

    /* Wakes the reader out of poll() so it can notice that it should stop.
     * Needed because a read already in progress cannot be cancelled, and
     * closing the descriptor underneath it is not a way to find out. */
    int signalStop();

    std::unique_ptr<DcControl> mControl;
    const uint32_t mHeadHandle;

    std::thread mThread;

    /* Read end polled alongside the event stream, write end used to wake it. */
    UniqueFd mStopReadFd;
    UniqueFd mStopWriteFd;

    /* Guards mCallback and mRunning against enable and disable racing the
     * reader thread. Never held while the callback is invoked. */
    std::mutex mMutex;
    Callback mCallback;
    bool mRunning = false;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_VSYNC_SOURCE_H
