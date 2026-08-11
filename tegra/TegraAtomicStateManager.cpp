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
#include <ndk/sync.h>
#include <time.h>

#include <optional>

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

  TegraAtomicRequest::Merge merge;

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
        merge.layers.push_back(MergeLayerFrom(joining.layer));
        merge.slot = slot;
        merge.window = static_cast<int32_t>(plane_id);
        merge.depth = DepthForZPos(joining.z_pos);
        continue;
      }

      if (!DescribeWindow(joining.layer, plane_id,
                          DepthForZPos(joining.z_pos), &windows[slot]))
        return nullptr;

      handles[slot] = joining.layer.bi ? NativeHandleOf(*joining.layer.bi)
                                       : nullptr;
    }
  }

  return std::make_unique<TegraAtomicRequest>(std::move(windows),
                                              std::move(handles),
                                              args.composition != nullptr,
                                              args.power_mode,
                                              std::move(merge));
}

bool TegraAtomicStateManager::Test(const AtomicRequest &request) {
  const auto &tegra = static_cast<const TegraAtomicRequest &>(request);
  return head_.test(tegra.GetWindows()) == 0;
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
    if (err)
      return err;
  }

  if (!tegra.HasComposition()) {
    if (tegra.GetPowerMode() && *tegra.GetPowerMode() != PowerMode::kOn)
      return SetPowered(false);
    return 0;
  }

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

    buffer_handle_t target = scratch_->Next();
    if (target == nullptr)
      return -EBUSY;

    merged = vic_->Compose(target, merge.layers);
    if (!merged) {
      /* The engine would not take it. Nothing has been written, so the honest
       * thing is to drop the frame rather than show a window whatever it held
       * before -- and the planner will be asked again for the next one. */
      ALOGE("the engine refused a frame of %zu layer(s)", merge.layers.size());
      return -EINVAL;
    }

    auto *gralloc = NvGralloc::GetInstance();
    NvGralloc::Surface surface{};
    const int fd = gralloc != nullptr ? gralloc->GetMemFd(target) : -1;
    if (fd < 0 || !gralloc->DescribeSurface(target, &surface)) {
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
     * this composer, as thirty-two bit colour laid out in rows, which is the
     * one shape the window it is bound for can show. */
    window.pixelFormat = TEGRA_DC_EXT_FMT_R8G8B8A8;
    window.flags = 0;
    window.blockHeightLog2 = 0;

    /* Shown whole and unresized. Everything about where each layer went is
     * already in the pixels; the window's only job is to put them on the
     * panel and blend them over what is underneath. */
    window.sourceWidth = static_cast<float>(surface.width);
    window.sourceHeight = static_cast<float>(surface.height);
    window.outWidth = static_cast<int32_t>(surface.width);
    window.outHeight = static_cast<int32_t>(surface.height);
    window.z = merge.depth;
    window.blend = TEGRA_DC_EXT_BLEND_PREMULT;
    window.preFence = *merged;
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
  if (err)
    return err;

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

  /* The fence handed out is the one the flip before this got, not this
   * flip's.
   *
   * The driver builds a flip's fence one step past the counter that flip
   * advances, so it does not come due until the following flip finishes.
   * Handed over as this frame's, that is a deadlock rather than a pessimism:
   * the framework attaches it to the buffer it drew the previous frame into,
   * then waits on it before drawing the next one -- into that same buffer,
   * there being only two. The frame that would release it is the frame that
   * cannot start.
   *
   * Shifting by one says what is actually being asked. The previous flip's
   * fence comes due exactly when this flip finishes, which is the moment this
   * frame is on the panel and the one before it is no longer being read. The
   * first frame has no predecessor and so hands back nothing, which reads as
   * already presented -- it is.
   */
  /* How late the fence this frame hands out actually comes due.
   *
   * It is the client's release fence as well as this frame's present fence,
   * and the client cannot draw into the buffer behind it until it is due. If
   * it comes due later than the frame it belongs to, the client waits -- with
   * nothing to do and nothing to blame, because from its side a late fence and
   * a slow application look the same.
   *
   * Asked of the fence handed out two frames ago, so that there has been time
   * for it to come due, and said only when it took longer than the refresh it
   * was supposed to arrive with. In the ordinary case that is silence.
   */
  if (handed_out_fence_) {
    const std::optional<int64_t> due = SignalTimeNs(handed_out_fence_);
    if (due) {
      /* Against the flip that is supposed to make it due, not against the
       * moment it was handed over.
       *
       * The fence given away belongs to the flip before this one, and what
       * makes it due is this flip reaching the panel. So the question is how
       * long after this flip was posted it arrived. One refresh is the honest
       * answer -- a flip takes effect at the next blank. Two means the fence
       * trails the frame it belongs to by a whole frame, and everything
       * holding a buffer behind it waits that much longer for nothing.
       *
       * Measured the other way, from the hand-over, a frame that was itself
       * late drags the fence along with it and the fence takes the blame for
       * being a symptom.
       */
      constexpr int64_t kTwoRefreshesNs = 33326654;
      const int64_t after_flip = *due - handed_out_ns_;
      /* Said outright rather than behind the trace switch. That switch guards
       * what is said about every frame, which is paid for whether or not
       * anything is wrong; this is said only when something is. */
      if (after_flip > kTwoRefreshesNs) {
        HWC_LOGW("release fence came due %" PRId64
                 "us after the flip that frees it",
                 after_flip / 1000);
      }
      handed_out_fence_ = {};
    }
  }

  if (!handed_out_fence_ && previous_post_fence_) {
    handed_out_fence_ = previous_post_fence_;
    /* The flip posted in this same call is what makes it due. */
    handed_out_ns_ = before_flip;
  }

  if (out_result != nullptr)
    out_result->present_fence = std::move(previous_post_fence_);

  previous_post_fence_ = post_fence ? MakeSharedFd(post_fence.release())
                                    : SharedFd{};

  return 0;
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
  /* Nothing to wait for that anyone is waiting on. The flip call does not
   * block, and what it hands back is a fence the caller already holds -- so
   * whoever needs the frame to have landed waits on that, and there is no
   * second notion of "the last frame" to keep here. */
}

}  // namespace android::drm_hwcomposer
