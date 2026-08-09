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

#include <cstdint>
#include <memory>

#include "display/VSyncSource.h"
#include "tegra/DcControl.h"

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
 *
 * No thread of its own, and nothing to start or stop. Whoever waits here is
 * the thread that wanted the blank, and everything built on top of blanks --
 * when to deliver them, what the period is, when the next one falls -- is
 * upstream's and lives above this.
 */
class TegraVSyncSource : public VSyncSource {
public:
    /* `headHandle` selects which display's blanks are reported; events for
     * any other are dropped. Returns null if the control node will not open,
     * or if the controller refuses to report blanks. */
    static std::unique_ptr<TegraVSyncSource> create(uint32_t headHandle);

    ~TegraVSyncSource() override;

    int waitForVSync(int64_t *outTimestampNs) override;

private:
    TegraVSyncSource(std::unique_ptr<DcControl> control, uint32_t headHandle)
        : mControl(std::move(control)), mHeadHandle(headHandle) {}

    std::unique_ptr<DcControl> mControl;
    const uint32_t mHeadHandle;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_VSYNC_SOURCE_H
