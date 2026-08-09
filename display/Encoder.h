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

/* Adapted from drm-hwcomposer's drm/DrmEncoder.h.
 *
 * What turns what a display controller produces into the signalling a
 * particular kind of output needs. It is a link in the chain a frame travels
 * down, and a pipeline names it for that reason.
 *
 * Nothing above the pipeline asks it anything -- not once, in either the
 * display or the composer device. It is here so that a pipeline is the same
 * chain it is upstream, and so that code arriving later which does name it
 * finds it where it expects.
 */

#pragma once

#include <cstdint>

#include "display/PipelineBinding.h"

namespace android::drm_hwcomposer {

class Encoder : public PipelineBindable<Encoder> {
 public:
  virtual ~Encoder() = default;

  virtual uint32_t GetId() const = 0;
};

}  // namespace android::drm_hwcomposer
