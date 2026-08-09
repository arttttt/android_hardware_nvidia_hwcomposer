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

#include "BufferInfo.h"

#include <errno.h>

#include <hardware/hardware.h>
#include <system/graphics.h>

#include <tegra_dc_ext.h>

#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-buffer"

/* The allocator's own interface, declared here rather than taken from its
 * header.
 *
 * The header that ships with the leaked sources describes a build older than
 * the library on this device -- it does not even declare nvgr_get_stride,
 * which the library exports. Including it would drag in that build's idea of
 * the surface structure and of the handle, and those are exactly the things
 * that have changed. Four functions with a plain C signature are all this
 * needs, and their names have survived every version seen so far.
 */
extern "C" {
int nvgr_is_valid(buffer_handle_t handle);
int nvgr_get_memfd(buffer_handle_t handle);
int nvgr_get_format(buffer_handle_t handle);
int nvgr_get_stride(buffer_handle_t handle);
}

namespace android {
namespace hwc {

namespace {

/* Bytes each pixel occupies, for the formats a display can scan out. Zero
 * for anything else, which is how the caller learns it cannot. */
uint32_t bytesPerPixel(int halFormat) {
    switch (halFormat) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
        return 4;
    case HAL_PIXEL_FORMAT_RGB_565:
        return 2;
    default:
        return 0;
    }
}

/* The controller's format code for a framework format.
 *
 * The pairing is not guessable from the names: the framework names a format
 * by the order of its bytes in memory, the controller by the order of its
 * channels in a word, so the two read as mirror images of each other and
 * agree anyway. Taken from how the vendor's own composer mapped them.
 */
uint32_t tegraFormat(int halFormat) {
    switch (halFormat) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
        return TEGRA_DC_EXT_FMT_R8G8B8A8;
    case HAL_PIXEL_FORMAT_BGRA_8888:
        return TEGRA_DC_EXT_FMT_B8G8R8A8;
    case HAL_PIXEL_FORMAT_RGB_565:
        return TEGRA_DC_EXT_FMT_B5G6R5;
    default:
        return 0;
    }
}

}  // namespace

int describeBuffer(buffer_handle_t handle, uint32_t width, BufferInfo *outInfo) {
    if (handle == nullptr)
        return -EINVAL;

    if (!nvgr_is_valid(handle)) {
        HWC_LOGE("buffer %p is not one of the allocator's", handle);
        return -EINVAL;
    }

    const int halFormat = nvgr_get_format(handle);
    const uint32_t bpp = bytesPerPixel(halFormat);
    if (bpp == 0) {
        HWC_LOGE("format 0x%x cannot be scanned out", halFormat);
        return -EINVAL;
    }

    const int fd = nvgr_get_memfd(handle);
    if (fd < 0) {
        HWC_LOGE("buffer %p has no memory descriptor", handle);
        return -EINVAL;
    }

    const int stride = nvgr_get_stride(handle);
    if (stride <= 0) {
        HWC_LOGE("buffer %p reports a stride of %d", handle, stride);
        return -EINVAL;
    }

    /* The allocator counts a row in pixels, as the framework does; the
     * controller counts it in bytes. A row cannot be narrower than the image
     * it holds, so a value already at least that wide in bytes is taken as
     * bytes and anything smaller as pixels. Both cases are logged, because
     * this is the one number here that is inferred rather than asked for. */
    uint32_t strideBytes;
    if (static_cast<uint32_t>(stride) >= width * bpp) {
        strideBytes = static_cast<uint32_t>(stride);
        HWC_LOGD("buffer %p: stride %d read as bytes", handle, stride);
    } else {
        strideBytes = static_cast<uint32_t>(stride) * bpp;
        HWC_LOGD("buffer %p: stride %d pixels, %u bytes", handle, stride,
                 strideBytes);
    }

    if (strideBytes < width * bpp) {
        HWC_LOGE("buffer %p: a row of %u bytes cannot hold %u pixels at %u "
                 "bytes each", handle, strideBytes, width, bpp);
        return -EINVAL;
    }

    outInfo->fd = fd;
    outInfo->offset = 0;
    outInfo->strideBytes = strideBytes;
    outInfo->format = tegraFormat(halFormat);
    outInfo->flags = 0;
    outInfo->blockHeightLog2 = 0;

    HWC_LOGD("buffer %p: fd=%d fmt=0x%x -> 0x%x stride=%u", handle, fd,
             halFormat, outInfo->format, strideBytes);

    return 0;
}

}  // namespace hwc
}  // namespace android
