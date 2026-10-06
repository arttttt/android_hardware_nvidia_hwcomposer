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

#include <xf86drmMode.h>

#include "display/VSyncSource.h"
#include "tegra/DcControl.h"
#include "tegra/DcHead.h"

namespace android {
namespace hwc {

/* Vertical blank, read from the display controller's event stream.
 *
 * Two devices, because receiving blanks takes both. The head is asked to
 * report them at all -- that is what unmasks the controller's interrupt --
 * and the control device is where the events come out. Subscribing to the
 * stream without asking the head produces a reader waiting on a stream
 * nobody writes to, which is silent, indistinguishable from a panel that has
 * simply stopped, and costs a full timeout on every wait.
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
    /* `head` is asked to report blanks and must outlive this. `headHandle`
     * selects which display's blanks are taken from the stream; events for
     * any other are dropped. Returns null if the control node will not open.
     *
     * Reporting is not turned on here. Whether the head will take the request
     * depends on the display being on, which it need not be at the moment a
     * composer starts, so the request is made from the wait -- where the
     * answer to it is what the wait is about anyway. */
    /* `native` is the panel's own timing, which a reported porch is
     * measured against to give the period of the frame it ran. */
    static std::unique_ptr<TegraVSyncSource> create(
        DcHead &head, uint32_t headHandle, const drmModeModeInfo &native);

    ~TegraVSyncSource() override;

    int waitForVSync(int64_t *outTimestampNs, int64_t *outPeriodNs) override;

    /* Reporting off and the queue read dry. The controller's interrupt is
     * masked while nobody is waiting, and the kernel is not left holding a
     * growing list of blanks nobody will read. */
    void stop() override;

private:
    /* Reads everything queued without waiting and throws it away. */
    void discardQueued();

    TegraVSyncSource(std::unique_ptr<DcControl> control, DcHead &head,
                     uint32_t headHandle, const drmModeModeInfo &native)
        : mControl(std::move(control)),
          mHead(head),
          mHeadHandle(headHandle),
          mNative(native) {}

    /* How long a frame with front porch `actVfp` takes on this panel:
     * nought for the panel's own porch, the line count otherwise. Nought
     * when the timing carries no pixel clock to work it out from. */
    int64_t periodFor(uint32_t actVfp) const;

    std::unique_ptr<DcControl> mControl;
    DcHead &mHead;
    const uint32_t mHeadHandle;
    const drmModeModeInfo mNative;

    /* Whether a blank has ever reported a stretched porch. A kernel that
     * does not report the porch reports nought for ever, which reads the
     * same as the panel's own -- so the porch is believed only once it has
     * been seen to be something else, and until then the period is left
     * unsaid. */
    bool mPorchReported = false;

    /* Whether the controller was reporting blanks as of the last wait, or
     * granted the request to at the start of this one.
     *
     * What it decides is whether the request has to be made again: while
     * blanks are arriving it plainly still holds, and while they are not
     * there is no telling it from a request the driver has quietly dropped.
     *
     * And it decides what a wait is worth. While it holds, a wait that comes
     * up empty means something is wrong and is worth a real timeout to
     * establish; while it does not, waiting is pure delay -- the answer is
     * already known -- and the wait only looks, so that the caller can time
     * the blanks itself at the panel's rate instead of at the rate this
     * source fails.
     *
     * Only the reading thread touches it. */
    bool mReporting = false;

    /* Whether the last wait that trusted a grant came up empty. The driver
     * answers the request from a flag of its own, which it clears only when
     * the head is turned off through it; a grant given while the head was
     * on its way off is repeated for as long as the head stays off, and a
     * wait that trusted it would time out over and over, at a sixth of the
     * panel's rate. So a grant is trusted until it fails once, and after
     * that a blank has to be seen before a wait is worth its timeout. */
    bool mTimedOutWhileGranted = false;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_VSYNC_SOURCE_H
