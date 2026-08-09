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
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

/* From the kernel tree; see Android.mk for why the include path stops at
 * include/video rather than include/. */
#include <tegra_dc_ext.h>

#include "utils/Logging.h"

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

/* How far up to look for windows. No generation of this controller has more
 * than six, and asking for one past the end costs a single refused ioctl at
 * startup. */
constexpr uint32_t kWindowSearchLimit = 6;

/* How the controller spells its feature table.
 *
 * The call that returns the table is in the interface the kernel publishes,
 * but the shape of what it returns is not: the codes below live in a header
 * private to the driver (drivers/video/tegra/dc/dc_config.h) and are repeated
 * here rather than reached for, because a composer has no business including
 * a driver's private headers. They are read from the same kernel this builds
 * against, and a mismatch would show as nonsense in the line logged at
 * start-up.
 */
constexpr size_t kEntryArgs = 4;

constexpr uint32_t kFeatureFormats = 0;
constexpr uint32_t kFeatureMaximumSize = 2;
constexpr uint32_t kFeatureMaximumScale = 3;
constexpr uint32_t kFeatureLayoutType = 5;
constexpr uint32_t kFeatureInvertType = 6;

constexpr size_t kSizeMaxWidth = 0;
constexpr size_t kSizeMinWidth = 1;
constexpr size_t kSizeMaxHeight = 2;
constexpr size_t kSizeMinHeight = 3;

constexpr size_t kLayoutPitched = 0;
constexpr size_t kLayoutTiled = 1;
constexpr size_t kLayoutBlockLinear = 2;

constexpr size_t kInvertH = 0;
constexpr size_t kInvertV = 1;
constexpr size_t kInvertScanColumn = 2;

}  // namespace

std::unique_ptr<DcHead> DcHead::open(int index) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/tegra_dc_%d", index);

    UniqueFd fd(::open(path, O_RDWR | O_CLOEXEC));
    if (!fd) {
        HWC_LOGE("%s: %s", path, strerror(errno));
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

const std::vector<uint32_t> &DcHead::windows() {
    if (mWindowsDiscovered)
        return mOwnedWindows;

    /* Marked done first: a head with no windows at all is an answer too, and
     * repeating the search every frame would not change it. */
    mWindowsDiscovered = true;

    for (uint32_t index = 0; index < kWindowSearchLimit; ++index) {
        if (tryClaimWindow(index))
            mOwnedWindows.push_back(index);
    }

    if (mOwnedWindows.empty()) {
        HWC_LOGE("head %d has no windows to give", mIndex);
        return mOwnedWindows;
    }

    /* One line rather than one per window: this is startup, and the set is
     * small enough to read at a glance. */
    char list[kWindowSearchLimit * 3 + 1];
    size_t used = 0;
    for (uint32_t index : mOwnedWindows)
        used += snprintf(list + used, sizeof(list) - used, "%s%u",
                         used ? "," : "", index);

    HWC_LOGI("head %d owns window(s) %s", mIndex, list);
    return mOwnedWindows;
}

bool DcHead::tryClaimWindow(uint32_t index) {
    return ioctl(mFd.get(), TEGRA_DC_EXT_GET_WINDOW, index) == 0;
}

const DcHead::WindowCapabilities *DcHead::capabilities(uint32_t index) {
    if (!mCapabilitiesRead) {
        mCapabilitiesRead = true;
        if (!readCapabilities())
            HWC_LOGE("head %d would not say what its windows can do", mIndex);
    }

    auto it = mCapabilities.find(index);
    return it == mCapabilities.end() ? nullptr : &it->second;
}

bool DcHead::readCapabilities() {
    /* The controller hands back its whole feature table in one go: a run of
     * entries, each naming a window, a property of it, and up to four values.
     *
     * There is no way to ask how long the table is first -- the call fills
     * whatever it is given and reports the length afterwards -- so the buffer
     * is made large enough for any table this driver builds. A table for one
     * head is a few entries per window.
     */
    constexpr size_t kMaxEntries = 256;

    struct Entry {
        __u32 window;
        __u32 option;
        __u32 arg[kEntryArgs];
    };

    std::vector<Entry> entries(kMaxEntries);

    struct tegra_dc_ext_feature request;
    memset(&request, 0, sizeof(request));
    request.length = kMaxEntries;
    request.entries = reinterpret_cast<__u32 *>(entries.data());

    if (ioctl(mFd.get(), TEGRA_DC_EXT_GET_FEATURES, &request) < 0) {
        HWC_LOGE("head %d: GET_FEATURES: %s", mIndex, strerror(errno));
        return false;
    }

    if (request.length == 0 || request.length > kMaxEntries) {
        HWC_LOGE("head %d: feature table of %u entries", mIndex,
                 request.length);
        return false;
    }

    for (size_t i = 0; i < request.length; ++i) {
        const Entry &entry = entries[i];
        WindowCapabilities &caps = mCapabilities[entry.window];

        switch (entry.option) {
        case kFeatureFormats:
            /* Two words, low half then high, one bit per format code. */
            caps.formats = static_cast<uint64_t>(entry.arg[0]) |
                           (static_cast<uint64_t>(entry.arg[1]) << 32);
            break;
        case kFeatureMaximumSize:
            caps.maxWidth = entry.arg[kSizeMaxWidth];
            caps.minWidth = entry.arg[kSizeMinWidth];
            caps.maxHeight = entry.arg[kSizeMaxHeight];
            caps.minHeight = entry.arg[kSizeMinHeight];
            break;
        case kFeatureMaximumScale:
            /* All four ratios are one where the window cannot resize. */
            caps.scaling = entry.arg[0] != 1 || entry.arg[1] != 1 ||
                           entry.arg[2] != 1 || entry.arg[3] != 1;
            break;
        case kFeatureLayoutType:
            caps.pitchLayout = entry.arg[kLayoutPitched] != 0;
            caps.tiledLayout = entry.arg[kLayoutTiled] != 0;
            caps.blocklinearLayout = entry.arg[kLayoutBlockLinear] != 0;
            break;
        case kFeatureInvertType:
            caps.invertH = entry.arg[kInvertH] != 0;
            caps.invertV = entry.arg[kInvertV] != 0;
            caps.scanColumn = entry.arg[kInvertScanColumn] != 0;
            break;
        default:
            /* Colour conversion, filtering, field order, rotation formats.
             * Nothing here plans around them yet, and an entry nobody reads
             * is not an error. */
            break;
        }
    }

    for (const auto &entry : mCapabilities) {
        HWC_LOGI("head %d: win %u formats 0x%" PRIx64 " up to %ux%u%s%s%s",
                 mIndex, entry.first, entry.second.formats,
                 entry.second.maxWidth, entry.second.maxHeight,
                 entry.second.blocklinearLayout ? " blocklinear" : "",
                 entry.second.tiledLayout ? " tiled" : "",
                 entry.second.scaling ? " scaling" : "");
    }

    return true;
}

void DcHead::releaseWindow(uint32_t index) {
    auto it = std::find(mOwnedWindows.begin(), mOwnedWindows.end(), index);
    if (it == mOwnedWindows.end())
        return;

    /* Dropped from the list first: the ioctl blanks the window, and leaving
     * it listed after a failure would have the destructor try again on a
     * window the driver may already consider gone. */
    mOwnedWindows.erase(it);

    if (ioctl(mFd.get(), TEGRA_DC_EXT_PUT_WINDOW, index) < 0)
        HWC_LOGE("head %d: PUT_WINDOW(%u): %s", mIndex, index, strerror(errno));
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
        dst.block_height_log2 = src.blockHeightLog2;
        dst.z = src.z;

        dst.x = toFixed(src.sourceX);
        dst.y = toFixed(src.sourceY);
        dst.w = toFixed(src.sourceWidth);
        dst.h = toFixed(src.sourceHeight);

        dst.out_x = static_cast<__u32>(src.outX);
        dst.out_y = static_cast<__u32>(src.outY);
        dst.out_w = static_cast<__u32>(src.outWidth);
        dst.out_h = static_cast<__u32>(src.outHeight);

        /* Take the new contents at the end of a frame rather than the end of
         * a line.
         *
         * Left at zero the driver treats an update that changes nothing but
         * the address as safe to apply between two scanlines, and applies it
         * there -- so the panel finishes the frame it started from one buffer
         * and out of the next. That is a tear, and it shows up exactly on the
         * frames where consecutive images differ enough to notice.
         *
         * Any non-zero interval asks for the vertical blank instead, which
         * costs at most the remainder of the frame already being drawn. A
         * composer has no use for the other trade. */
        dst.swap_interval = 1;

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

    for (const Window &window : windows) {
        HWC_LOGD("head %d: win %d buf=%d off=%u stride=%u fmt=0x%x flags=0x%x "
                 "blockh=%u src=%.1fx%.1f+%.1f+%.1f dst=%dx%d+%d+%d z=%u "
                 "pre=%d",
                 mIndex, window.index, window.bufferFd, window.offset,
                 window.stride, window.pixelFormat, window.flags,
                 window.blockHeightLog2, window.sourceWidth,
                 window.sourceHeight, window.sourceX, window.sourceY,
                 window.outWidth, window.outHeight, window.outX, window.outY,
                 window.z, window.preFence);
    }

    if (ioctl(mFd.get(), TEGRA_DC_EXT_FLIP3, &flip) < 0) {
        int err = -errno;
        HWC_LOGE("head %d: FLIP3 with %zu window(s): %s", mIndex,
                 windows.size(), strerror(-err));
        return err;
    }

    HWC_LOGD("head %d: flipped %zu window(s), post fence %d", mIndex,
             windows.size(), flip.post_syncpt_fd);

    outPostFence->reset(flip.post_syncpt_fd);
    return 0;
}

}  // namespace hwc
}  // namespace android
