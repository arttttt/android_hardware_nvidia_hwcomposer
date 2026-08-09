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

#include "DcControl.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* From the kernel tree, reached through include/video rather than copied
 * here; see Android.mk for why the path stops at that directory. */
#include <tegra_dc_ext.h>

#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-dc-control"

namespace android {
namespace hwc {

namespace {

constexpr char kControlNode[] = "/dev/tegra_dc_ctrl";

/* The kernel writes a header followed by the payload, and the two arrive in
 * a single read. Sized for the largest payload the controller can send, so
 * that one read always takes a whole event: a short read would desynchronise
 * the stream and every event after it would be misparsed. */
constexpr size_t kEventBufferSize = sizeof(struct tegra_dc_ext_event) + 256;

}  // namespace

std::unique_ptr<DcControl> DcControl::open() {
    UniqueFd fd(::open(kControlNode, O_RDWR | O_CLOEXEC));
    if (!fd) {
        HWC_LOGE("%s: %s", kControlNode, strerror(errno));
        return nullptr;
    }
    return std::unique_ptr<DcControl>(new DcControl(std::move(fd)));
}

DcControl::~DcControl() {
    /* Leaving a mask behind would keep the kernel queueing events for a
     * descriptor about to close. Failure here is not actionable. */
    setEventMask(0);
}

int DcControl::outputCount(uint32_t *outCount) const {
    __u32 count = 0;
    if (ioctl(mFd.get(), TEGRA_DC_EXT_CONTROL_GET_NUM_OUTPUTS, &count) < 0) {
        int err = -errno;
        HWC_LOGE("GET_NUM_OUTPUTS: %s", strerror(-err));
        return err;
    }
    *outCount = count;
    return 0;
}

int DcControl::setEventMask(uint32_t mask) {
    /* The mask goes by value, not by address.
     *
     * The ioctl is declared _IOW with a __u32, which says a pointer to one,
     * and every other call on this node does take a pointer. This one does
     * not: the driver reads the argument itself as the mask and rejects
     * anything outside the valid bits. Passing an address gave it a pointer
     * to validate, and it answered EINVAL -- correctly.
     */
    if (ioctl(mFd.get(), TEGRA_DC_EXT_CONTROL_SET_EVENT_MASK, mask) < 0) {
        int err = -errno;
        HWC_LOGE("SET_EVENT_MASK(0x%x): %s", mask, strerror(-err));
        return err;
    }
    return 0;
}

int DcControl::readEvent(Event *outEvent) {
    char buffer[kEventBufferSize];

    ssize_t got = TEMP_FAILURE_RETRY(::read(mFd.get(), buffer, sizeof(buffer)));
    if (got < 0)
        return -errno;
    if (static_cast<size_t>(got) < sizeof(struct tegra_dc_ext_event)) {
        HWC_LOGE("short event: %zd bytes", got);
        return -EIO;
    }

    const auto *header = reinterpret_cast<const struct tegra_dc_ext_event *>(buffer);
    const size_t payloadAvailable = static_cast<size_t>(got) - sizeof(*header);
    if (header->data_size > payloadAvailable) {
        HWC_LOGE("event type 0x%x claims %u payload bytes, %zu present",
              header->type, header->data_size, payloadAvailable);
        return -EIO;
    }

    *outEvent = Event{};

    switch (header->type) {
    case TEGRA_DC_EXT_EVENT_VBLANK: {
        if (header->data_size < sizeof(struct tegra_dc_ext_control_event_vblank))
            return -EIO;
        const auto *vblank =
            reinterpret_cast<const struct tegra_dc_ext_control_event_vblank *>(
                header->data);
        outEvent->type = EventType::VBlank;
        outEvent->handle = vblank->handle;
        outEvent->timestampNs = static_cast<int64_t>(vblank->timestamp_ns);
        break;
    }
    case TEGRA_DC_EXT_EVENT_HOTPLUG: {
        if (header->data_size < sizeof(struct tegra_dc_ext_control_event_hotplug))
            return -EIO;
        const auto *hotplug =
            reinterpret_cast<const struct tegra_dc_ext_control_event_hotplug *>(
                header->data);
        outEvent->type = EventType::Hotplug;
        outEvent->handle = hotplug->handle;
        outEvent->connected = hotplug->connected != 0;
        break;
    }
    default:
        /* Bandwidth notifications land here. Reported as Unknown rather than
         * as an error: the read consumed a whole event, so the stream is
         * still aligned and the caller simply has nothing to do. */
        outEvent->type = EventType::Unknown;
        break;
    }

    return 0;
}

}  // namespace hwc
}  // namespace android
