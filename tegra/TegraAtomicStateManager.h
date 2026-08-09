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

#include <memory>
#include <optional>
#include <vector>

#include "display/AtomicStateManager.h"
#include "display/DrmMode.h"
#include "tegra/DcHead.h"

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
  TegraAtomicRequest(std::vector<hwc::DcHead::Window> windows,
                     std::vector<buffer_handle_t> handles,
                     bool has_composition,
                     std::optional<PowerMode> power_mode)
      : windows_(std::move(windows)),
        handles_(std::move(handles)),
        has_composition_(has_composition),
        power_mode_(power_mode) {
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
};

/* Turns plans into frames on this controller.
 *
 * Every window of the head goes into every frame, whether or not a layer
 * claimed it: a window keeps what it was last given until told otherwise, so
 * one left out of a frame stays on screen over the top of it.
 */
class TegraAtomicStateManager : public AtomicStateManager {
 public:
  /* Both `head` and `modes` belong to the pipeline and outlive this. */
  TegraAtomicStateManager(hwc::DcHead &head,
                          const std::vector<DrmMode> &modes)
      : head_(head), modes_(modes) {
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

  bool active_ = true;

  /* The fence the previous flip returned. What the driver hands back comes
   * due one flip later, so this is what a present is answered with. */
  SharedFd previous_post_fence_;

  /* When the previous flip was posted, to say how long after it the fence it
   * returned came due. Diagnostic only, and only read by the trace. */
  int64_t last_flip_ns_ = 0;
};

}  // namespace android::drm_hwcomposer
