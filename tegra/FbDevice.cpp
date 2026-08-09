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

#include "FbDevice.h"

#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "utils/Logging.h"

#include "utils/UniqueFd.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-fb-device"

namespace android {
namespace hwc {

namespace {

/* Used when the panel reports no pixel clock. Sixty is not a guess about this
 * hardware so much as the value every consumer copes with; a composer that
 * reported zero would have the framework dividing by it. */
constexpr uint32_t kFallbackVRefresh = 60;

/* A pixel clock is quoted in picoseconds per pixel here and in kilohertz
 * there. Pixels per second is 1e12 divided by the one, and a thousand of
 * those is the other. */
constexpr uint64_t kPicosecondsPerKilopixel = 1'000'000'000;

/* The two ways a synchronisation pulse can be active, in both vocabularies.
 * A framebuffer device says which are active high and leaves the rest to be
 * read as active low; a mode says both explicitly. */
uint32_t modeFlagsFrom(const struct fb_var_screeninfo &info) {
    uint32_t flags = 0;

    flags |= (info.sync & FB_SYNC_HOR_HIGH_ACT) ? DRM_MODE_FLAG_PHSYNC
                                                : DRM_MODE_FLAG_NHSYNC;
    flags |= (info.sync & FB_SYNC_VERT_HIGH_ACT) ? DRM_MODE_FLAG_PVSYNC
                                                 : DRM_MODE_FLAG_NVSYNC;

    if ((info.vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED)
        flags |= DRM_MODE_FLAG_INTERLACE;
    if ((info.vmode & FB_VMODE_MASK) == FB_VMODE_DOUBLE)
        flags |= DRM_MODE_FLAG_DBLSCAN;

    return flags;
}

/* Both descriptions carry the same six numbers per axis; only the naming and
 * the order differ. A framebuffer device counts outwards from the active
 * area -- so much before the pulse, so much of it, so much after -- while a
 * mode counts positions along the line. Nothing is computed here that the
 * panel did not report. */
void fillTiming(const struct fb_var_screeninfo &info, drmModeModeInfo *mode) {
    mode->hdisplay = static_cast<uint16_t>(info.xres);
    mode->hsync_start = static_cast<uint16_t>(mode->hdisplay +
                                              info.right_margin);
    mode->hsync_end = static_cast<uint16_t>(mode->hsync_start + info.hsync_len);
    mode->htotal = static_cast<uint16_t>(mode->hsync_end + info.left_margin);

    mode->vdisplay = static_cast<uint16_t>(info.yres);
    mode->vsync_start = static_cast<uint16_t>(mode->vdisplay +
                                              info.lower_margin);
    mode->vsync_end = static_cast<uint16_t>(mode->vsync_start + info.vsync_len);
    mode->vtotal = static_cast<uint16_t>(mode->vsync_end + info.upper_margin);

    mode->flags = modeFlagsFrom(info);

    /* The panel is soldered to this board and is the display this composer
     * comes up in. Both of those are exactly what the two marks mean. */
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    snprintf(mode->name, sizeof(mode->name), "%ux%u", info.xres, info.yres);

    /* Refresh is left to be worked out from the clock and the totals, which
     * is what every consumer of this structure does. Where there is no clock
     * there is nothing to work it out from, and the field below is what gets
     * read instead. */
    const uint64_t pixels = static_cast<uint64_t>(mode->htotal) * mode->vtotal;

    if (info.pixclock == 0 || pixels == 0) {
        HWC_LOGW("panel reports no pixel clock; assuming %u Hz",
                 kFallbackVRefresh);
        mode->clock = 0;
        mode->vrefresh = kFallbackVRefresh;
        return;
    }

    mode->clock = static_cast<uint32_t>(kPicosecondsPerKilopixel /
                                        info.pixclock);
    mode->vrefresh = static_cast<uint32_t>(
        (static_cast<uint64_t>(mode->clock) * 1000 + pixels / 2) / pixels);
}

}  // namespace

namespace {

UniqueFd openFb(int index, int flags, char *pathOut, size_t pathSize) {
    snprintf(pathOut, pathSize, "/dev/graphics/fb%d", index);
    return UniqueFd(::open(pathOut, flags | O_CLOEXEC));
}

}  // namespace

int readPanelTiming(int index, PanelTiming *outTiming) {
    char path[32];
    UniqueFd fd = openFb(index, O_RDONLY, path, sizeof(path));
    if (!fd) {
        int err = -errno;
        HWC_LOGE("%s: %s", path, strerror(-err));
        return err;
    }

    struct fb_var_screeninfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd.get(), FBIOGET_VSCREENINFO, &info) < 0) {
        int err = -errno;
        HWC_LOGE("%s: FBIOGET_VSCREENINFO: %s", path, strerror(-err));
        return err;
    }

    if (info.xres == 0 || info.yres == 0) {
        HWC_LOGE("%s: reports a %ux%u panel", path, info.xres, info.yres);
        return -EINVAL;
    }

    memset(&outTiming->mode, 0, sizeof(outTiming->mode));
    fillTiming(info, &outTiming->mode);

    /* Zero where the panel does not say. A display of unknown size is a fact
     * the layers above are equipped to be told; a plausible number is not. */
    outTiming->mmWidth = info.width;
    outTiming->mmHeight = info.height;

    const drmModeModeInfo &mode = outTiming->mode;
    HWC_LOGI("%s: %s %ux%u total, %u kHz, ~%u Hz, %ux%u mm", path, mode.name,
             mode.htotal, mode.vtotal, mode.clock, mode.vrefresh,
             outTiming->mmWidth, outTiming->mmHeight);

    return 0;
}

int setPanelPowered(int index, bool powered) {
    char path[32];

    /* Write access, unlike the mode read: blanking changes the hardware. */
    UniqueFd fd = openFb(index, O_RDWR, path, sizeof(path));
    if (!fd) {
        int err = -errno;
        HWC_LOGE("%s: %s", path, strerror(-err));
        return err;
    }

    /* POWERDOWN rather than one of the two intermediate levels. Those exist
     * for displays that can drop their sync signals and keep the panel warm,
     * which saves the power a backlight uses and none of the rest. This one
     * has two useful states. */
    const int level = powered ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;

    if (ioctl(fd.get(), FBIOBLANK, level) < 0) {
        int err = -errno;
        HWC_LOGE("%s: FBIOBLANK(%d): %s", path, level, strerror(-err));
        return err;
    }

    HWC_LOGI("%s: panel %s", path, powered ? "on" : "off");
    return 0;
}

}  // namespace hwc
}  // namespace android
