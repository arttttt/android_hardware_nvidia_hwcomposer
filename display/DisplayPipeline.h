/*
 * Copyright (C) 2022 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's drm/DrmDisplayPipeline.h.
 *
 * The chain a frame travels down to reach one display, and the things built
 * on that chain: something to decide what goes where, something to carry the
 * decision out, and a statement of what the hardware underneath can do.
 *
 * The chain is theirs, link for link and name for name, because their code
 * reaches into it by name -- a display asks `pipeline->connector->Get()` what
 * the panel is and `pipeline->planner` for a plan, with no accessor in
 * between. Each link is now something to implement rather than a DRM object,
 * which is the whole of the adaptation: what a link IS became a question for
 * a backend, while what a link ANSWERS stayed exactly as it was.
 *
 * Building the chain is a backend's business, so the one thing here that
 * cannot be inherited from upstream is their static CreatePipeline: there is
 * no connector to enumerate and be handed. Which planes the chain may use is
 * likewise something only the backend knows, so it is asked rather than
 * worked out.
 */

#pragma once

#include <memory>

/* Complete types rather than declarations: the pipeline owns three of these
 * and its destructor is here, and taking apart a unique pointer needs to
 * know what it points at. Upstream puts its destructor in a source file and
 * so gets away with declarations; this one is a header alone. */
#include "backend/BackendDisplayCapabilities.h"
#include "compositor/CompositionPlanner.h"
#include "display/AtomicStateManager.h"
#include "display/Connector.h"
#include "display/Crtc.h"
#include "display/Encoder.h"
#include "display/PipelineBinding.h"

namespace android {

namespace hwc {
class VSyncSource;
}  // namespace hwc

namespace drm_hwcomposer {

class Device;
class FbImporter;
class Plane;

struct DisplayPipeline {
  virtual ~DisplayPipeline() = default;

  /* The planes this display may put layers on, and the one meant for a
   * cursor if it has one.
   *
   * Handed out as bindings rather than plainly, so that a plane cannot end
   * up claimed by two displays at once: holding the binding is what says it
   * is this one's, and letting go is what gives it back.
   */
  virtual UsablePlanes GetUsablePlanes() const = 0;

  /* Where blanks come from.
   *
   * Upstream's display builds its own reader over the DRM vertical-blank
   * ioctl, which every KMS driver has. There is nothing to assume here --
   * how a controller reports a blank is its own business -- so the display
   * is handed one rather than making one.
   */
  virtual hwc::VSyncSource &GetVSyncSource() = 0;

  /* Not owned, and outliving the pipeline. */
  Device *device{};
  FbImporter *importer{};

  /* The chain. Bound rather than pointed at, so that no two displays can
   * claim the same piece of hardware. */
  std::shared_ptr<BindingOwner<Connector>> connector;
  std::shared_ptr<BindingOwner<Connector>> writeback_connector;
  std::shared_ptr<BindingOwner<Encoder>> encoder;
  std::shared_ptr<BindingOwner<Crtc>> crtc;
  std::shared_ptr<BindingOwner<Plane>> primary_plane;

  /* Owned by the pipeline, and not outliving it. Public and named as
   * upstream names them, because upstream reaches for them by name. */
  std::unique_ptr<AtomicStateManager> atomic_state_manager;
  std::unique_ptr<CompositionPlanner> planner;
  std::unique_ptr<BackendDisplayCapabilities> capabilities;
};

}  // namespace drm_hwcomposer
}  // namespace android
