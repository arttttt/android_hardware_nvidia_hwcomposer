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
#include <map>
#include <memory>
#include <vector>

#include "utils/UniqueFd.h"

/* The kernel's colour-pipeline snapshot; kept behind a pointer so the kernel
 * header stays out of everyone who includes this one. */
struct tegra_dc_ext_cmu;

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
        /* Which hardware window; one of those windows() reports. */
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

        /* How far back this window sits, not how high: zero is nearest the
         * viewer and 0xff is the furthest back the controller goes. It is
         * the opposite of the order layers are usually counted in, and the
         * driver's own choice for the single window it raises by itself is
         * 0xff -- the hardware's word for the bottom. */
        uint32_t z = 0;

        /* Blending and layout, as the controller's own flags. */
        uint32_t blend = 0;
        uint32_t flags = 0;

        /* How tall a block is, where the flags say the memory is arranged in
         * blocks rather than rows. Ignored otherwise. */
        uint8_t blockHeightLog2 = 0;

        /* Waited on by the hardware before this window is read. Borrowed;
         * ownership stays with the caller. -1 for a buffer already ready. */
        int preFence = -1;
    };

    /* What one window can be asked to do.
     *
     * The windows of a head are not alike: which formats each reads, how far
     * it will scale, whether it understands memory arranged in blocks --
     * these differ from one to the next, and a composer that assumed them
     * equal would hand the hardware a frame it cannot show and find out only
     * when the flip is refused. So they are asked for.
     */
    struct WindowCapabilities {
        /* Which formats this window reads, as a bit per format code. */
        uint64_t formats = 0;

        uint32_t minWidth = 0;
        uint32_t maxWidth = 0;
        uint32_t minHeight = 0;
        uint32_t maxHeight = 0;

        bool pitchLayout = false;
        bool tiledLayout = false;
        bool blocklinearLayout = false;

        bool invertH = false;
        bool invertV = false;
        bool scanColumn = false;

        bool scaling = false;

        /* How far the window will resize, as the driver's own ratios: a
         * source may be up to maxDown times wider or taller than the window
         * shows it, and shown up to maxUp times wider or taller than it is.
         * All ones where the window cannot resize at all. */
        uint32_t maxUpH = 1;
        uint32_t maxUpV = 1;
        uint32_t maxDownH = 1;
        uint32_t maxDownV = 1;
    };

    /* Opens head `index`. Returns null and logs on failure. */
    static std::unique_ptr<DcHead> open(int index);

    ~DcHead();

    /* Which head this is.
     *
     * Not needed to post a frame -- the descriptor already says which head --
     * but the same number names this display's other two devices, and whoever
     * has the head is who ends up needing them. */
    int index() const { return mIndex; }

    DcHead(const DcHead &) = delete;
    DcHead &operator=(const DcHead &) = delete;

    /* The windows this head has, in ascending order, all owned by us.
     *
     * Which windows a head has is a property of the board rather than of the
     * chip: the controller's windows are split between the heads, and the
     * split is not the same on every design. There is no ioctl that answers
     * it, so they are discovered by being asked for -- the driver hands over
     * the ones this head has and refuses the rest. Ownership is per open
     * file and lasts until this object dies, so the asking happens once.
     *
     * A flip must name a window this head owns, so everything that builds a
     * flip starts here.
     */
    const std::vector<uint32_t> &windows();

    /* What window `index` can do, or null if the controller would not say.
     *
     * Asked once and remembered: this describes the silicon, which does not
     * change while the composer runs.
     */
    const WindowCapabilities *capabilities(uint32_t index);

    /* Asks the controller to report vertical blanks, or to stop.
     *
     * Subscribing to the event stream on the control device is only half of
     * receiving blanks and is the half that changes nothing in the hardware:
     * it says which events this reader wants to be handed, out of those the
     * driver produces. Producing them at all is this call. Until it is made
     * the controller's vertical blank interrupt stays masked, the driver
     * never reaches the code that queues the event, and a reader subscribed
     * to a stream nobody writes to waits for ever.
     *
     * The request is not counted and not remembered across the display being
     * turned off: the driver drops it when the head is disabled, without
     * telling anyone, so whoever wants blanks has to be prepared to ask
     * again. Asking twice is free -- the driver answers from a flag when the
     * request already holds.
     *
     * Returns 0 if the request was taken. A positive result is the driver
     * refusing it because the display is off, which is an answer rather than
     * a fault; negative is errno.
     */
    int setVBlankReporting(bool enabled);

    /* The head's own descriptor, for the one client of the extension that
     * is not a window: the cursor unit is claimed and driven through the
     * same file the flips go through, and the kernel grants it per
     * descriptor. Borrowed; the head outlives every borrower here. */
    int fd() const {
        return mFd.get();
    }

    /* Posts one frame.
     *
     * Returns 0 and, in `outPostFence`, a fence that fires once the frame is
     * on the panel and the buffers of the frame before it are free. The
     * caller owns that descriptor. The call itself does not block.
     */
    int flip(const std::vector<Window> &windows, UniqueFd *outPostFence);

    /* Would this frame go up?
     *
     * Asks the controller to weigh the same set of windows it would be given
     * to show, and to say whether it can feed them all at once. That is the
     * one limit no amount of reading a window's capabilities will reveal:
     * each window on its own may be within what it can do, while together
     * they ask more of memory than there is to give.
     *
     * Nothing is posted and nothing changes. Returns 0 if the frame would be
     * accepted.
     */
    int test(const std::vector<Window> &windows);

    /* Sets the head's colour matrix.
     *
     * The head ends in a colour pipeline the whole output passes through --
     * every window and the cursor, after blending: a degamma table, a 3x3
     * matrix, a regamma table. It boots as a net no-op, and the tables stay
     * at that boot state here: the matrix multiplies linear light between
     * them, which keeps a third more distinguishable shadow levels than
     * collapsing the tables would, so a caller with gamma-domain semantics
     * translates its coefficients rather than its domain. The uniform
     * `offset` -- an addition the matrix stage does not have -- is folded
     * into the regamma table as a shift of its output values.
     *
     * `matrix` is row-major, out = M * in, as fractions of unity. A
     * coefficient lives in ten bits of two's complement Q1.8 -- [-2, 2)
     * with unity at 256 -- and is clamped to that range here because the
     * driver writes the register unmasked: a value past either end would
     * come out the other side.
     *
     * The write lands at the next frame boundary, never mid-scan, and does
     * not block: the driver keeps a shadow copy and folds the difference in
     * during the following vertical blank. Reset puts the boot pipeline
     * back. All three return false only when the kernel refused.
     */
    bool setColorMatrix(const float matrix[9], float offset);
    bool resetColorMatrix();

    /* Writes the boot pipeline whether or not this instance ever changed
     * it: the kernel keeps a dead predecessor's transform standing, and a
     * home that is computed rather than read cannot notice it. Called once
     * when colour service starts; harmless when nothing was stale. */
    bool writeBootState();

    /* Shows every colour as its tinted distance from white: the per-channel
     * flip, exact in this hardware where the framework's own cross-channel
     * inversion cannot run at all -- the matrix's sums reach the regamma as
     * an unsigned index, so mixed-sign arithmetic folds to zero. The flip
     * lives in the degamma and `tint` scales each flipped channel in the
     * matrix stage -- night mode over an inverted screen, the tint
     * outermost as the framework composes it. Non-negative by contract,
     * all ones for the bare inversion. */
    bool setInversion(const float tint[3]);

private:
    /* Out of line: a member unique_ptr of the forward-declared snapshot
     * makes every path that could destroy a half-built DcHead -- this
     * constructor included -- need the complete type. */
    DcHead(UniqueFd fd, int index);

    /* Takes ownership of one window. A refusal is an answer, not a failure --
     * it is how the head says the window is not its -- so it is not logged. */
    bool tryClaimWindow(uint32_t index);

    /* Gives a window back and blanks it. Done for us on destruction. */
    void releaseWindow(uint32_t index);

    UniqueFd mFd;
    int mIndex;

    /* Reads the live colour pipeline and keeps it as the state to restore.
     * Done once, before the first write ever changes it. */
    bool rememberBootCmu();

    /* Makes the snapshot's tables the identity: degamma spreading each
     * 8-bit level over the 12-bit scale, regamma answering every slot with
     * its own level back. */
    static void fillIdentityTables(tegra_dc_ext_cmu *cmu);

    /* Reads the controller's whole feature table and keeps what it says about
     * each window. One call answers for every window, so it is done once. */
    bool readCapabilities();

    /* Owned windows, ascending. Empty until the first call to windows(). */
    std::vector<uint32_t> mOwnedWindows;
    bool mWindowsDiscovered = false;

    /* Indexed by window. Empty until the controller has been asked. */
    std::map<uint32_t, WindowCapabilities> mCapabilities;
    bool mCapabilitiesRead = false;

    /* The pipeline as it booted: the calibration to come home to. Null until
     * the first colour write; never touched after it is filled. */
    std::unique_ptr<tegra_dc_ext_cmu> mBootCmu;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DC_HEAD_H
