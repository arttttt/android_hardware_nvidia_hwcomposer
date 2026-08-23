/*
 * Copyright (C) 2022 The Android Open Source Project
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

#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/FrameTimeHistory.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

class FbIdHandle;

using ILayerId = int64_t;

enum class CompositionType {
  kInvalid,
  kClient,
  kDevice,
  kSolidColor,
  kCursor,
  kDeviceOccluded
};

enum class TransferFunction : int32_t {
  kUnknown,
  kSmpte170M,
  kSrgb,
  kPq,
  kHlg,
};

/* Rotation is defined in the clockwise direction */
/* The flip is done before rotation */
struct LayerTransform {
  bool hflip;
  bool vflip;
  bool rotate90;
};

template <typename T>
struct Rect {
  T left;
  T top;
  T right;
  T bottom;

  T Width() const {
    return right - left;
  }

  T Height() const {
    return bottom - top;
  }

  bool operator==(const Rect<T>& rhs) const {
    return left == rhs.left && top == rhs.top && right == rhs.right &&
           bottom == rhs.bottom;
  }
  bool operator!=(const Rect<T>& rhs) const {
    return !(*this == rhs);
  }
};

using IRect = Rect<int32_t>;
using FRect = Rect<float>;

struct SrcRectInfo {
  /* nullopt means the whole buffer */
  std::optional<FRect> f_rect;

  bool operator==(const SrcRectInfo& rhs) const {
    return f_rect == rhs.f_rect;
  }
  bool operator!=(const SrcRectInfo& rhs) const {
    return !(*this == rhs);
  }
};

struct DstRectInfo {
  /* nullopt means the whole display */
  std::optional<IRect> i_rect;

  bool operator==(const DstRectInfo& rhs) const {
    return i_rect == rhs.i_rect;
  }
  bool operator!=(const DstRectInfo& rhs) const {
    return !(*this == rhs);
  }
};

struct DamageInfo {
  /* Empty vector means the whole source buffer may have been modified. */
  std::vector<IRect> dmg_rects;
};

constexpr float kAlphaOpaque = 1.0F;

struct PresentInfo {
  LayerTransform transform{};
  float alpha = kAlphaOpaque;
  SrcRectInfo source_crop{};
  DstRectInfo display_frame{};
  DamageInfo damage{};

  struct MatchedExtents {
    float src_w;
    float src_h;
    float dst_w;
    float dst_h;
  };

  /* Source and destination extents in the same axes, so a resize is a
   * disagreement of like with like.
   *
   * The display frame arrives already turned: under a quarter turn the
   * source's width sits against the screen's height and height against
   * width. Crossing lives here and only here, because four independent
   * copies of the question had already drifted -- two crossed, two did
   * not -- and a turned layer that did not resize was read as one that
   * did. Mirrors swap no axis.
   *
   * A missing rectangle means the whole buffer, and that is the caller's
   * rule: PresentInfo does not know the buffer, so its size arrives as
   * an argument. */
  MatchedExtents ExtentsInDestAxes(float whole_w = 0,
                                   float whole_h = 0) const {
    float src_w = source_crop.f_rect ? source_crop.f_rect->Width() : whole_w;
    float src_h = source_crop.f_rect ? source_crop.f_rect->Height() : whole_h;
    if (transform.rotate90) {
      const float t = src_w;
      src_w = src_h;
      src_h = t;
    }
    const float dst_w = display_frame.i_rect
                            ? static_cast<float>(display_frame.i_rect->Width())
                            : whole_w;
    const float dst_h = display_frame.i_rect
                            ? static_cast<float>(display_frame.i_rect->Height())
                            : whole_h;
    return {src_w, src_h, dst_w, dst_h};
  }

  bool Resizes() const {
    if (!source_crop.f_rect || !display_frame.i_rect)
      return false;
    const auto e = ExtentsInDestAxes();
    return e.src_w != e.dst_w || e.src_h != e.dst_h;
  }

  bool RequireScalingOrPhasing() const {
    if (!source_crop.f_rect || !display_frame.i_rect) {
      return false;
    }

    const auto &src = *source_crop.f_rect;
    const auto phasing = (src.left - std::floor(src.left) != 0) ||
                         (src.top - std::floor(src.top) != 0);
    return Resizes() || phasing;
  }
};

/* A rectangle mirrored within the extents it is placed in.
 *
 * The one home for the question "where does this rectangle land when its
 * placement is mirrored". The stock composer kept helpers of this kind and
 * pushed every rectangle through them before anything below saw it; three
 * of our own defects grew from answering the question inline -- the kernel
 * scale check, the merge clip, the window clip -- each crossing axes by
 * hand and each crossing them differently.
 *
 * Mirrors only, deliberately. The engine's transpose was measured to leave
 * placement alone, and a transposed mapping written here untested would be
 * a guessed convention -- the class of error this helper exists to end. */
struct MirroredRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
};

constexpr MirroredRect MirrorRectWithin(int32_t left, int32_t top,
                                        int32_t right, int32_t bottom,
                                        bool mirror_x, bool mirror_y,
                                        int32_t width, int32_t height) {
  MirroredRect out{left, top, right, bottom};
  if (mirror_x) {
    out.left = width - right;
    out.right = width - left;
  }
  if (mirror_y) {
    out.top = height - bottom;
    out.bottom = height - top;
  }
  return out;
}

/* The properties, checked on every build: a mirror is an involution about
 * the middle of the extents, sizes survive it, bands trade places, the
 * untouched axis stays put. Stated over neutral numbers on purpose -- the
 * panel cases that bred this helper are measurements and live in the
 * hunt's records; a panel size hard-coded here would tie a general helper
 * to one device. */
static_assert(MirrorRectWithin(7, 9, 30, 40, false, false, 100, 100).left == 7
                  && MirrorRectWithin(7, 9, 30, 40, false, false, 100, 100)
                             .top == 9,
              "no mirror, no movement");
static_assert(MirrorRectWithin(0, 10, 50, 30, false, true, 100, 100).top == 70
                  && MirrorRectWithin(0, 10, 50, 30, false, true, 100, 100)
                             .bottom == 90,
              "a band lands mirrored about the middle, height kept");
static_assert(MirrorRectWithin(0, 70, 50, 90, false, true, 100, 100).top == 10,
              "mirroring twice comes home");
static_assert(MirrorRectWithin(0, 0, 50, 40, false, true, 100, 100).top == 60,
              "the first band lands where the second one was");
static_assert(MirrorRectWithin(0, 10, 50, 30, false, true, 100, 100).left == 0,
              "the axis not mirrored stays put");
static_assert(MirrorRectWithin(80, 0, 100, 5, true, false, 100, 100).left == 0
                  && MirrorRectWithin(80, 0, 100, 5, true, false, 100, 100)
                             .right == 20,
              "an edge strip crosses to the other edge");

struct LayerData {
  std::optional<BufferInfo> bi;
  std::shared_ptr<FbIdHandle> fb;
  PresentInfo pi;
  SharedFd acquire_fence;

  /* Given values of their own, and it matters.
   *
   * Every other member here builds itself. These two are plain enumerations,
   * so a LayerData that is default-initialised -- which is what a layer's
   * copy is, being a member of HwcLayer with no initialiser -- leaves them
   * holding whatever the memory held before.
   *
   * Nothing sets the colourspace on this platform: the entry point never
   * fills it in, so the layer keeps whatever it was born with, and the
   * planner asks on every frame whether the layers agree on one. Compared
   * across layers, unset memory disagrees. The planner concluded that the
   * display was being asked to show layers in different colourspaces with no
   * colour pipeline to reconcile them, and sent every frame of every scene to
   * the GPU -- so no frame ever reached a window of the controller, and the
   * whole point of hardware composition was lost to a comparison of nothing
   * against nothing.
   *
   * Upstream has the same declaration; there the field is set, so it does not
   * show. Worth sending back.
   */
  HwcColorspace colorspace{};
  TransferFunction transfer_func{};
  FrameTimeHistory frame_time_history;
  std::optional<float> brightness;

  /* Whether this layer was drawing recently, judged by the HwcLayer it
   * was copied from. The joining plan reads it to keep drawing layers
   * out of the merge; nothing below the planner looks at it. */
  bool live = false;
};

}  // namespace android::drm_hwcomposer
