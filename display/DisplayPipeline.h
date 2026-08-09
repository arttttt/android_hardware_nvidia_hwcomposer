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
 * Upstream splits a display in two. A pipeline is the chain of DRM objects a
 * frame travels down -- connector, encoder, controller, planes -- and the
 * things built on that chain: a state manager, a planner, a statement of what
 * the backend can do. A connector, separately, answers what is on the far end
 * of the cable: which timings it runs, how large it is, whether it is built
 * into the machine or plugged into it.
 *
 * There is no cable here and nothing to enumerate, so both roles land on one
 * object. What was a chain of DRM objects becomes whatever a backend needs to
 * drive its display, which is why this is something to implement rather than
 * something to fill in.
 *
 * The fields upstream reads directly are kept as fields with their names, and
 * what it reached for through the connector is kept as the connector's own
 * method names. Their code above this line therefore reads the same as it
 * does upstream, which is the point.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* Complete types rather than declarations, because the pipeline owns all
 * three and its destructor is here: taking apart a unique pointer needs to
 * know what it is pointing at. Upstream puts its destructor in a source file
 * and so gets away with declarations; this one has no source file. */
#include "backend/BackendDisplayCapabilities.h"
#include "compositor/CompositionPlanner.h"
#include "display/AtomicStateManager.h"
#include "display/DrmMode.h"
#include "display/PipelineBinding.h"

namespace android {

namespace hwc {
class VSyncSource;
}  // namespace hwc

namespace drm_hwcomposer {

struct DisplayPipeline {
  virtual ~DisplayPipeline() = default;

  /* The planes this display may put layers on, and the one meant for a
   * cursor if it has one.
   *
   * Handed out as bindings rather than plainly, so that a plane cannot end up
   * claimed by two displays at once: holding the binding is what says it is
   * this one's, and letting go is what gives it back.
   */
  virtual UsablePlanes GetUsablePlanes() const = 0;

  /* What upstream asks the connector. */

  virtual std::string GetName() const = 0;

  /* Timings the panel can run. The one marked preferred is what the display
   * comes up in. Fixed-mode panels report exactly one. */
  virtual const std::vector<DrmMode> &GetModes() const = 0;

  /* Physical size of the visible area. Zero where the panel does not say,
   * which consumers must read as "no information" rather than as a display of
   * no size. */
  virtual uint32_t GetMmWidth() const = 0;
  virtual uint32_t GetMmHeight() const = 0;

  /* Whether this display is something the user plugged in. It decides which
   * of the two high-dynamic-range settings applies, and how the framework is
   * told about the display appearing and going away. */
  virtual bool IsExternal() const = 0;

  /* Where blanks come from.
   *
   * Upstream's display builds its own reader over the DRM vertical-blank
   * ioctl, which every KMS driver has. There is no such thing to assume here
   * -- how a controller reports a blank is its own business -- so the display
   * is handed one rather than making one.
   */
  virtual hwc::VSyncSource &GetVSyncSource() = 0;

  /* Owned by the pipeline, and not outliving it.
   *
   * Public and named as upstream names them, because upstream reaches for
   * them by name: a display asks its pipeline's planner for a plan and its
   * state manager to carry the plan out.
   */
  std::unique_ptr<AtomicStateManager> atomic_state_manager;
  std::unique_ptr<CompositionPlanner> planner;
  std::unique_ptr<BackendDisplayCapabilities> capabilities;
};

}  // namespace drm_hwcomposer
}  // namespace android
