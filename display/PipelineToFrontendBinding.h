/*
 * Copyright (C) 2023 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's drm/ResourceManager.h, where it is declared
 * beside the thing that calls it.
 *
 * How a display arriving or leaving reaches the composer. Upstream's resource
 * manager watches the kernel for cards and monitors appearing and calls these
 * as it finds them; the composer answers by making a display, or taking one
 * apart, and telling the framework afterwards.
 *
 * It is here rather than beside a resource manager because there is no
 * resource manager here -- what enumerates displays is whatever a backend
 * builds them from -- while what the composer does about it is the same
 * either way. Nothing on this board is ever plugged in or unplugged, so the
 * calls happen once at start-up and then never again, which is not a reason
 * for them to be shaped any differently.
 */

#pragma once

#include <memory>

namespace android::drm_hwcomposer {

struct DisplayPipeline;

class PipelineToFrontendBindingInterface {
 public:
  virtual ~PipelineToFrontendBindingInterface() = default;

  virtual bool BindDisplay(std::shared_ptr<DisplayPipeline>) = 0;
  virtual bool UnbindDisplay(std::shared_ptr<DisplayPipeline>) = 0;
  virtual void FinalizeDisplayBinding() = 0;

  virtual void NotifyDisplayLinkStatus(
      std::shared_ptr<DisplayPipeline> pipeline) = 0;
  virtual void NotifyHdcpTermination(
      std::shared_ptr<DisplayPipeline> pipeline) = 0;

  virtual void FlushHotplugEvents() = 0;
};

}  // namespace android::drm_hwcomposer
