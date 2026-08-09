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

#include <dlfcn.h>
#include <errno.h>

#include <hardware/hardware.h>
#include <system/graphics.h>

#include <tegra_dc_ext.h>

#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-buffer"

namespace android {
namespace hwc {

namespace {

/* The allocator's own interface, resolved at run time.
 *
 * Not linked against, for two reasons. The library is declared in the vendor
 * blob repository by installation path rather than as a vendor module, so the
 * build has nothing to link a vendor module against; and that repository is
 * shared with the branch that already works, which is not a thing to change
 * for our convenience.
 *
 * Three functions, all of them plain C, and their names have outlived every
 * version of the library seen on this hardware.
 */
struct Allocator {
    int (*isValid)(buffer_handle_t) = nullptr;
    int (*memFd)(buffer_handle_t) = nullptr;
    int (*format)(buffer_handle_t) = nullptr;

    /* Hands back the surface descriptors the allocator keeps for a buffer.
     * Everything the display controller needs about the memory -- where the
     * image starts, how long a row is, how it is arranged -- is in there, and
     * nothing else reports it. */
    void (*surfaces)(buffer_handle_t, const void **, size_t *) = nullptr;

    /* Whether the buffer's colour is still compressed, and the undoing of it.
     * The second takes the fence it is given and returns the one to wait on
     * instead. */
    int (*compressed)(buffer_handle_t) = nullptr;
    int (*decompress)(buffer_handle_t, int, int *) = nullptr;

    bool resolved = false;
    bool failed = false;
};

Allocator gAllocator;

template <typename Fn>
bool resolveOne(void *library, const char *name, Fn *slot) {
    *slot = reinterpret_cast<Fn>(dlsym(library, name));
    if (*slot == nullptr) {
        HWC_LOGE("libnvgr has no %s", name);
        return false;
    }
    return true;
}

/* Resolved once. A second attempt after a failure would fail the same way
 * and log the same lines on every frame. */
const Allocator *allocator() {
    if (gAllocator.resolved)
        return gAllocator.failed ? nullptr : &gAllocator;

    gAllocator.resolved = true;

    void *library = dlopen("libnvgr.so", RTLD_NOW);
    if (library == nullptr) {
        HWC_LOGE("libnvgr.so: %s", dlerror());
        gAllocator.failed = true;
        return nullptr;
    }

    const bool ok = resolveOne(library, "nvgr_is_valid", &gAllocator.isValid) &&
                    resolveOne(library, "nvgr_get_memfd", &gAllocator.memFd) &&
                    resolveOne(library, "nvgr_get_format", &gAllocator.format) &&
                    resolveOne(library, "nvgr_get_surfaces", &gAllocator.surfaces) &&
                    resolveOne(library, "nvgr_get_compressed", &gAllocator.compressed) &&
                    resolveOne(library, "nvgr_decompress", &gAllocator.decompress);
    if (!ok) {
        gAllocator.failed = true;
        return nullptr;
    }

    HWC_LOGI("libnvgr resolved");
    return &gAllocator;
}

/* The surface descriptor, as words.
 *
 * It is a flat run of thirty-two bit fields, and reading it by index rather
 * than through a declared structure is deliberate: the structure belongs to
 * the allocator and has gained a field since the last version of it published
 * with source, so a header copied from there would put every field after the
 * fourth word at the wrong offset. Indices instead, each one established
 * against the library and the buffers on this device:
 *
 *   - Colour format and pitch are the two the library itself reveals:
 *     nvgr_get_stride reads the third and the sixth word and divides one by
 *     the bytes-per-pixel packed into the other.
 *   - Width and height are self-evident in a dump -- they are the panel's.
 *   - Kind reads as the memory kind for compressible thirty-two bit colour,
 *     which no other field of this buffer could plausibly be, and it fixes
 *     everything around it: layout says blocklinear in the word before the
 *     pitch, and the block height beside the kind is a sane four.
 *
 * Anything that disagrees with itself is refused rather than guessed at --
 * see checkSurface below.
 */
enum SurfaceWord {
    kWidth = 0,
    kHeight = 1,
    kColorFormat = 2,
    kLayout = 4,
    kPitch = 5,
    kOffset = 7,
    kKind = 8,
    kBlockHeightLog2 = 9,
};

/* The allocator's own names for how memory is arranged. */
enum SurfaceLayout {
    kLayoutPitch = 1,
    kLayoutTiled = 2,
    kLayoutBlocklinear = 3,
};

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

/* Does the descriptor read as one?
 *
 * The word indices above were established against one build of the allocator,
 * and a different one would shift them. A shifted reading does not look like
 * an error, it looks like a plausible buffer of the wrong shape, and scanning
 * that out shows the user a broken picture with nothing in the log. So the
 * reading is asked to agree with itself first: a row cannot be shorter than
 * the pixels it holds, dimensions cannot be absent, and the layout has to be
 * one the allocator has a name for. Under a shift each of those lands on a
 * field that means something else entirely, and the odds of all three still
 * holding are slim.
 */
bool checkSurface(const uint32_t *word, uint32_t bpp) {
    if (word[kWidth] == 0 || word[kHeight] == 0) {
        HWC_LOGE("surface reads as %ux%u", word[kWidth], word[kHeight]);
        return false;
    }

    if (word[kPitch] < word[kWidth] * bpp) {
        HWC_LOGE("surface row of %u bytes cannot hold %u pixels at %u bytes "
                 "each", word[kPitch], word[kWidth], bpp);
        return false;
    }

    if (word[kLayout] < kLayoutPitch || word[kLayout] > kLayoutBlocklinear) {
        HWC_LOGE("surface layout reads as %u", word[kLayout]);
        return false;
    }

    return true;
}

}  // namespace

int describeBuffer(buffer_handle_t handle, BufferInfo *outInfo) {
    if (handle == nullptr)
        return -EINVAL;

    const Allocator *nvgr = allocator();
    if (nvgr == nullptr)
        return -ENOSYS;

    if (!nvgr->isValid(handle)) {
        HWC_LOGE("buffer %p is not one of the allocator's", handle);
        return -EINVAL;
    }

    const int halFormat = nvgr->format(handle);
    const uint32_t bpp = bytesPerPixel(halFormat);
    if (bpp == 0) {
        HWC_LOGE("format 0x%x cannot be scanned out", halFormat);
        return -EINVAL;
    }

    const int fd = nvgr->memFd(handle);
    if (fd < 0) {
        HWC_LOGE("buffer %p has no memory descriptor", handle);
        return -EINVAL;
    }

    const void *surfaces = nullptr;
    size_t count = 0;
    nvgr->surfaces(handle, &surfaces, &count);

    if (surfaces == nullptr || count == 0) {
        HWC_LOGE("buffer %p has no surfaces", handle);
        return -EINVAL;
    }

    /* The first surface only. A second one would carry chroma for a planar
     * format, and those are not scanned out here yet. */
    const uint32_t *word = static_cast<const uint32_t *>(surfaces);
    if (!checkSurface(word, bpp))
        return -EINVAL;

    *outInfo = BufferInfo{};

    outInfo->fd = fd;
    outInfo->offset = word[kOffset];
    outInfo->strideBytes = word[kPitch];
    outInfo->format = tegraFormat(halFormat);

    /* Blocklinear is what the GPU renders into by default on this hardware,
     * so this is the ordinary case rather than the exotic one. Told to read
     * such memory row by row the controller shows an orderly scramble, which
     * is the least helpful way for this to go wrong. */
    if (word[kLayout] == kLayoutBlocklinear) {
        outInfo->flags |= TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR;
        outInfo->blockHeightLog2 = static_cast<uint8_t>(word[kBlockHeightLog2]);
    } else if (word[kLayout] == kLayoutTiled) {
        outInfo->flags |= TEGRA_DC_EXT_FLIP_FLAG_TILED;
    }

    HWC_LOGD("buffer %p: fd=%d %ux%u fmt=0x%x -> 0x%x pitch=%u offset=%u "
             "layout=%u kind=0x%x blockh=%u compressed=%d", handle, fd,
             word[kWidth], word[kHeight], halFormat, outInfo->format,
             word[kPitch], word[kOffset], word[kLayout], word[kKind],
             word[kBlockHeightLog2], nvgr->compressed(handle));

    return 0;
}

void prepareForScanout(buffer_handle_t handle, int acquireFence,
                       UniqueFd *outFence) {
    const Allocator *nvgr = allocator();
    if (nvgr == nullptr) {
        outFence->reset();
        return;
    }

    /* A copy, because the allocator takes what it is given: it either hands
     * the same descriptor straight back, or closes it once it has merged the
     * wait into a new one. The layer still owns the original and closes it in
     * its own time. */
    UniqueFd handed(acquireFence >= 0 ? ::dup(acquireFence) : -1);

    int produced = -1;
    const int err = nvgr->decompress(handle, handed.get(), &produced);

    /* Given away either way. On failure the allocator has left the
     * descriptor in a state only it knows, and letting go of one descriptor
     * on a path that should not be taken is better than closing one it is
     * still holding. */
    handed.release();

    if (err) {
        /* The frame goes up as it is. That is what happened before any of
         * this was asked for, so it is a worse picture rather than none. */
        HWC_LOGE("buffer %p could not be decompressed: %d", handle, err);
        outFence->reset();
        return;
    }

    outFence->reset(produced);
}

}  // namespace hwc
}  // namespace android
