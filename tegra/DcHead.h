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

#ifndef TEGRA_DC_HEAD_H
#define TEGRA_DC_HEAD_H

#include <cstdint>
#include <memory>
#include <vector>

#include "utils/UniqueFd.h"

namespace android {
namespace hwc {

/* One display head, /dev/tegra_dc_N.
 *
 * Where DcControl answers questions about the controller, this is what
 * actually shows pixels: it owns display windows and posts flips.
 *
 * The kernel's flip structure is not exposed here. It carries better than
 * thirty fields, of which a composer that does not scale, rotate, decompress
 * or play protected video uses a dozen; translating in one place keeps the
 * kernel ABI inside the implementation, where a change to it is a local
 * repair rather than a change of interface.
 */
class DcHead {
public:
    /* One window's worth of a flip.
     *
     * A window is the display controller's own compositing unit: it reads a
     * region of memory and places it on the panel, and the hardware blends
     * however many are enabled. Showing a single client-composed frame uses
     * exactly one; overlays are the same structure, more of them.
     */
    struct Window {
        /* Which hardware window. Must have been claimed with claimWindow. */
        int32_t index = 0;

        /* dma-buf descriptor for the memory to scan out, borrowed for the
         * duration of the call. Despite its name in the kernel headers the
         * flip takes a descriptor, not an nvmap identifier: the driver
         * resolves it with dma_buf_get, and its SET_NVMAP_FD ioctl is a
         * no-op that returns success without recording anything. Zero
         * disables the window. */
        int bufferFd = 0;

        uint32_t offset = 0;
        uint32_t stride = 0;

        /* One of the controller's own format codes. */
        uint32_t pixelFormat = 0;

        /* Source region, in pixels. Converted to the hardware's fixed point
         * on the way down. */
        float sourceX = 0.f;
        float sourceY = 0.f;
        float sourceWidth = 0.f;
        float sourceHeight = 0.f;

        /* Destination region on the panel, in whole pixels. */
        int32_t outX = 0;
        int32_t outY = 0;
        int32_t outWidth = 0;
        int32_t outHeight = 0;

        /* Bottom-most first. */
        uint32_t z = 0;

        /* Blending and layout, as the controller's own flags. */
        uint32_t blend = 0;
        uint32_t flags = 0;

        /* Waited on by the hardware before this window is read. Borrowed;
         * ownership stays with the caller. -1 for a buffer already ready. */
        int preFence = -1;
    };

    /* Opens head `index`. Returns null and logs on failure. */
    static std::unique_ptr<DcHead> open(int index);

    ~DcHead();

    DcHead(const DcHead &) = delete;
    DcHead &operator=(const DcHead &) = delete;

    /* Takes ownership of a hardware window. The driver refuses a flip that
     * touches a window owned by another client, so this must succeed before
     * that window appears in a flip. Idempotent within one instance. */
    int claimWindow(uint32_t index);

    /* Gives a window back and blanks it. Called for us on destruction. */
    int releaseWindow(uint32_t index);

    /* Posts one frame.
     *
     * Returns 0 and, in `outPostFence`, a fence that fires once the frame is
     * on the panel and the buffers of the frame before it are free. The
     * caller owns that descriptor. The call itself does not block.
     */
    int flip(const std::vector<Window> &windows, UniqueFd *outPostFence);

private:
    DcHead(UniqueFd fd, int index): mFd(std::move(fd)), mIndex(index) {}

    UniqueFd mFd;
    int mIndex;

    /* Claimed windows, so destruction can hand them all back. */
    std::vector<uint32_t> mOwnedWindows;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DC_HEAD_H
