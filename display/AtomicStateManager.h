/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "display/DrmMode.h"
#include "drm/drm_mode.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

template <typename T>
class BindingOwner;

class FbIdHandle;
class Plane;
struct LayerToPlaneJoiningPlan;

enum class HwcColorspace;
enum class ContentProtection;
enum class ContentType;
enum class HdcpContentType;
enum class PanelOrientation;
enum class TransferFunction;

struct AtomicCommitArgs {
  /* inputs. All fields are optional, but at least one has to be specified */
  bool blocking = false;
  bool teardown = false;
  bool seamless = false;
  std::optional<DrmMode> display_mode;
  std::optional<PowerMode> power_mode;
  std::shared_ptr<LayerToPlaneJoiningPlan> composition;
  std::shared_ptr<const HalColorTransformMatrix> color_matrix;
  std::optional<HwcColorspace> colorspace;
  std::optional<TransferFunction> transfer_func;
  std::optional<ContentType> content_type;
  std::shared_ptr<hdr_output_metadata> hdr_metadata;
  std::optional<HdcpContentType> hdcp_content_type;
  std::optional<ContentProtection> content_protection;
  std::optional<int32_t> min_bpc;
  std::optional<float> brightness;
  std::optional<float> hdr_headroom;

  std::shared_ptr<FbIdHandle> writeback_fb;
  SharedFd writeback_release_fence;

  /* helpers */
  auto HasInputs() const -> bool {
    return display_mode || power_mode || composition;
  }
};

struct AtomicCommitResult {
  SharedFd writeback_complete_fence;

  /* When this frame appears, as told to the client.
   *
   * The client reads it for two things at once, and they do not want the same
   * answer. It is the timing of the frame, which is this flip; and it is the
   * client's own permission to keep going, because a frame whose fence has
   * not come due by the next refresh is a frame the client concludes it
   * missed and answers by skipping the refresh entirely. */
  SharedFd present_fence;

  /* When the buffers this frame replaced stop being read.
   *
   * The same instant, said for a different purpose, and it must be this
   * flip's whatever the one above is: a buffer handed back before the display
   * has finished with it is drawn over while it is on screen. Told apart from
   * the present fence because only one of the two can be answered loosely,
   * and it is not this one. */
  SharedFd release_fence;

  /* Buffers this frame was not shown from, and when they stopped being read.
   *
   * A buffer the display is given is read for as long as it is on screen, so
   * it is not free until the frame after it appears -- which is what the
   * present fence says and why every layer is told to wait for it.
   *
   * A buffer some other engine was given is read once, while the frame is
   * being put together, and is free the moment that engine is finished. That
   * is sooner than the frame reaches the panel, by however long the frame
   * then waits for the display, and telling its owner otherwise keeps a
   * buffer out of their hands for no reason at all.
   *
   * Empty on a frame the display composed by itself, which is most of them.
   */
  SharedFd engine_fence;
  std::vector<buffer_handle_t> engine_read;
};

class AtomicRequest {
 public:
  virtual ~AtomicRequest() = 0;
};

inline AtomicRequest::~AtomicRequest() = default;

class AtomicStateManager {
 public:
  virtual ~AtomicStateManager() = default;

  virtual std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs &args) = 0;
  virtual bool IsActive() const = 0;
  virtual void WaitLastFrame() = 0;

  /* What this platform's flip machinery has to say in a dump.
   *
   * Empty by default, which is the honest answer for a state manager that
   * counts nothing. It exists because the numbers worth having about a flip --
   * whether the fence handed back had already come due, how many frames went
   * out without one -- are known only where the flip is made, and nowhere
   * above here can ask for them.
   *
   * Read-and-reset is left to the implementation. Both callers of a dump want
   * the same thing the composition statistics already give: what happened
   * since the last time anyone asked.
   */
  virtual std::string DumpState() {
    return {};
  }

  /* A frame came and went without this manager judging it -- the display
   * skipped presenting entirely, or gave up before a request was built.
   * Whatever a manager remembers across frames on the strength of seeing
   * every one of them cannot be trusted past such a gap. No-op by default:
   * a manager that remembers nothing has nothing to forget. */
  virtual void NoteFrameUnjudged() {
  }
};

}  // namespace android::drm_hwcomposer
