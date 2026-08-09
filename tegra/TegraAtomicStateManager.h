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

#include <memory>
#include <vector>

#include "display/AtomicStateManager.h"
#include "tegra/DcHead.h"

namespace android::drm_hwcomposer {

/* A frame, described in the terms the display controller takes.
 *
 * Built from a plan and then handed back to be carried out, so that deciding
 * what to show and telling the hardware to show it stay two steps -- the
 * first can be asked "would this work" without the second happening.
 */
class TegraAtomicRequest : public AtomicRequest {
 public:
  explicit TegraAtomicRequest(std::vector<hwc::DcHead::Window> windows)
      : windows_(std::move(windows)) {
  }

  const std::vector<hwc::DcHead::Window> &GetWindows() const {
    return windows_;
  }

 private:
  const std::vector<hwc::DcHead::Window> windows_;
};

/* Turns plans into frames on this controller.
 *
 * Every window of the head goes into every frame, whether or not a layer
 * claimed it: a window keeps what it was last given until told otherwise, so
 * one left out of a frame stays on screen over the top of it.
 */
class TegraAtomicStateManager : public AtomicStateManager {
 public:
  explicit TegraAtomicStateManager(hwc::DcHead &head) : head_(head) {
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
  hwc::DcHead &head_;
  bool active_ = true;

  /* The fence the previous flip returned. What the driver hands back comes
   * due one flip later, so this is what a present is answered with. */
  SharedFd previous_post_fence_;
};

}  // namespace android::drm_hwcomposer
