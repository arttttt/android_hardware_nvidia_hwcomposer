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

#ifndef TEGRA_DC_CONTROL_H
#define TEGRA_DC_CONTROL_H

#include <cstdint>
#include <memory>

#include "utils/UniqueFd.h"

namespace android {
namespace hwc {

/* The display controller's control node, /dev/tegra_dc_ctrl.
 *
 * One node for the whole controller, as opposed to the per-head nodes that
 * carry flips. It answers what outputs exist and what the hardware can do,
 * and it is where display events are delivered.
 *
 * Events are the reason this is a class rather than a few free functions:
 * subscribing is a property of the open file, so whoever wants vblank has to
 * hold this descriptor open and read from it, and the two concerns cannot be
 * separated without handing the descriptor around.
 */
class DcControl {
public:
    /* One event as it arrives from the kernel. Only the two types this
     * composer acts on are represented; the rest are read and discarded so
     * that one uninteresting event cannot stall the stream. */
    enum class EventType {
        Unknown,
        Hotplug,
        VBlank,
    };

    struct Event {
        EventType type = EventType::Unknown;

        /* Which output the event is about. */
        uint32_t handle = 0;

        /* VBlank only: when the blank happened. The kernel stamps these
         * from its monotonic clock, which is the same base the framework
         * schedules against. */
        int64_t timestampNs = 0;

        /* Hotplug only. */
        bool connected = false;
    };

    /* Opens the control node. Returns null and logs on failure, which is
     * fatal for the composer: without this there is no display. */
    static std::unique_ptr<DcControl> open();

    ~DcControl();

    DcControl(const DcControl &) = delete;
    DcControl &operator=(const DcControl &) = delete;

    /* Number of outputs the controller knows about, connected or not. */
    int outputCount(uint32_t *outCount) const;

    /* Chooses which events this descriptor will deliver. A mask of zero
     * stops delivery. Returns 0 or a negative errno. */
    int setEventMask(uint32_t mask);

    /* Blocks until one event is available, then fills `outEvent`.
     *
     * Returns 0 on success, or a negative errno. Callers that must remain
     * interruptible should poll() on fd() instead of blocking here, because
     * there is no way to cancel a read already in progress.
     */
    int readEvent(Event *outEvent);

    /* For poll(). The descriptor stays owned here. */
    int fd() const { return mFd.get(); }

private:
    explicit DcControl(UniqueFd fd): mFd(std::move(fd)) {}

    UniqueFd mFd;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DC_CONTROL_H
