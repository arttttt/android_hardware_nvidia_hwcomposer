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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

/* From the kernel tree; see Android.mk for why the include path stops at
 * include/video rather than include/. */
#include <tegra_dc_ext.h>

#include "tegra/GetFeaturesSource.h"
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

}  // namespace

DcHead::DcHead(UniqueFd fd, int index)
    : mFd(std::move(fd)),
      mIndex(index),
      mFeatures(std::make_unique<GetFeaturesSource>(mFd.get())) {}

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

bool DcHead::settlePorch(int index) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/tegra_dc_%d", index);

    UniqueFd fd(::open(path, O_RDWR | O_CLOEXEC));
    if (!fd)
        return false;

    uint32_t native = 0;
    if (ioctl(fd.get(), TEGRA_DC_EXT_SET_ACT_VFP, &native) != 0) {
        HWC_LOGI("head %d: no porch stretch in this kernel (%s); one rate",
                 index, strerror(errno));
        return false;
    }
    return true;
}

int DcHead::setActiveVfp(uint32_t vfp) {
    if (ioctl(mFd.get(), TEGRA_DC_EXT_SET_ACT_VFP, &vfp) != 0) {
        int err = -errno;
        HWC_LOGE("head %d: porch %u: %s", mIndex, vfp, strerror(-err));
        return err;
    }
    return 0;
}

DcHead::~DcHead() {
    /* The kernel keeps the last matrix written for as long as the head is
     * up, composer or no composer -- a tint left behind here would outlive
     * us and be captured by our successor as its idea of the boot state. */
    resetColorMatrix();

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

    /* Asked now rather than when a plan first needs it, so that what the
     * hardware says about itself is in the log from the start -- next to the
     * list of windows it belongs to, and before anything has depended on it. */
    capabilities(mOwnedWindows.front());

    return mOwnedWindows;
}

bool DcHead::tryClaimWindow(uint32_t index) {
    return ioctl(mFd.get(), TEGRA_DC_EXT_GET_WINDOW, index) == 0;
}

const DcHead::WindowCapabilities *DcHead::capabilities(uint32_t index) {
    /* Remembered only once it has been answered. What the windows can do is a
     * fact about the silicon and does not change -- but the controller will
     * only recite it while it is running, so a refusal describes the moment,
     * not the hardware. Latching one would leave a head that happened to be
     * asked a fraction too early unable to show anything for as long as the
     * composer lives. */
    if (!mCapabilitiesRead) {
        if (readCapabilities())
            mCapabilitiesRead = true;
        else
            HWC_LOGE("head %d would not say what its windows can do", mIndex);
    }

    auto it = mCapabilities.find(index);
    return it == mCapabilities.end() ? nullptr : &it->second;
}

bool DcHead::readCapabilities() {
    std::vector<FeatureEntry> table;
    int err = mFeatures->read(table);
    if (err < 0) {
        HWC_LOGE("head %d: feature table: %s", mIndex, strerror(-err));
        return false;
    }

    if (table.empty()) {
        HWC_LOGE("head %d described nothing, which is what it does while it "
                 "is off", mIndex);
        return false;
    }

    describeWindows(table, mCapabilities);

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

int DcHead::setVBlankReporting(bool enabled) {
    struct tegra_dc_ext_set_vblank request;
    memset(&request, 0, sizeof(request));
    request.enable = enabled ? 1 : 0;

    /* Checked against zero rather than against a negative, and that is not
     * pedantry: this call answers its own refusal in the return value. The
     * driver hands back 1 when it will not take the request because the head
     * is off, and 1 is a success as far as the C library is concerned -- a
     * caller looking for -1 would read a refusal as a request granted, and
     * then wait for events that were never turned on. */
    const int ret = ioctl(mFd.get(), TEGRA_DC_EXT_SET_VBLANK, &request);
    if (ret < 0) {
        const int err = -errno;
        HWC_LOGE("head %d: SET_VBLANK(%d): %s", mIndex, request.enable,
                 strerror(-err));
        return err;
    }

    return ret;
}

/* Turns windows into what the controller's calls expect.
 *
 * Shared between asking whether a frame would go up and putting it up,
 * because those must weigh exactly the same thing: a test of anything other
 * than what is posted answers a question nobody asked.
 *
 * A free function rather than a method, so that the kernel's structure stays
 * out of the header -- see the note there on why this class does not show it.
 */
static std::vector<struct tegra_dc_ext_flip_windowattr> describe(
    const std::vector<DcHead::Window> &windows) {
    std::vector<struct tegra_dc_ext_flip_windowattr> attrs(windows.size());

    for (size_t i = 0; i < windows.size(); ++i) {
        const DcHead::Window &src = windows[i];
        struct tegra_dc_ext_flip_windowattr &dst = attrs[i];

        memset(&dst, 0, sizeof(dst));

        dst.index = src.index;
        dst.buff_id = static_cast<__u32>(src.bufferFd);
        dst.offset = src.offset;
        dst.stride = src.stride;
        /* Chroma from the same descriptor: the two descriptor fields for
         * it stay zero, which the driver reads as "take it from buff_id",
         * and the offset is added to that base. The second chroma plane's
         * fields stay zero too -- for the semi-planar formats sent here
         * the driver never writes the register they would go to. */
        dst.offset_u = src.offsetU;
        dst.stride_uv = src.strideUV;
        dst.pixformat = src.pixelFormat;
        dst.blend = src.blend;
        dst.flags = src.flags;
        if (src.loadCsc) {
            dst.flags |= TEGRA_DC_EXT_FLIP_FLAG_UPDATE_CSC;
            dst.csc.yof = src.csc.yof;
            dst.csc.kyrgb = src.csc.kyrgb;
            dst.csc.kur = src.csc.kur;
            dst.csc.kvr = src.csc.kvr;
            dst.csc.kug = src.csc.kug;
            dst.csc.kvg = src.csc.kvg;
            dst.csc.kub = src.csc.kub;
            dst.csc.kvb = src.csc.kvb;
        }
        /* Read only while the flags carry GLOBAL_ALPHA; the driver writes
         * back 255 the moment they do not. */
        dst.global_alpha = src.globalAlpha;
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

    return attrs;
}

/* The request both the proposal and the flip are made of: the fourth revision
 * of the structure, the one both the 3.10 kernel and the 4.9 kernels of the
 * L4T r32 line serve, and the only flip the latter still know. Its head is
 * the third revision byte for byte; the tail that would point at per-flip
 * user data stays empty. The only user data the 3.10 kernel understands is
 * HDR metadata, which on a DSI panel changes nothing except arming the
 * frame-end interrupt whenever the metadata is sent, and on r32 the fence fd
 * in the head remains the default way to be handed the post fence. */
static struct tegra_dc_ext_flip_4 enclose(
        const std::vector<struct tegra_dc_ext_flip_windowattr> &attrs) {
    struct tegra_dc_ext_flip_4 request;
    memset(&request, 0, sizeof(request));

    request.win = reinterpret_cast<__u64>(attrs.data());
    request.win_num = static_cast<__u8>(attrs.size());
    request.post_syncpt_fd = -1;

    /* No head flags. The ones that exist select YUV bypass, variable refresh
     * and, on the 3.10 kernel, a second, wider window structure; sending that
     * last flag with the structure this code fills would have the driver
     * parse every field at the wrong offset. */
    request.flags = 0;

    return request;
}

int DcHead::test(const std::vector<Window> &windows) {
    if (windows.empty())
        return -EINVAL;

    std::vector<struct tegra_dc_ext_flip_windowattr> attrs = describe(windows);
    struct tegra_dc_ext_flip_4 proposal = enclose(attrs);

    /* The controller weighs the whole set against the memory bandwidth it can
     * command and answers without touching anything. This is the one refusal
     * that cannot be predicted from the windows alone: each may be within
     * what it can do while together they ask for more than there is.
     *
     * How much of the request the driver reads is settled by the header this
     * is built against: the 3.10 kernel declares the proposal over the third
     * revision and copies its 24 bytes, the r32 line declares it over the
     * fourth and copies all 40. The head is the same bytes either way. */
    if (ioctl(mFd.get(), TEGRA_DC_EXT_SET_PROPOSED_BW_3, &proposal) < 0) {
        int err = -errno;
        HWC_LOGD("head %d: %zu window(s) will not fit: %s", mIndex,
                 windows.size(), strerror(-err));
        return err;
    }

    return 0;
}

int DcHead::flip(const std::vector<Window> &windows, UniqueFd *outPostFence) {
    if (windows.empty())
        return -EINVAL;

    std::vector<struct tegra_dc_ext_flip_windowattr> attrs = describe(windows);
    struct tegra_dc_ext_flip_4 flip = enclose(attrs);

    for (const Window &window : windows) {
        HWC_LOGD("head %d: win %d buf=%d off=%u stride=%u offU=%u strideUV=%u "
                 "fmt=0x%x flags=0x%x blockh=%u src=%.1fx%.1f+%.1f+%.1f "
                 "dst=%dx%d+%d+%d z=%u pre=%d",
                 mIndex, window.index, window.bufferFd, window.offset,
                 window.stride, window.offsetU, window.strideUV,
                 window.pixelFormat, window.flags,
                 window.blockHeightLog2, window.sourceWidth,
                 window.sourceHeight, window.sourceX, window.sourceY,
                 window.outWidth, window.outHeight, window.outX, window.outY,
                 window.z, window.preFence);
    }

    if (ioctl(mFd.get(), TEGRA_DC_EXT_FLIP4, &flip) < 0) {
        int err = -errno;
        HWC_LOGE("head %d: FLIP4 with %zu window(s): %s", mIndex,
                 windows.size(), strerror(-err));
        return err;
    }

    HWC_LOGD("head %d: flipped %zu window(s), post fence %d", mIndex,
             windows.size(), flip.post_syncpt_fd);

    outPostFence->reset(flip.post_syncpt_fd);
    return 0;
}

namespace {

float srgbEncode(float linear) {
    return linear <= 0.0031308F ? linear * 12.92F
                                : 1.055F * powf(linear, 1.F / 2.4F) - 0.055F;
}

float srgbDecode(float encoded) {
    return encoded <= 0.04045F ? encoded / 12.92F
                               : powf((encoded + 0.055F) / 1.055F, 2.4F);
}

/* The hardware maps a 12-bit value to a regamma slot directly over the low
 * eighth of the range and in steps of eight over the rest -- derived from
 * the vendor table and confirmed against all 960 entries. This walks slots
 * and asks which value each covers: the same map inverted. */
uint32_t lut2Covers(uint32_t slot) {
    return slot < 512 ? slot : 512 + (slot - 512) * 8;
}

}  // namespace

bool DcHead::rememberBootCmu() {
    if (mBootCmu)
        return true;

    /* Home is computed, not read. The pipeline boots as the sRGB pair --
     * degamma to linear light, regamma back -- and these formulas reproduce
     * the kernel's tables to the byte, checked entry for entry. Reading the
     * live state instead would remember whatever a predecessor died holding:
     * the kernel keeps the last write for as long as the head is up, so a
     * composer that crashed mid-transform leaves its tables standing, and a
     * home read from them would make that state permanent. A computed home
     * has no memory to poison. The day the board declares a calibrated
     * pipeline of its own, this is the place to revisit. */
    auto cmu = std::make_unique<tegra_dc_ext_cmu>();
    memset(cmu.get(), 0, sizeof(*cmu));
    cmu->cmu_enable = 1;

    static const __u16 kIdentity[9] = {256, 0, 0, 0, 256, 0, 0, 0, 256};
    memcpy(cmu->csc, kIdentity, sizeof(kIdentity));

    for (uint32_t i = 0; i < 256; ++i)
        cmu->lut1[i] = static_cast<__u16>(
            lroundf(4095.F * srgbDecode(static_cast<float>(i) / 255.F)));
    for (uint32_t i = 0; i < 960; ++i)
        cmu->lut2[i] = static_cast<__u16>(lroundf(
            255.F * srgbEncode(static_cast<float>(lut2Covers(i)) / 4095.F)));

    mBootCmu = std::move(cmu);
    return true;
}

void DcHead::fillIdentityTables(tegra_dc_ext_cmu *cmu) {
    /* A degamma entry per 8-bit level, spread over the 12-bit output. */
    for (uint32_t i = 0; i < 256; ++i)
        cmu->lut1[i] = static_cast<__u16>(i << 4);

    /* Every slot answers with the 8-bit level whose 12-bit neighbourhood it
     * covers, so LUT1 above lands exactly on itself: net passthrough. */
    for (uint32_t i = 0; i < 960; ++i) {
        const uint32_t v8 = (lut2Covers(i) + 8) >> 4;
        cmu->lut2[i] = static_cast<__u16>(v8 > 255 ? 255 : v8);
    }
}

bool DcHead::setColorMatrix(const float matrix[9], float offset) {
    if (!rememberBootCmu())
        return false;

    /* The boot tables stay: the matrix acts between the degamma and the
     * regamma, multiplying linear light -- chosen over an identity-table
     * gamma-domain pipeline on numbers, not taste: the 12-bit linear
     * middle keeps a third more distinguishable shadow levels under a
     * strong tint (33 against 25 of the darkest 49 at half strength), and
     * colourimetry agrees. The caller owns translating gamma-domain
     * semantics onto this (the diagonal bridge); the uniform offset, an
     * addition the matrix stage does not have, is folded into the regamma
     * as a shift of its output values -- past the point where a negative
     * sum could fold the range over. */
    tegra_dc_ext_cmu cmu = *mBootCmu;

    for (int i = 0; i < 9; ++i) {
        /* Ten bits of two's complement, Q1.8. Clamped to the field: a
         * value written past either end would come out the other side --
         * 2.5 read back as roughly -1.5. */
        long v = lroundf(matrix[i] * 256.F);
        if (v > 0x1ff)
            v = 0x1ff;
        if (v < -0x200)
            v = -0x200;
        cmu.csc[i] = static_cast<__u16>(v & 0x3ff);
    }

    if (offset != 0.F) {
        const long shift = lroundf(offset * 255.F);
        for (uint32_t i = 0; i < 960; ++i) {
            long v = static_cast<long>(cmu.lut2[i]) + shift;
            cmu.lut2[i] = static_cast<__u16>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }

    if (ioctl(mFd.get(), TEGRA_DC_EXT_SET_CMU_ALIGNED, &cmu) < 0) {
        HWC_LOGE("head %d: SET_CMU_ALIGNED: %s", mIndex, strerror(errno));
        return false;
    }
    return true;
}

bool DcHead::setInversion(const float tint[3]) {
    if (!rememberBootCmu())
        return false;

    /* The framework's inversion is a cross-channel luminance flip with
     * mixed-sign coefficients, and this pipeline cannot run those: the
     * matrix's sums pass to the regamma as an unsigned index, so every
     * negative result folds to zero. What it can run exactly is the
     * per-channel flip with a tint over it -- the tint is outermost in the
     * framework's own composition, so the flip goes in the DEGAMMA, where
     * every input becomes its distance from white, the tint multiplies the
     * flipped value in the matrix stage, and the regamma is the identity:
     * out = tint * (white - in), per channel, all of it non-negative. Same
     * purpose as the true inversion, light and dark exchanged; hues map
     * differently than on hardware with a signed path. The same trade
     * shipped for years on this block's descendant in another product.
     *
     * Gamma-domain throughout -- the framework's own domain for these
     * matrices, so the tint's diagonal needs no bridge. Positive by the
     * caller's contract: a negative would cross the unsigned index this
     * mode exists to avoid. */
    tegra_dc_ext_cmu cmu = *mBootCmu;
    fillIdentityTables(&cmu);

    for (uint32_t i = 0; i < 256; ++i)
        cmu.lut1[i] = static_cast<__u16>((255 - i) << 4);

    for (int c = 0; c < 3; ++c) {
        long v = lroundf(tint[c] * 256.F);
        if (v < 0)
            v = 0;
        if (v > 0x1ff)
            v = 0x1ff;
        cmu.csc[c * 3 + c] = static_cast<__u16>(v);
    }

    /* With the tint at most unity -- there is no whiter than white in this
     * family -- the matrix output tops out at 4080 of the 4095 the stage
     * can carry, so the hardware's clamp is never reached. A tint above
     * unity, should one ever exist, would clip lightly against it. */

    if (ioctl(mFd.get(), TEGRA_DC_EXT_SET_CMU_ALIGNED, &cmu) < 0) {
        HWC_LOGE("head %d: SET_CMU_ALIGNED (inversion): %s", mIndex,
                 strerror(errno));
        return false;
    }
    return true;
}

bool DcHead::resetColorMatrix() {
    /* Nothing was ever written, so there is nothing to put back. */
    if (!mBootCmu)
        return true;

    return writeBootState();
}

bool DcHead::writeBootState() {
    if (!rememberBootCmu())
        return false;

    /* Unconditional where reset is guarded: the kernel keeps whatever the
     * last composer wrote for as long as the head is up, so a predecessor
     * that died mid-transform leaves the panel wearing it, and nothing else
     * ever puts it back -- home is computed here, not read, so it cannot
     * even be seen. Written once at the start of service, this heals any
     * such leftover; written when nothing is stale, it diffs to nothing. */
    if (ioctl(mFd.get(), TEGRA_DC_EXT_SET_CMU_ALIGNED, mBootCmu.get()) < 0) {
        HWC_LOGE("head %d: SET_CMU_ALIGNED (boot state): %s", mIndex,
                 strerror(errno));
        return false;
    }
    return true;
}

}  // namespace hwc
}  // namespace android
