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

#include "FbMode.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <utils/Log.h>

#include "utils/UniqueFd.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-fb-mode"

namespace android {
namespace hwc {

namespace {

/* Used when the panel reports no usable timing. Sixty is not a guess about
 * this hardware so much as the value every consumer copes with; a composer
 * that reported zero would have the framework dividing by it. */
constexpr int32_t kFallbackVsyncPeriodNs = 16'666'667;

constexpr float kMillimetresPerInch = 25.4f;

/* The framework wants density in thousandths of a pixel per inch. */
int32_t densityFromSize(uint32_t pixels, uint32_t millimetres) {
    if (pixels == 0 || millimetres == 0)
        return 0;
    return static_cast<int32_t>(
        (static_cast<float>(pixels) * kMillimetresPerInch * 1000.f) /
        static_cast<float>(millimetres));
}

/* Refresh comes out of the timing, not out of a field: the pixel clock is
 * picoseconds per pixel, and one frame is every pixel the panel clocks
 * including the blanking it spends not drawing. */
int32_t vsyncPeriodFrom(const struct fb_var_screeninfo &info) {
    if (info.pixclock == 0)
        return kFallbackVsyncPeriodNs;

    const uint64_t totalWidth =
        info.xres + info.left_margin + info.right_margin + info.hsync_len;
    const uint64_t totalHeight =
        info.yres + info.upper_margin + info.lower_margin + info.vsync_len;

    const uint64_t picoseconds =
        static_cast<uint64_t>(info.pixclock) * totalWidth * totalHeight;
    const uint64_t nanoseconds = picoseconds / 1000;

    if (nanoseconds == 0 || nanoseconds > INT32_MAX)
        return kFallbackVsyncPeriodNs;

    return static_cast<int32_t>(nanoseconds);
}

}  // namespace

int readDisplayMode(int index, DisplayMode *outMode) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/graphics/fb%d", index);

    UniqueFd fd(::open(path, O_RDONLY | O_CLOEXEC));
    if (!fd) {
        int err = -errno;
        ALOGE("%s: %s", path, strerror(-err));
        return err;
    }

    struct fb_var_screeninfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd.get(), FBIOGET_VSCREENINFO, &info) < 0) {
        int err = -errno;
        ALOGE("%s: FBIOGET_VSCREENINFO: %s", path, strerror(-err));
        return err;
    }

    if (info.xres == 0 || info.yres == 0) {
        ALOGE("%s: reports a %ux%u panel", path, info.xres, info.yres);
        return -EINVAL;
    }

    outMode->width = static_cast<int32_t>(info.xres);
    outMode->height = static_cast<int32_t>(info.yres);
    outMode->vsyncPeriodNs = vsyncPeriodFrom(info);
    outMode->dpiX = densityFromSize(info.xres, info.width);
    outMode->dpiY = densityFromSize(info.yres, info.height);

    ALOGI("%s: %dx%d, %.2f Hz, density %d/%d", path, outMode->width,
          outMode->height, 1e9f / static_cast<float>(outMode->vsyncPeriodNs),
          outMode->dpiX, outMode->dpiY);

    return 0;
}

}  // namespace hwc
}  // namespace android
