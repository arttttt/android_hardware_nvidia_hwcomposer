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

#pragma once

#include <cutils/native_handle.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "display/AtomicStateManager.h"
#include "display/DrmMode.h"
#include "tegra/DcHead.h"
#include "tegra/ScratchPool.h"
#include "tegra/VicSession.h"

namespace android::drm_hwcomposer {

/* One commit, described in the terms this hardware takes.
 *
 * Built from the arguments and then handed back to be carried out, so that
 * deciding what to do and doing it stay two steps -- the first can be asked
 * "would this work" without the second happening.
 *
 * A commit is not always a frame. Upstream puts the frame, the timing and
 * whether the display is lit into one atomic request, because on a DRM
 * display all three are properties of the same objects. Here they are three
 * different devices, so the request carries them separately and the order
 * they are applied in is decided when it is executed.
 */
class TegraAtomicRequest : public AtomicRequest {
 public:
  /* What is to be drawn by the image compositor rather than shown straight,
   * and which window slot the result goes to.
   *
   * Empty on every frame that needs no merge, which is most of them. */
  struct Merge {
    std::vector<hwc::VicSession::Layer> layers;
    size_t slot;
    int32_t window;
    uint32_t depth;
  };

  TegraAtomicRequest(std::vector<hwc::DcHead::Window> windows,
                     std::vector<buffer_handle_t> handles,
                     bool has_composition,
                     std::optional<PowerMode> power_mode,
                     Merge merge = {})
      : windows_(std::move(windows)),
        handles_(std::move(handles)),
        has_composition_(has_composition),
        power_mode_(power_mode),
        merge_(std::move(merge)) {
  }

  const Merge &GetMerge() const {
    return merge_;
  }

  const std::vector<hwc::DcHead::Window> &GetWindows() const {
    return windows_;
  }

  /* The buffer behind each window, as the allocator knows it, in step with
   * GetWindows(); null where a window shows nothing.
   *
   * Carried because a window is described in the controller's terms and the
   * allocator answers to none of them: preparing a buffer for the display
   * takes its own handle. Kept beside the windows rather than inside one,
   * because it is not something the controller is told -- it is what has to
   * happen before the controller is told anything.
   */
  const std::vector<buffer_handle_t> &GetHandles() const {
    return handles_;
  }

  /* Whether anything is to be shown. A commit that only changes the power
   * state must not post a frame: every window of the head goes into a flip,
   * so flipping without a composition would blank the display as a side
   * effect of turning it on. */
  bool HasComposition() const {
    return has_composition_;
  }

  const std::optional<PowerMode> &GetPowerMode() const {
    return power_mode_;
  }

 private:
  const std::vector<hwc::DcHead::Window> windows_;
  const std::vector<buffer_handle_t> handles_;
  const bool has_composition_;
  const std::optional<PowerMode> power_mode_;
  const Merge merge_;
};

/* Turns plans into frames on this controller.
 *
 * Every window of the head goes into every frame, whether or not a layer
 * claimed it: a window keeps what it was last given until told otherwise, so
 * one left out of a frame stays on screen over the top of it.
 */
class TegraAtomicStateManager : public AtomicStateManager {
 public:
  /* All four belong to the pipeline and outlive this. `vic` and `scratch` are
   * null together on a device that was not asked for the image compositor,
   * which is every device by default; nothing then reaches the merge. */
  TegraAtomicStateManager(hwc::DcHead &head,
                          const std::vector<DrmMode> &modes,
                          hwc::VicSession *vic, hwc::ScratchPool *scratch)
      : head_(head), modes_(modes), vic_(vic), scratch_(scratch) {
    present_fence_source_ = PresentFenceFromProperty();
  }

  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs &args) override;

  bool IsActive() const override {
    return active_;
  }

  void WaitLastFrame() override;

  void SetActive(bool active) {
    active_ = active;
  }

  /* Would the controller take this frame? Nothing is shown and nothing
   * changes. */
  bool Test(const AtomicRequest &request);

  /* Shows it. The fence handed back signals when this frame is on the panel;
   * see the note in the implementation on why that is not the fence this
   * flip returned. */
  int Execute(const AtomicRequest &request, AtomicCommitResult *out_result);

 private:
  /* Lights the panel or puts it out, and remembers which. Not the head's
   * job: the controller posts frames and has no say over whether the display
   * is lit, so this goes to the framebuffer device on the same hardware. */
  int SetPowered(bool powered);

  hwc::DcHead &head_;

  /* The timings this panel runs, to check a requested one against. A fixed
   * panel has one, so the only mode that ever arrives here is the one already
   * in use -- but a request for another is a mistake worth refusing rather
   * than accepting and not carrying out. */
  const std::vector<DrmMode> &modes_;

  /* The engine that draws what will not fit a window, and somewhere for it to
   * write. Null together where the device was not asked for them. */
  hwc::VicSession *const vic_ = nullptr;
  hwc::ScratchPool *const scratch_ = nullptr;

  bool active_ = true;

  /* What each window was last given, and so what has already been flattened.
   *
   * A buffer stays flat until something draws into it again, and a window
   * handed the same buffer as last time is showing pixels that were flattened
   * then. Keyed by window rather than by buffer because that is the question
   * being asked -- what is on this window now against what was on it before.
   */
  std::map<int32_t, buffer_handle_t> last_flattened_;

  /* Which flip's fence a present is answered with.
   *
   * The contract has one answer -- HWC2 says the present fence signals "at the
   * vsync when the result of composition of this frame starts to appear", and
   * a release fence signals once the device "has finished reading from the
   * buffer presented in the prior frame", which is the same instant. Both are
   * this flip's fence.
   *
   * It is a switch anyway, because what this composer has always done is the
   * other thing, and the reason given for it is a claim about the hardware
   * that wants testing against the panel rather than against reason. Read once
   * at construction: nothing here belongs in the path a frame takes.
   */
  enum class PresentFence {
    /* The flip before this one. What was done until now. */
    kPreviousFlip,
    /* This flip. What the contract asks for. */
    kThisFlip,
    /* None at all -- which the contract reads as "already free", so the client
     * never waits. Diagnostic: it says whether waiting is what costs us,
     * at the price of drawing into a buffer still being read. */
    kNone,
  };
  PresentFence present_fence_source_ = PresentFence::kPreviousFlip;

  /* Asked of the system once, at construction. Not in Execute: what a frame
   * costs is the one thing being measured here, and a measurement that adds
   * to it is worth nothing. */
  static PresentFence PresentFenceFromProperty();

  /* The fence the previous flip returned. */
  SharedFd previous_post_fence_;

  /* When the last flip was posted. Diagnostic only, read by the trace. */
  int64_t last_flip_ns_ = 0;

  /* A copy of the fence given to the client, and the moment it was given.
   * Kept so that a later frame can ask how long it actually took to come due:
   * a fence handed out as this frame's and coming due two frames later is a
   * client standing still with nothing to blame. Diagnostic only. */
  SharedFd handed_out_fence_;
  int64_t handed_out_ns_ = 0;
};

}  // namespace android::drm_hwcomposer
