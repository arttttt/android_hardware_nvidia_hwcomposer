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

#include "DcHead.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

/* From the kernel tree; see Android.mk for why the include path stops at
 * include/video rather than include/. */
#include <tegra_dc_ext.h>

#include <utils/Log.h>

#undef  LOG_TAG
#define LOG_TAG "hwc-dc-head"

namespace android {
namespace hwc {

namespace {

/* Source coordinates are 20.12 fixed point: twenty integer bits, twelve
 * fractional. The framework hands us floats, so the conversion belongs here
 * rather than in every caller. */
constexpr int kFixedPointShift = 12;

uint32_t toFixed(float value) {
    if (value < 0.f)
        value = 0.f;
    return static_cast<uint32_t>(value * (1 << kFixedPointShift));
}

}  // namespace

std::unique_ptr<DcHead> DcHead::open(int index) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/tegra_dc_%d", index);

    UniqueFd fd(::open(path, O_RDWR | O_CLOEXEC));
    if (!fd) {
        ALOGE("%s: %s", path, strerror(errno));
        return nullptr;
    }
    return std::unique_ptr<DcHead>(new DcHead(std::move(fd), index));
}

DcHead::~DcHead() {
    /* Copied, because releaseWindow edits the list it walks. */
    const std::vector<uint32_t> owned = mOwnedWindows;
    for (uint32_t window : owned)
        releaseWindow(window);
}

int DcHead::claimWindow(uint32_t index) {
    if (std::find(mOwnedWindows.begin(), mOwnedWindows.end(), index) !=
        mOwnedWindows.end())
        return 0;

    if (ioctl(mFd.get(), TEGRA_DC_EXT_GET_WINDOW, index) < 0) {
        int err = -errno;
        ALOGE("head %d: GET_WINDOW(%u): %s", mIndex, index, strerror(-err));
        return err;
    }

    mOwnedWindows.push_back(index);
    return 0;
}

int DcHead::releaseWindow(uint32_t index) {
    auto it = std::find(mOwnedWindows.begin(), mOwnedWindows.end(), index);
    if (it == mOwnedWindows.end())
        return 0;

    /* Dropped from the list first: the ioctl blanks the window, and leaving
     * it listed after a failure would have the destructor try again on a
     * window the driver may already consider gone. */
    mOwnedWindows.erase(it);

    if (ioctl(mFd.get(), TEGRA_DC_EXT_PUT_WINDOW, index) < 0) {
        int err = -errno;
        ALOGE("head %d: PUT_WINDOW(%u): %s", mIndex, index, strerror(-err));
        return err;
    }
    return 0;
}

int DcHead::flip(const std::vector<Window> &windows, UniqueFd *outPostFence) {
    if (windows.empty())
        return -EINVAL;

    std::vector<struct tegra_dc_ext_flip_windowattr> attrs(windows.size());

    for (size_t i = 0; i < windows.size(); ++i) {
        const Window &src = windows[i];
        struct tegra_dc_ext_flip_windowattr &dst = attrs[i];

        memset(&dst, 0, sizeof(dst));

        dst.index = src.index;
        dst.buff_id = static_cast<__u32>(src.bufferFd);
        dst.offset = src.offset;
        dst.stride = src.stride;
        dst.pixformat = src.pixelFormat;
        dst.blend = src.blend;
        dst.flags = src.flags;
        dst.z = src.z;

        dst.x = toFixed(src.sourceX);
        dst.y = toFixed(src.sourceY);
        dst.w = toFixed(src.sourceWidth);
        dst.h = toFixed(src.sourceHeight);

        dst.out_x = static_cast<__u32>(src.outX);
        dst.out_y = static_cast<__u32>(src.outY);
        dst.out_w = static_cast<__u32>(src.outWidth);
        dst.out_h = static_cast<__u32>(src.outHeight);

        /* The union here is either a syncpoint id and value pair or a
         * descriptor. This flip always means the descriptor: the driver
         * decides by whether the caller wants a post fence back, and this
         * ioctl always asks for one. -1 means the buffer is ready now. */
        dst.pre_syncpt_fd = src.preFence;
    }

    struct tegra_dc_ext_flip_3 flip;
    memset(&flip, 0, sizeof(flip));

    flip.win = reinterpret_cast<__u64>(attrs.data());
    flip.win_num = static_cast<__u8>(attrs.size());
    flip.post_syncpt_fd = -1;

    /* No head flags. The two that exist here select YUV bypass and a second,
     * wider window structure; sending the wider flag with the structure this
     * code fills would have the driver parse every field at the wrong
     * offset. */
    flip.flags = 0;

    if (ioctl(mFd.get(), TEGRA_DC_EXT_FLIP3, &flip) < 0) {
        int err = -errno;
        ALOGE("head %d: FLIP3 with %zu window(s): %s", mIndex, windows.size(),
              strerror(-err));
        return err;
    }

    outPostFence->reset(flip.post_syncpt_fd);
    return 0;
}

}  // namespace hwc
}  // namespace android
