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

#include "tegra/TegraAtomicStateManager.h"

#include "tegra/TegraPlane.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <ndk/sync.h>
#include <string.h>
#include <sync/sync.h>
#include <time.h>

#include <optional>
#include <sstream>
#include <string>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <tegra_dc_ext.h>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/GrallocBufferHandle.h"
#include "bufferinfo/NvGralloc.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "display/FbIdHandle.h"
#include "display/Plane.h"
#include "display/PipelineBinding.h"
#include "tegra/FbDevice.h"
#include "tegra/TegraFormat.h"
#include "utils/Logging.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {

/* Whether to undo the compression at all.
 *
 * Off, the display reads the compressed arrangement as though it were pixels
 * and shows a regular grid over the picture -- so this is not a way to run,
 * it is a way to measure. What flattening costs cannot be told from a build
 * that always does it, and the question is worth answering directly rather
 * than by inference: everything else about a frame stays exactly the same
 * with this off, so whatever the numbers move by is what it costs.
 *
 * Read once. Answering it per frame would let it be changed without
 * restarting anything, which is worth very little against asking the property
 * store the same question sixty times a second for the life of the device --
 * and a switch that exists to measure with is one that can afford a restart.
 */
bool FlatteningWanted() {
  static const bool wanted = property_get_bool("vendor.hwc.tegra.flatten",
                                               1) != 0;
  return wanted;
}

int64_t NowNs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (static_cast<int64_t>(ts.tv_sec) * 1000000000) + ts.tv_nsec;
}

/* When a fence came due, or nothing if it has not yet.
 *
 * Read rather than waited on: the question is whether it was due by a given
 * moment, and waiting to find out would change the answer. The time is the
 * fence's own, so it says when the hardware finished rather than when this
 * thread got round to asking.
 */
/* Has this fence come due yet?
 *
 * Separate from SignalTimeNs, which answers "not yet" and "could not ask" with
 * the same empty result. For a warning that is fine -- neither is worth
 * saying. For counting it is not: a column of frames that could not be asked
 * about, read as a column of frames that waited, would answer the question
 * this counting exists to settle, and answer it wrongly.
 */
enum class Due { kYes, kNotYet, kCouldNotAsk };

Due FenceDue(const SharedFd &fence) {
  if (!fence)
    return Due::kCouldNotAsk;

  struct sync_file_info *info = sync_file_info(*fence);
  if (info == nullptr)
    return Due::kCouldNotAsk;

  const Due answer = info->status == 1 ? Due::kYes : Due::kNotYet;
  sync_file_info_free(info);
  return answer;
}

std::optional<int64_t> SignalTimeNs(const SharedFd &fence) {
  if (!fence)
    return std::nullopt;

  struct sync_file_info *info = sync_file_info(*fence);
  if (info == nullptr)
    return std::nullopt;

  if (info->status != 1) {
    sync_file_info_free(info);
    return std::nullopt;
  }

  int64_t latest = 0;
  struct sync_fence_info *each = sync_get_fence_info(info);
  for (size_t i = 0; i < info->num_fences; i++) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    latest = std::max(latest, static_cast<int64_t>(each[i].timestamp_ns));
  }

  sync_file_info_free(info);
  return latest;
}

uint32_t BlendFor(BufferBlendMode mode) {
  switch (mode) {
    case BufferBlendMode::kPreMult:
      return TEGRA_DC_EXT_BLEND_PREMULT;
    case BufferBlendMode::kCoverage:
      return TEGRA_DC_EXT_BLEND_COVERAGE;
    case BufferBlendMode::kNone:
    case BufferBlendMode::kUndefined:
    default:
      return TEGRA_DC_EXT_BLEND_NONE;
  }
}

/* The buffer as the allocator knows it, if whoever read this one left it
 * behind, and null otherwise.
 *
 * The cast is not a guess. That field exists to carry exactly this -- the
 * handle a description was read from, kept alive as long as the description
 * is -- and GrallocBufferHandle is the only thing in this tree that can be
 * put in it. It is a static cast because this platform's builds turn run-time
 * type information off, so the checked one would not link.
 */
buffer_handle_t NativeHandleOf(const BufferInfo &bi) {
  if (!bi.fds_shared)
    return nullptr;

  return std::static_pointer_cast<GrallocBufferHandle>(bi.fds_shared)
      ->GetHandle();
}

/* Where a plan puts a layer, said the way this controller reads it.
 *
 * The two count in opposite directions. A plan says how high a layer sits,
 * from the bottom up, which is the order the framework hands layers over and
 * the order everything above this is written against. The controller takes a
 * depth: the smallest number is the window nearest the viewer, and 0xff is as
 * far back as it goes -- which is what the driver gives the single window it
 * puts up on its own, and so is the hardware's own word for the bottom.
 *
 * Handing a plan's height over unchanged puts the bottom layer in front of
 * everything, which on a display showing a wallpaper is a wallpaper over the
 * whole screen and the entire user interface behind it. It stayed hidden for
 * as long as only one window was ever used, because one window has no order
 * to get wrong.
 *
 * Turned round here, at the single point where the plan's vocabulary becomes
 * the controller's, rather than by teaching the planner this hardware's
 * dialect.
 */
uint32_t DepthForZPos(int z_pos) {
  constexpr uint32_t kFurthestBack = 0xff;

  const auto height = static_cast<uint32_t>(std::max(z_pos, 0));
  return height >= kFurthestBack ? 0 : kFurthestBack - height;
}

/* Fills one window from one layer of a plan. False if the layer cannot be
 * described to this controller at all, which the planner should already have
 * ruled out by asking the plane -- so it is a fault worth logging rather than
 * an ordinary refusal. */
bool DescribeWindow(const LayerData &layer, uint32_t plane_id, uint32_t depth,
                    int32_t panel_w, int32_t panel_h,
                    hwc::DcHead::Window *out) {
  if (!layer.bi || !layer.fb) {
    ALOGE("layer for plane %u has no buffer", plane_id);
    return false;
  }

  const BufferInfo &bi = *layer.bi;

  const uint32_t format = TegraFormatFromDrm(bi.format);
  uint32_t flags = 0;
  uint8_t block_height_log2 = 0;

  if (format == 0 ||
      !TegraLayoutFromModifier(bi.modifiers[0], &flags, &block_height_log2)) {
    ALOGE("layer for plane %u cannot be shown here", plane_id);
    return false;
  }

  *out = hwc::DcHead::Window{};

  out->index = static_cast<int32_t>(plane_id);
  out->bufferFd = static_cast<int>(layer.fb->GetFbId());
  out->offset = bi.offsets[0];
  out->stride = bi.pitches[0];
  out->pixelFormat = format;
  out->flags = flags;
  out->blockHeightLog2 = block_height_log2;
  out->z = depth;
  out->blend = BlendFor(bi.blend_mode);

  /* A source region left unsaid means the whole buffer, and a destination
   * left unsaid means the whole panel. The controller has no such shorthand,
   * so both are spelled out from what is known. */
  if (layer.pi.source_crop.f_rect) {
    const auto &src = *layer.pi.source_crop.f_rect;
    out->sourceX = src.left;
    out->sourceY = src.top;
    out->sourceWidth = src.Width();
    out->sourceHeight = src.Height();
  } else {
    out->sourceWidth = static_cast<float>(bi.width);
    out->sourceHeight = static_cast<float>(bi.height);
  }

  if (layer.pi.display_frame.i_rect) {
    const auto &dst = *layer.pi.display_frame.i_rect;
    out->outX = dst.left;
    out->outY = dst.top;
    out->outWidth = dst.Width();
    out->outHeight = dst.Height();
  } else {
    out->outWidth = static_cast<int32_t>(bi.width);
    out->outHeight = static_cast<int32_t>(bi.height);
  }

  /* Clipped to the panel, for the same reason the merged group is: the
   * hardware's position fields are unsigned thirteen-bit and the kernel
   * forwards them unchecked, so an off-panel corner wraps into a position
   * that is never scanned and the layer silently vanishes. Ordinary layers
   * slide off edges too -- a parallax wallpaper does it on every swipe.
   * The source is trimmed in the window's own scale, so a resizing window
   * keeps its ratio and the no-resize windows stay exactly one-to-one. */
  if (panel_w > 0 && panel_h > 0 && out->outWidth > 0 && out->outHeight > 0) {
    const int32_t cl = std::max(out->outX, 0);
    const int32_t ct = std::max(out->outY, 0);
    const int32_t cr = std::min(out->outX + out->outWidth, panel_w);
    const int32_t cb = std::min(out->outY + out->outHeight, panel_h);

    if (cr <= cl || cb <= ct) {
      /* Nothing of it lies on the panel. A window showing nothing is a
       * switched-off window, not a failed frame. */
      const auto index = static_cast<int32_t>(plane_id);
      *out = hwc::DcHead::Window{};
      out->index = index;
      return true;
    }

    if (cl != out->outX || ct != out->outY ||
        cr != out->outX + out->outWidth ||
        cb != out->outY + out->outHeight) {
      const float sx = out->sourceWidth / static_cast<float>(out->outWidth);
      const float sy = out->sourceHeight / static_cast<float>(out->outHeight);
      const float src_l =
          out->sourceX + static_cast<float>(cl - out->outX) * sx;
      const float src_t =
          out->sourceY + static_cast<float>(ct - out->outY) * sy;
      const float src_r =
          out->sourceX + out->sourceWidth -
          static_cast<float>(out->outX + out->outWidth - cr) * sx;
      const float src_b =
          out->sourceY + out->sourceHeight -
          static_cast<float>(out->outY + out->outHeight - cb) * sy;

      out->sourceX = src_l;
      out->sourceY = src_t;
      out->sourceWidth = src_r - src_l;
      out->sourceHeight = src_b - src_t;
      out->outX = cl;
      out->outY = ct;
      out->outWidth = cr - cl;
      out->outHeight = cb - ct;
    }
  }

  /* Borrowed: the plan owns the fence and outlives the flip, and the kernel
   * takes its own reference while the call is in progress. */
  out->preFence = layer.acquire_fence ? *layer.acquire_fence : -1;

  return true;
}

/* One layer of a plan, said the way the image compositor takes it.
 *
 * Both rectangles are spelled out for the same reason they are in
 * DescribeWindow: unsaid means the whole buffer and the whole panel, and
 * neither the controller nor the engine has that shorthand.
 *
 * The fence is borrowed. The engine is told to wait on it and does not take
 * it; the plan owns it and outlives the frame.
 */
hwc::VicSession::Layer MergeLayerFrom(const LayerData &layer) {
  hwc::VicSession::Layer out{};

  const BufferInfo &bi = *layer.bi;

  out.handle = bi.handle;

  if (layer.pi.source_crop.f_rect) {
    const auto &src = *layer.pi.source_crop.f_rect;
    out.source_left = src.left;
    out.source_top = src.top;
    out.source_right = src.right;
    out.source_bottom = src.bottom;
  } else {
    out.source_right = static_cast<float>(bi.width);
    out.source_bottom = static_cast<float>(bi.height);
  }

  if (layer.pi.display_frame.i_rect) {
    const auto &dst = *layer.pi.display_frame.i_rect;
    out.display_left = dst.left;
    out.display_top = dst.top;
    out.display_right = dst.right;
    out.display_bottom = dst.bottom;
  } else {
    out.display_right = static_cast<int32_t>(bi.width);
    out.display_bottom = static_cast<int32_t>(bi.height);
  }

  /* Coverage blending multiplies at draw time; premultiplied does not.
   * Anything else means the layer carries no alpha worth honouring, and
   * saying premultiplied of an opaque layer costs nothing. */
  out.premultiplied = bi.blend_mode != BufferBlendMode::kCoverage;
  out.alpha = layer.pi.alpha;
  out.acquire_fence = layer.acquire_fence ? *layer.acquire_fence : -1;

  return out;
}

}  // namespace

std::unique_ptr<AtomicRequest> TegraAtomicStateManager::GetAtomicModeReqForArgs(
    AtomicCommitArgs &args) {
  const std::vector<uint32_t> &available = head_.windows();
  if (available.empty()) {
    ALOGE("no windows to show anything with");
    return nullptr;
  }

  /* A timing is accepted only if it is one the panel runs. On this board
   * that means the one it is already running, so honouring the request is
   * doing nothing -- but a request for a timing this panel does not have must
   * not be answered with silence, or the framework will believe a mode change
   * happened that did not. */
  if (args.display_mode) {
    const drmModeModeInfo &wanted = args.display_mode->GetRawMode();

    const bool known = std::any_of(modes_.begin(), modes_.end(),
                                   [&wanted](const DrmMode &mode) {
                                     return mode == wanted;
                                   });
    if (!known) {
      ALOGE("asked for mode %s, which this panel does not run",
            args.display_mode->GetName().c_str());
      return nullptr;
    }
  }

  /* Every window of the head, whether or not a layer claimed it.
   *
   * A window keeps what it was last given until told otherwise, and a frame
   * only touches the windows it names -- so one left out stays on screen over
   * the top of this frame. The spare ones are therefore sent with no buffer,
   * which is how a window is switched off, and that is what a default-built
   * Window already is.
   */
  std::vector<hwc::DcHead::Window> windows(available.size());
  for (size_t i = 0; i < available.size(); ++i)
    windows[i].index = static_cast<int32_t>(available[i]);

  /* In step with the windows, and null wherever one shows nothing. This is
   * the answer to "which buffers will the display actually read", which is
   * not known any earlier than here and is the whole reason for carrying the
   * handles this far. */
  std::vector<buffer_handle_t> handles(available.size(), nullptr);

  /* Value-initialised on purpose: the geometry fields are only assigned
   * when layers join the group, and every reader is behind a non-empty
   * guard today -- but an invariant that lives in guards alone is one
   * refactor from undefined reads, and zeroes cost nothing here. */
  TegraAtomicRequest::Merge merge{};

  const int32_t panel_w =
      modes_.empty() ? 0
                     : static_cast<int32_t>(modes_.front().GetRawMode()
                                                .hdisplay);
  const int32_t panel_h =
      modes_.empty() ? 0
                     : static_cast<int32_t>(modes_.front().GetRawMode()
                                                .vdisplay);

  if (args.composition) {
    for (const auto &joining : args.composition->plan) {
      if (!joining.plane)
        continue;

      const uint32_t plane_id = joining.plane->Get()->GetId();

      auto it = std::find(available.begin(), available.end(), plane_id);
      if (it == available.end()) {
        ALOGE("plan names plane %u, which this head does not own", plane_id);
        return nullptr;
      }

      const size_t slot = static_cast<size_t>(it - available.begin());

      /* A layer bound for the merging plane is not described to the
       * controller at all, because what that window will show is not this
       * buffer. Set aside instead, to be drawn when the frame is executed --
       * a plan is weighed several times before one is chosen, and drawing for
       * each of those would cost more than the merge saves. */
      const auto *plane = static_cast<const TegraPlane *>(joining.plane->Get());
      if (plane->IsMerging()) {
        /* Several planes can name the same window -- that is how a planner
         * which gives one layer to one plane is told that this one takes
         * more. They arrive in order of height, so the first is the bottom of
         * the group, and its place in the stack is the merged buffer's: what
         * is above it in the group is above it inside the buffer. */
        if (merge.layers.empty()) {
          merge.slot = slot;
          merge.window = static_cast<int32_t>(plane_id);
          merge.depth = DepthForZPos(joining.z_pos);
        }
        merge.layers.push_back(MergeLayerFrom(joining.layer));
        merge.source_ids.push_back(joining.layer.bi
                                       ? joining.layer.bi->unique_id
                                       : 0);
        continue;
      }

      if (!DescribeWindow(joining.layer, plane_id,
                          DepthForZPos(joining.z_pos), panel_w, panel_h,
                          &windows[slot]))
        return nullptr;

      handles[slot] = joining.layer.bi ? NativeHandleOf(*joining.layer.bi)
                                       : nullptr;
    }
  }

  /* Clipped to the panel before anything else is derived from it.
   *
   * A layer sliding off an edge puts part of its rectangle outside the
   * panel, and a group's corner outside the panel is a position this
   * hardware cannot be told: the window's coordinates are unsigned
   * thirteen-bit fields, the kernel passes them through unchecked -- its
   * own comment says so -- and a negative number wraps into a position far
   * off the panel, where the window is silently never scanned. So the
   * visible part is kept, the rest is cut away here: each rectangle is
   * intersected with the panel and its source trimmed in proportion, and a
   * member nothing of which is visible leaves the group. What remains is a
   * group that lies on the panel whole, by construction. */
  if (!merge.layers.empty() && panel_w > 0 && panel_h > 0) {
    size_t kept = 0;
    for (size_t i = 0; i < merge.layers.size(); ++i) {
      hwc::VicSession::Layer l = merge.layers[i];
      const int32_t cl = std::max(l.display_left, 0);
      const int32_t ct = std::max(l.display_top, 0);
      const int32_t cr = std::min(l.display_right, panel_w);
      const int32_t cb = std::min(l.display_bottom, panel_h);
      if (cr <= cl || cb <= ct)
        continue;

      const auto dw = static_cast<float>(l.display_right - l.display_left);
      const auto dh = static_cast<float>(l.display_bottom - l.display_top);
      if (dw > 0 && dh > 0) {
        const float sx = (l.source_right - l.source_left) / dw;
        const float sy = (l.source_bottom - l.source_top) / dh;
        l.source_left += static_cast<float>(cl - l.display_left) * sx;
        l.source_top += static_cast<float>(ct - l.display_top) * sy;
        l.source_right -= static_cast<float>(l.display_right - cr) * sx;
        l.source_bottom -= static_cast<float>(l.display_bottom - cb) * sy;
      }
      l.display_left = cl;
      l.display_top = ct;
      l.display_right = cr;
      l.display_bottom = cb;

      merge.layers[kept] = l;
      merge.source_ids[kept] = merge.source_ids[i];
      ++kept;
    }
    merge.layers.resize(kept);
    merge.source_ids.resize(kept);
  }

  /* The group's own frame of reference, found now that its members are
   * known. The layers' panel rectangles are rebased to the group's corner:
   * the engine will draw them from the buffer's origin, and where the group
   * sits on the panel becomes the window's business alone. */
  if (!merge.layers.empty()) {
    int32_t left = INT32_MAX;
    int32_t top = INT32_MAX;
    int32_t right = INT32_MIN;
    int32_t bottom = INT32_MIN;
    for (const auto &l : merge.layers) {
      left = std::min(left, l.display_left);
      top = std::min(top, l.display_top);
      right = std::max(right, l.display_right);
      bottom = std::max(bottom, l.display_bottom);
    }
    merge.origin_x = left;
    merge.origin_y = top;
    merge.width = right > left ? static_cast<uint32_t>(right - left) : 1;
    merge.height = bottom > top ? static_cast<uint32_t>(bottom - top) : 1;

    for (auto &l : merge.layers) {
      l.display_left -= left;
      l.display_right -= left;
      l.display_top -= top;
      l.display_bottom -= top;
    }
  }

  /* The proposal must weigh the merged window too. Its buffer does not
   * exist yet -- the engine draws it at execute, after exactly one plan has
   * won -- but the bandwidth question never needed the buffer: the kernel
   * counts a window from its geometry and format alone, and asks only that
   * the buffer field be positive. So the window the engine will fill is
   * described here as it will really be scanned out -- the group's own
   * rectangle, thirty-two-bit rows -- with a stand-in descriptor nothing
   * ever resolves: the test path discards the request after asking, and
   * the execute path rebuilds this window from the real buffer before
   * posting. Described full-panel, as it long was, every proposal carried
   * a whole screen of imaginary bandwidth for what is usually a strip. */
  if (!merge.layers.empty() && !modes_.empty()) {
    const drmModeModeInfo &mode = modes_.front().GetRawMode();
    hwc::DcHead::Window &window = windows[merge.slot];
    window = hwc::DcHead::Window{};
    window.index = merge.window;
    window.bufferFd = 1; /* positive is all the proposal reads */
    window.stride = static_cast<uint32_t>(mode.hdisplay) * 4;
    window.pixelFormat = TEGRA_DC_EXT_FMT_R8G8B8A8;
    window.sourceWidth = static_cast<float>(merge.width);
    window.sourceHeight = static_cast<float>(merge.height);
    window.outX = merge.origin_x;
    window.outY = merge.origin_y;
    window.outWidth = static_cast<int32_t>(merge.width);
    window.outHeight = static_cast<int32_t>(merge.height);
    window.z = merge.depth;
    window.blend = TEGRA_DC_EXT_BLEND_PREMULT;
  }

  return std::make_unique<TegraAtomicRequest>(std::move(windows),
                                              std::move(handles),
                                              args.composition != nullptr,
                                              args.power_mode,
                                              std::move(merge),
                                              args.color_matrix);
}

bool TegraAtomicStateManager::Test(const AtomicRequest &request) {
  const auto &tegra = static_cast<const TegraAtomicRequest &>(request);
  return head_.test(tegra.GetWindows()) == 0;
}

bool TegraAtomicStateManager::RecognisesMerge(
    const TegraAtomicRequest::Merge &merge) {
  if (last_merge_window_ < 0) {
    merges_.first_sight++;
    return false;
  }

  if (merge.window != last_merge_window_ ||
      merge.depth != last_merge_depth_ ||
      merge.layers.size() != last_merge_sources_.size() ||
      merge.source_ids.size() != merge.layers.size()) {
    merges_.changed_shape++;
    return false;
  }

  /* The group's size -- a resize really is a different picture. Its PLACE
   * is deliberately not judged: the pixels are drawn relative to the
   * group's own corner, so a group that merely moved is the same picture
   * shown somewhere else, and the window is simply moved under it. That is
   * the whole dividend of the group's frame of reference -- a sliding
   * volume panel, a settling notification, anything that travels without
   * redrawing, stops waking the engine. */
  if (merge.width != last_merge_width_ ||
      merge.height != last_merge_height_) {
    merges_.changed_size++;
    return false;
  }

  for (size_t i = 0; i < merge.layers.size(); ++i) {
    const hwc::VicSession::Layer &l = merge.layers[i];
    const MergedSource &s = last_merge_sources_[i];

    if (merge.source_ids[i] == 0) {
      merges_.nameless++;
      return false;
    }
    if (merge.source_ids[i] != s.id) {
      merges_.changed_identity++;
      return false;
    }

    /* Exact comparison on purpose, floats included: both sides came out of
     * the same description untouched, so anything unequal really is a
     * different frame. */
    if (l.source_left != s.source_left || l.source_top != s.source_top ||
        l.source_right != s.source_right ||
        l.source_bottom != s.source_bottom ||
        l.display_left != s.display_left || l.display_top != s.display_top ||
        l.display_right != s.display_right ||
        l.display_bottom != s.display_bottom) {
      merges_.changed_geometry++;
      return false;
    }
    if (l.premultiplied != s.premultiplied || l.alpha != s.alpha) {
      merges_.changed_blend++;
      return false;
    }
  }

  return true;
}

void TegraAtomicStateManager::RememberMerge(
    const TegraAtomicRequest::Merge &merge,
    const hwc::DcHead::Window &described, const SharedFd &drawn) {
  last_merge_sources_.clear();
  last_merge_sources_.reserve(merge.layers.size());

  const bool named = merge.source_ids.size() == merge.layers.size();
  for (size_t i = 0; i < merge.layers.size(); ++i) {
    const hwc::VicSession::Layer &l = merge.layers[i];
    last_merge_sources_.push_back(
        MergedSource{named ? merge.source_ids[i] : 0, l.source_left,
                     l.source_top, l.source_right, l.source_bottom,
                     l.display_left, l.display_top, l.display_right,
                     l.display_bottom, l.premultiplied, l.alpha});
  }

  last_merge_window_ = merge.window;
  last_merge_depth_ = merge.depth;
  last_merge_width_ = merge.width;
  last_merge_height_ = merge.height;
  last_merge_described_ = described;
  last_merge_fence_ = drawn;
}

int TegraAtomicStateManager::Execute(const AtomicRequest &request,
                                     AtomicCommitResult *out_result) {
  const auto &tegra = static_cast<const TegraAtomicRequest &>(request);

  /* Lighting the panel comes before showing anything on it: a frame posted
   * to a display that is not scanning out never appears, and nothing later
   * repeats it. Going dark is the other way round for the same reason -- the
   * last frame asked for should be on screen when the light goes out. */
  if (tegra.GetPowerMode() && *tegra.GetPowerMode() == PowerMode::kOn) {
    int err = SetPowered(true);
    if (err) {
      /* A frame abandoned before anything was drawn or shown -- but a
       * frame nobody judged all the same. See ForgetMerge. */
      ForgetMerge();
      return err;
    }
  }

  if (!tegra.HasComposition()) {
    /* A commit with no frame is a frame nobody judged -- see ForgetMerge. */
    ForgetMerge();
    if (tegra.GetPowerMode() && *tegra.GetPowerMode() != PowerMode::kOn)
      return SetPowered(false);
    return 0;
  }

  /* The frame's colour transform, before the frame itself: the write is
   * folded in at the next frame boundary whatever the order here, and a
   * frame the controller then refuses leaves colour -- a property of the
   * display, not of any one frame -- pointing the way the framework said. */
  if (cmu_ctm_ && tegra.GetColorMatrix())
    ProgramColorMatrix(*tegra.GetColorMatrix());

  /* Flattened here, and only here, because this is the first point at which
   * it is known which buffers the display will read -- and the last point
   * before it reads them.
   *
   * What the GPU draws is compressed: beside the pixels it keeps a smaller
   * record of each tile and writes only that where it can. A display that
   * understands the arrangement reads both; this one does not, and reads the
   * record as though it were pixels, which shows as a regular grid laid over
   * a recognisable picture. So the buffers going to a window are flattened
   * back, every frame, because every frame is drawn again.
   *
   * Only those. A layer the planner sent to the client is composed by the GPU,
   * which reads the compressed arrangement natively and gains nothing from a
   * flattened copy -- flattening it is a full pass over the screen thrown
   * away, and there were several of them in every frame.
   *
   * Not done while a plan is merely being weighed, either: the planner asks
   * whether a frame would go up several times before settling on one, and
   * doing the work for each of those would be worse than doing it once for
   * every buffer.
   */
  std::vector<hwc::DcHead::Window> windows = tegra.GetWindows();
  const std::vector<buffer_handle_t> &handles = tegra.GetHandles();

  /* What would not fit a window is drawn now, into a buffer of our own, and
   * the window is told to show that instead.
   *
   * Here rather than where the plan was described, because a plan is weighed
   * several times before one is chosen and only one of those becomes a frame.
   *
   * The result is left out of the handles beside the windows on purpose: that
   * list is what the flattening pass below walks, and this buffer was never
   * drawn by the GPU, so there is nothing in it to flatten.
   */
  SharedFd merged;
  const auto &merge = tegra.GetMerge();
  if (!merge.layers.empty()) {
    if (vic_ == nullptr || scratch_ == nullptr) {
      ALOGE("a frame was planned for an engine this display has not got");
      return -EINVAL;
    }

    if (merge_cache_ && RecognisesMerge(merge)) {
      /* The group is the one already drawn, so the window is shown the
       * buffer it is already showing. No buffer changes hands: the pool is
       * not advanced, nothing is described again, and the layers' own fences
       * need no heeding -- a buffer that kept its identity was not drawn
       * into, or the client would have had to queue a different one for at
       * least the frame in between. The fence below came due when the
       * original drawing finished, so the flip's wait on it is a no-op.
       *
       * A flip that failed goes unnoticed here on purpose: the next frame's
       * reuse submits this same description again, so a dropped frame
       * retries itself out of what is remembered. */
      merges_.reused++;
      windows[merge.slot] = last_merge_described_;
      windows[merge.slot].preFence = last_merge_fence_ ? *last_merge_fence_
                                                       : -1;

      /* Where the group sits now, not where it sat when it was drawn. The
       * pixels are relative to the group's corner, so a moved group is the
       * remembered picture in a new place -- the window follows it. */
      windows[merge.slot].outX = merge.origin_x;
      windows[merge.slot].outY = merge.origin_y;
    } else {
      /* The buffer and, separately, when it may be written to. The engine is
       * told the second and waits for it itself; nothing here does. */
      SharedFd target_ready;
      buffer_handle_t target = scratch_->Next(&target_ready);
      if (target == nullptr) {
        ForgetMerge();
        return -EBUSY;
      }

      const int64_t before_merge = NowNs();
      merged = vic_->Compose(target, merge.layers, merge.width, merge.height,
                             target_ready ? *target_ready : -1);
      if (!merged) {
        /* The engine would not take it. Nothing has been written, so the
         * honest thing is to drop the frame rather than show a window
         * whatever it held before -- and the planner will be asked again
         * for the next one. What was remembered is dropped too: this frame
         * went unjudged, and the argument that keeps a remembered identity
         * honest does not survive a frame nobody watched. The rotation
         * steps back as well -- the slot just taken was never shown, and
         * walking past it strands the rotation ever closer to the buffer
         * the panel is scanning. */
        scratch_->Rewind();
        ForgetMerge();
        ALOGE("the engine refused a frame of %zu layer(s)",
              merge.layers.size());
        return -EINVAL;
      }

      const int64_t took = NowNs() - before_merge;
      merges_.frames++;
      merges_.layers += merge.layers.size();
      merges_.engine_ns += took;
      if (took > merges_.engine_ns_max)
        merges_.engine_ns_max = took;

      auto *gralloc = NvGralloc::GetInstance();
      NvGralloc::Surface surface{};
      const int fd = gralloc != nullptr ? gralloc->GetMemFd(target) : -1;
      if (fd < 0 || !gralloc->DescribeSurface(target, &surface)) {
        /* Drawn, but this frame will never reach the panel: the slot goes
         * back into rotation as the next one to write, and nothing of this
         * frame is remembered. */
        scratch_->Rewind();
        ForgetMerge();
        ALOGE("cannot describe the buffer the engine just drew");
        return -EINVAL;
      }

      hwc::DcHead::Window &window = windows[merge.slot];
      window = hwc::DcHead::Window{};
      window.index = merge.window;
      window.bufferFd = fd;
      window.offset = surface.offset;
      window.stride = surface.pitch;

      /* Ours to know rather than to ask about: this buffer was allocated by
       * this composer, as thirty-two bit colour laid out in rows, which is
       * the one shape the window it is bound for can show. */
      window.pixelFormat = TEGRA_DC_EXT_FMT_R8G8B8A8;
      window.flags = 0;
      window.blockHeightLog2 = 0;

      /* Shown unresized, and only the group's worth of it. The buffer is
       * panel-sized because the pool allocates once for the worst case, but
       * the group lives in its top-left corner and the window reads exactly
       * that: everything about where each layer sits inside the group is
       * already in the pixels, and where the group sits on the panel is
       * said here, in the window's destination. The controller never
       * fetches past the rectangle it is given -- a strip's window costs a
       * strip's bandwidth. */
      window.sourceWidth = static_cast<float>(merge.width);
      window.sourceHeight = static_cast<float>(merge.height);
      window.outX = merge.origin_x;
      window.outY = merge.origin_y;
      window.outWidth = static_cast<int32_t>(merge.width);
      window.outHeight = static_cast<int32_t>(merge.height);
      window.z = merge.depth;
      window.blend = TEGRA_DC_EXT_BLEND_PREMULT;
      window.preFence = *merged;

      RememberMerge(merge, window, merged);
    }
  } else {
    /* No group this frame, so no frame of any group was judged: a layer
     * away from the merge can be redrawn under a kept identity, and this
     * path would not see the buffer that proves it. Forgotten, so that the
     * group's return starts the argument over. */
    ForgetMerge();
  }

  /* Held until the flip has been made. The controller waits on these before
   * reading, and a fence closed while it is still waited on is a frame shown
   * before it was finished. */
  std::vector<SharedFd> flattened;
  flattened.reserve(windows.size());

  const bool flatten = FlatteningWanted();

  /* Split from the flip that follows it, because between them they are the
   * whole of what showing a frame costs and they are answerable in different
   * places -- one is a favour asked of the allocator, the other an ioctl. */
  const int64_t before_flatten = NowNs();
  size_t flattened_count = 0;

  for (size_t i = 0; flatten && i < windows.size() && i < handles.size();
       ++i) {
    if (handles[i] == nullptr || windows[i].bufferFd == 0)
      continue;

    /* Only what has been drawn again since it was last flattened.
     *
     * Flattening is not a property of showing a buffer, it is a property of
     * the buffer: once undone it stays undone until something draws into it
     * again. A window given the same buffer as last time is showing the same
     * pixels, and those pixels are already flat.
     *
     * Most of a frame is like that. The wallpaper is drawn once and stands
     * still; the status bar changes when the clock does. Flattening them on
     * every frame is a full pass over the screen, each, for a picture that
     * did not change -- and there are as many of those passes as there are
     * windows, on a GPU the application needs for its own drawing.
     *
     * The buffer is recognised by the handle the allocator gave it. An
     * application draws into a chain of them in turn, so a handle coming back
     * unchanged from one frame to the next means that layer stood still.
     */
    if (last_flattened_[windows[i].index] == handles[i])
      continue;

    SharedFd ready;
    NvGralloc::GetInstance()->PrepareForScanout(handles[i],
                                                windows[i].preFence, &ready);

    last_flattened_[windows[i].index] = handles[i];

    /* Nothing handed back means nothing to wait for beyond what was already
     * being waited for, so the window keeps the fence it came with. */
    if (!ready)
      continue;

    windows[i].preFence = *ready;
    flattened.push_back(std::move(ready));
    ++flattened_count;
  }

  const int64_t after_flatten = NowNs();

  const int64_t before_flip = NowNs();

  hwc::UniqueFd post_fence;
  int err = head_.flip(windows, &post_fence);
  if (err) {
    /* A frame drawn by the engine and refused by the controller was never
     * shown: the slot it went into is the right one to draw the next
     * attempt into, and its description must not be shown from memory --
     * a reuse would put on the panel a frame the panel never accepted,
     * against a pool that still truthfully names the older buffer. A
     * reused frame that fails here needs neither: nothing was drawn, and
     * what memory describes is exactly what the panel kept showing. */
    if (merged) {
      scratch_->Rewind();
      ForgetMerge();
    }
    return err;
  }

  /* Said even when the flip failed would be wrong -- nothing is showing that
   * buffer then, and holding it back would cost a frame for nothing. Said
   * here, with the fence this flip returned: the buffer is free to be drawn
   * into again once the display has finished reading it, and that is what
   * this fence means. */
  if (merged && scratch_ != nullptr && post_fence)
    scratch_->Presented(MakeSharedFd(::dup(post_fence.get())));

  /* Only when the two together did not fit comfortably inside a refresh --
   * the rest is the ordinary case and says nothing. */
  constexpr int64_t kWorthSaying = 3000000;
  const int64_t after_flip = NowNs();
  if (after_flip - before_flatten > kWorthSaying) {
    HWC_LOGD("slow present: flattened %zu buffer(s) in %" PRId64
             "us, flip %" PRId64 "us",
             flattened_count, (after_flatten - before_flatten) / 1000,
             (after_flip - before_flip) / 1000);
  }

  last_flip_ns_ = before_flip;

  /* Which flip's fence goes back to the framework.
   *
   * The contract is not ambiguous about this. A present fence signals "when
   * the current frame appears on the screen"; a release fence signals "when
   * the HWC is no longer using the previous buffer because the current buffer
   * has replaced the previous buffer on the display". Both name one instant,
   * and that instant belongs to this flip.
   *
   * This used to be a switch, because the true answer once cost whole frames:
   * a fence still pending at the client's next wake-up made it drop the frame
   * outright, with no grace at all. That check is off now (the device's
   * system.prop turns it off), the kernel raises the fence from the vblank
   * itself, and the panel has said its piece: fed the true fence every frame,
   * the client's model of the refresh stops being torn down and rebuilt twice
   * a second, and a run of transitions comes out the same length every time.
   *
   * The loose answers turned out to have prices of their own. The flip before
   * is a fence already come due -- and the framework frees a dying layer's
   * last buffer with exactly this fence, so a stale one hands the buffer back
   * while the panel is still reading it. No fence at all reads as "already
   * free" and frees it the same way. The true fence is also the safe one.
   *
   * The first frame has no predecessor and hands back nothing, which reads as
   * already presented -- it is.
   */
  SharedFd this_post_fence = post_fence ? MakeSharedFd(post_fence.release())
                                        : SharedFd{};

  if (out_result != nullptr) {
    out_result->present_fence = this_post_fence;

    /* The same instant, so the same fence. When these two answers differed,
     * a paragraph stood here reconciling them; with both questions answered
     * truly there is nothing left to reconcile. */
    out_result->release_fence = this_post_fence;

    /* What the engine read, said separately from what the display was given.
     *
     * These buffers were drawn into a scratch buffer of ours and the display
     * was handed that instead, so nothing on the panel is reading them. They
     * stopped being read when the engine finished, which this fence names --
     * and the display has not even started by then, since the flip above was
     * told to wait for this very fence before touching the result.
     *
     * The vendor's own composer did exactly this, and its interface required
     * it in as many words: the fence a composition returns "will be signalled
     * once composition is complete", and the client "is responsible for
     * updating each layer" with it. Ours handed every layer the flip's fence
     * instead, which for these is one to two frames further off than the
     * truth -- and a buffer withheld that long from a client with three of
     * them leaves it drawing into two.
     */
    if (report_engine_reads_ && merged && !merge.layers.empty()) {
      out_result->engine_fence = merged;
      out_result->engine_read.reserve(merge.layers.size());
      for (const auto &layer : merge.layers)
        if (layer.handle != nullptr)
          out_result->engine_read.push_back(layer.handle);
    }
  }

  previous_post_fence_ = std::move(this_post_fence);

  /* Was it already due when it was given away? */
  if (count_fences_) {
    fences_.frames++;
    if (out_result == nullptr || !out_result->present_fence) {
      fences_.without_fence++;
    } else {
      switch (FenceDue(out_result->present_fence)) {
        case Due::kYes:
          fences_.already_due++;
          break;
        case Due::kNotYet:
          fences_.not_yet_due++;
          break;
        case Due::kCouldNotAsk:
          fences_.could_not_ask++;
          break;
      }
    }
  }

  /* How late the fence just handed out comes due, asked one frame later.
   *
   * It is the client's release fence as well as this frame's present fence,
   * and the client cannot draw into the buffer behind it until it is due. A
   * fence trailing the frame it belongs to is a client standing still with
   * nothing to blame -- from its side a late fence and a slow application look
   * exactly alike.
   *
   * Replaced every frame now, come due or not. Held until it signalled -- what
   * this did before -- the timestamp beside it went on naming an older frame
   * while the fence moved on, so the interval reported was however many frames
   * had gone by in the meantime. It read as tens of milliseconds of lateness
   * that were never there, and it was believed for a while.
   */
  if (handed_out_fence_) {
    const std::optional<int64_t> due = SignalTimeNs(handed_out_fence_);

    /* Free: the asking has already been done for the warning below. */
    if (count_fences_) {
      if (due)
        fences_.due_a_frame_later++;
      else
        fences_.still_not_due++;
    }

    constexpr int64_t kTwoRefreshesNs = 33326654;
    if (due && *due - handed_out_ns_ > kTwoRefreshesNs) {
      /* Said outright rather than behind the trace switch. That switch guards
       * what is said about every frame, which is paid for whether or not
       * anything is wrong; this is said only when something is. */
      HWC_LOGW("the fence handed out last frame came due %" PRId64
               "us after the flip it belongs to",
               (*due - handed_out_ns_) / 1000);
    }
  }

  handed_out_fence_ = out_result != nullptr ? out_result->present_fence
                                            : SharedFd{};
  handed_out_ns_ = before_flip;

  return 0;
}

std::string TegraAtomicStateManager::DumpState() {
  std::stringstream ss;

  /* The engine's work, if this display has one. Read and reset the interval
   * counters so two dumps around a transition describe that transition; the
   * accepted/refused tallies come straight from the engine and run for its
   * whole life. */
  if (vic_ != nullptr) {
    const MergeCounters m = merges_;
    merges_ = {};

    ss << "Merges since last dumpsys request:\n"
       << "  frames merged           : " << m.frames << "\n"
       << "  shown from memory       : " << m.reused << "\n";
    if (m.frames != 0)
      ss << "  layers per merge (avg)  : "
         << (m.layers / m.frames) << "\n"
         << "  engine us per merge     : "
         << (m.engine_ns / 1000 / static_cast<int64_t>(m.frames)) << "\n"
         << "  engine us worst         : " << (m.engine_ns_max / 1000)
         << "\n";
    /* Gated on the causes as well as the outcomes: an interval of nothing
     * but engine refusals draws no frame and reuses none, and the causes
     * are then the only line explaining what kept being attempted. */
    const uint64_t causes = m.first_sight + m.changed_shape +
                            m.changed_identity + m.changed_geometry +
                            m.changed_size + m.changed_blend + m.nameless;
    if (m.frames != 0 || m.reused != 0 || causes != 0)
      ss << "  drawn because           : first " << m.first_sight
         << ", shape " << m.changed_shape << ", buffer "
         << m.changed_identity << ", geometry " << m.changed_geometry
         << ", size " << m.changed_size << ", blend " << m.changed_blend
         << ", nameless " << m.nameless << "\n";
    ss << "Engine over its lifetime:\n"
       << "  frames accepted         : " << vic_->composed() << "\n"
       << "  frames refused          : " << vic_->refused() << "\n\n";
  }

  /* Quiet when colour was never asked to change: most dumps, on a display
   * that spends its life at the identity. */
  {
    const CmuCounters c = cmu_;
    cmu_ = {};
    const uint64_t any = c.applied + c.restored + c.skipped_offset;
    if (any != 0 || csc_programmed_) {
      ss << "Colour transforms since last dumpsys request:\n"
         << "  written to the pipeline : " << c.applied << "\n"
         << "  boot state restored     : " << c.restored << "\n"
         << "  skipped, offset         : " << c.skipped_offset << "\n"
         << "  approximated in shape   : " << c.approximated << "\n"
         << "  holding a transform now : " << (csc_programmed_ ? "yes" : "no")
         << "\n\n";
    }
  }

  if (!count_fences_)
    return ss.str();

  const FenceCounters c = fences_;
  /* Read and reset, so that two dumps around one transition describe that
   * transition and nothing else -- which is how the composition statistics
   * beside it are already read. */
  fences_ = {};

  ss << "Fences handed to the framework since last dumpsys request:\n"
     << "  frames                  : " << c.frames << "\n"
     << "  already due when given  : " << c.already_due << "\n"
     << "  not yet due when given  : " << c.not_yet_due << "\n"
     << "  no fence given at all   : " << c.without_fence << "\n"
     << "  due one frame later     : " << c.due_a_frame_later << "\n"
     << "  still not due then      : " << c.still_not_due << "\n";
  if (c.could_not_ask != 0)
    ss << "  could not be asked      : " << c.could_not_ask << "\n";

  return ss.str();
}

bool TegraAtomicStateManager::CountFencesFromProperty() {
  return property_get_bool("vendor.hwc.fencestats", 0) != 0;
}

bool TegraAtomicStateManager::ThrottleFromProperty() {
  /* On unless told otherwise: it is what upstream and every vendor does, and
   * an unbounded flip queue is hard to defend whatever it turns out to cost
   * here. Off is for measuring it. */
  return property_get_bool("vendor.hwc.throttle", 1) != 0;
}

bool TegraAtomicStateManager::EngineReadsFromProperty() {
  /* On unless told otherwise. Off restores what this did before: every layer
   * waits for the flip, whether or not the display ever read it. */
  return property_get_bool("vendor.hwc.enginefence", 1) != 0;
}

bool TegraAtomicStateManager::MergeCacheFromProperty() {
  /* On unless told otherwise. Off draws the merge afresh on every frame,
   * which is what this did before -- kept switchable only so the two can be
   * measured against each other on the panel without building twice. */
  return property_get_bool("vendor.hwc.mergecache", 1) != 0;
}

bool TegraAtomicStateManager::CmuFromProperty() {
  /* On unless told otherwise. Off leaves the head's colour pipeline at its
   * boot state and the framework's matrices unapplied, which is what this
   * did before the pipeline had a consumer. */
  return property_get_bool("vendor.hwc.cmu", 1) != 0;
}

/* The panel's gamut corrected toward sRGB, in linear light -- the inner
 * factor under every framework matrix, and the resting state when the
 * calibrated mode is chosen.
 *
 * Reconstructed, not measured: no factory colorimetry for this module
 * exists anywhere public, so the primaries are the ones this 63%-of-sRGB
 * class of white-LED panel is documented to have, anchored to lab
 * measurements of the same Sharp module in another product. Rows sum to
 * one -- white stays white, and no non-negative input can drive the
 * matrix stage negative. Typical-unit accuracy, not this-unit accuracy:
 * expected to cut the native hue error roughly in half, never to
 * eliminate it; a measured matrix can replace these numbers without
 * touching anything else. */
static constexpr float kPanelToSrgb[9] = {1.014F,  -0.057F, 0.043F,
                                          -0.092F, 1.218F,  -0.126F,
                                          -0.073F, -0.115F, 1.188F};

/* Row-major 3x3 product: out = a * b. */
static void MultiplyCsc(const float a[9], const float b[9], float out[9]) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] +
                       a[r * 3 + 1] * b[1 * 3 + c] +
                       a[r * 3 + 2] * b[2 * 3 + c];
}

void TegraAtomicStateManager::ProgramColorMatrix(
    const HalColorTransformMatrix &matrix) {
  /* The steady state: the same matrix arrives with every frame for as long
   * as nothing changes, and must cost this comparison and nothing else. */
  if (color_matrix_seen_ &&
      std::equal(matrix.begin(), matrix.end(), last_color_matrix_.begin()))
    return;
  last_color_matrix_ = matrix;
  color_matrix_seen_ = true;

  /* Everything below runs once per change of matrix. */

  constexpr float kEps = 1e-4F;
  const float *m = matrix.data();

  /* Column-major, as the client API hands it: m[col * 4 + row]. The fourth
   * column is the offset, the bottom row the projective weight. */
  /* The first restore after start writes home even when this instance never
   * programmed anything: a predecessor may have died mid-transform, its
   * state outlives it in the kernel, and a computed home cannot see it --
   * only overwrite it. Once written, an untouched pipeline is left alone.
   * Home is the boot pipeline, or the panel correction over it when the
   * calibrated mode is chosen; the true boot state returns when the head
   * is torn down. */
  const auto restore = [this]() {
    if (!csc_programmed_ && boot_state_written_)
      return;
    const bool written = calibrated_home_
                             ? head_.setColorMatrix(kPanelToSrgb, 0.F)
                             : head_.writeBootState();
    if (written) {
      if (csc_programmed_)
        cmu_.restored++;
      csc_programmed_ = false;
      boot_state_written_ = true;
    }
  };

  bool identity = true;
  for (int i = 0; i < 16 && identity; ++i)
    identity = fabsf(m[i] - (i % 5 == 0 ? 1.F : 0.F)) < kEps;
  if (identity) {
    restore();
    return;
  }

  /* Row-major out = M * in, which is the order the head takes, and the
   * offset column as the addition it is. */
  float rm[9];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      rm[r * 3 + c] = m[c * 4 + r];

  const float offset = (m[12] + m[13] + m[14]) / 3.F;
  const bool has_offset = fabsf(m[12]) > kEps || fabsf(m[13]) > kEps ||
                          fabsf(m[14]) > kEps;

  bool negative = false;
  for (float v : rm)
    negative = negative || v < -kEps;

  /* The framework's inversion family, matched by decomposition rather than
   * by shape: the verbatim inversion alone, and the same inversion under a
   * night tint, which the framework multiplies into one matrix -- the tint
   * outermost, so each ROW of the product is the inversion's row scaled by
   * that channel's factor, and the offset column carries the same factors
   * rather than staying uniform. Each factor is recovered by least squares
   * over its row, the residual rejects impostors, and the offset must echo
   * the factors -- the affine half of the same composition -- or this is
   * not that family. The true inversion's cross-channel character becomes
   * the per-channel flip: the same feature with a different hue mapping,
   * counted as the approximation it is. The panel correction is not
   * composed here -- its linear-light matrix has no seat in this
   * gamma-domain pipeline, and an inverted screen is no place to judge
   * gamut fidelity from anyway. */
  if (negative && has_offset && offset > 0.4F) {
    static constexpr float kInvert[9] = {0.402F,  -1.174F, -0.228F,
                                         -0.598F, -0.174F, -0.228F,
                                         -0.599F, -1.175F, 0.772F};
    float tint[3];
    bool family = true;
    for (int r = 0; r < 3 && family; ++r) {
      float num = 0.F;
      float den = 0.F;
      for (int c = 0; c < 3; ++c) {
        num += rm[r * 3 + c] * kInvert[r * 3 + c];
        den += kInvert[r * 3 + c] * kInvert[r * 3 + c];
      }
      tint[r] = num / den;
      family = tint[r] > 0.F && fabsf(m[12 + r] - tint[r]) < 0.05F;
    }
    for (int i = 0; i < 9 && family; ++i)
      family = fabsf(rm[i] - tint[i / 3] * kInvert[i]) < 0.05F;

    if (family) {
      if (head_.setInversion(tint)) {
        csc_programmed_ = true;
        boot_state_written_ = true;
        cmu_.applied++;
        cmu_.approximated++;
      }
      return;
    }
  }

  /* An offset the family above did not claim has no home here: the matrix
   * stage has no addend, and folding a bare positive offset into the
   * regamma would lift its floor -- a brightened screen with dead shadow
   * contrast, worse than an untransformed frame. Nothing sends one. */
  if (has_offset) {
    restore();
    cmu_.skipped_offset++;
    return;
  }

  /* The framework's matrix is meant for gamma-encoded values -- that is how
   * the GPU applies it -- while this matrix stage sits between a degamma
   * and a regamma table and multiplies linear light, where the shadows keep
   * a third more distinguishable levels. For a diagonal matrix the two
   * domains reconcile on a power law: raising the factor to an exponent
   * makes scale-then-encode track encode-then-scale. The exponent is fit,
   * not read off sRGB: minimising the worst mismatch against the GPU's own
   * application across the full level range puts it at 2.1 -- the
   * transfer's linear toe drags it below the curve's 2.4 -- for a worst
   * case of ~5 of 255 at the strongest night tint. Cross-channel terms
   * have no such bridge and go in as they are; the divergence from the
   * gamma-domain reference is real (large on daltonism simulations) and
   * accepted deliberately: the linear domain wins on shadow precision and
   * on colourimetry, and the reference itself is only gamma-domain because
   * the framework has not gone linear yet. A non-uniform offset rides on
   * the channels' average -- the table it folds into is shared -- and
   * nothing the framework sends today has one. */
  constexpr float kDisplayGamma = 2.1F;
  const bool diagonal = fabsf(rm[1]) < kEps && fabsf(rm[2]) < kEps &&
                        fabsf(rm[3]) < kEps && fabsf(rm[5]) < kEps &&
                        fabsf(rm[6]) < kEps && fabsf(rm[7]) < kEps;
  if (diagonal) {
    rm[0] = powf(rm[0], kDisplayGamma);
    rm[4] = powf(rm[4], kDisplayGamma);
    rm[8] = powf(rm[8], kDisplayGamma);
  }

  /* The panel correction sits inside every framework transform: both live
   * in linear light by this point, so the composition is a product, the
   * correction nearest the panel. */
  if (calibrated_home_) {
    float composed[9];
    MultiplyCsc(rm, kPanelToSrgb, composed);
    memcpy(rm, composed, sizeof(composed));
  }

  if (head_.setColorMatrix(rm, 0.F)) {
    csc_programmed_ = true;
    boot_state_written_ = true;
    cmu_.applied++;
    if (!diagonal)
      cmu_.approximated++;
  }
}

int TegraAtomicStateManager::SetPowered(bool powered) {
  if (active_ == powered)
    return 0;

  int err = hwc::setPanelPowered(head_.index(), powered);
  if (err)
    return err;

  active_ = powered;
  return 0;
}

void TegraAtomicStateManager::WaitLastFrame() {
  /* Keep one frame in the air, no more.
   *
   * This used to do nothing, on the reasoning that the flip does not block and
   * the fence it hands back is the caller's to wait on. Both halves of that are
   * true and the conclusion still does not follow: the flip is queued, and
   * nothing else in this composer bounds how deep that queue may get. Left
   * alone, we run ahead of the panel -- and then a window's syncpoint maximum
   * stands not one step above its minimum but as many as there are flips
   * waiting, so the fence of any one of them needs that many frames rather than
   * one. The frame the framework is holding a buffer for is not the frame the
   * panel is about to show.
   *
   * It is measurable from the other side: with this empty, the recents surface
   * sits at queued-frames=2 through the transition, its producer two frames
   * ahead of the display with nothing free to draw into.
   *
   * Everyone else does this. Upstream waits on the prior present fence before
   * every commit; Intel calls it "in-flight frames to 1"; Samsung waits up to
   * five refreshes on the previous retire fence before submitting a config.
   * This composer was the only one of the four with no bound at all.
   *
   * The caller waits here, after the next frame has been described and before
   * it is posted, which is where upstream waits too. Describing a frame
   * therefore overlaps the previous one reaching the panel; only the posting
   * of it waits.
   *
   * Half a second, matching upstream: long enough that no honest frame ever
   * reaches it, short enough that a display which has stopped answering does
   * not take the composer down with it.
   */
  if (!throttle_to_one_frame_ || !previous_post_fence_)
    return;

  constexpr int kTimeoutMs = 500;
  if (sync_wait(*previous_post_fence_, kTimeoutMs) < 0)
    HWC_LOGE("the frame before this one never reached the panel");
}

}  // namespace android::drm_hwcomposer
