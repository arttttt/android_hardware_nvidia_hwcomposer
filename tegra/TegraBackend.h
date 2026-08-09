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

#include "backend/GenericBackend.h"

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class Connector;
class Device;

struct DisplayPipeline;

/* What this display controller is, as a factory.
 *
 * Everything specific to the hardware that a backend decides is decided
 * here: what a pipeline for one of its heads is made of, and where a batch
 * of frames is sent. The two decisions that are the same on any hardware --
 * how a buffer is read and which planner runs -- are inherited rather than
 * made again.
 */
class TegraBackend : public GenericBackend {
 public:
  explicit TegraBackend(Device &device) : GenericBackend(device) {
  }

  std::unique_ptr<DisplayPipeline> CreatePipeline(
      Connector &connector) override;

  std::unique_ptr<AtomicCommitSink> CreateAtomicCommitSink() override;
};

}  // namespace android::drm_hwcomposer
