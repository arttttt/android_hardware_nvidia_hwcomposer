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

/* Adapted from drm-hwcomposer's drm/DrmDevice.h.
 *
 * The piece of hardware the displays hang off. Upstream's is a card under
 * /dev/dri and most of its file is about finding out what that card has;
 * three questions survive the trip, because they are the only three anything
 * above the pipeline ever asks.
 */

#pragma once

#include <cstdint>
#include <optional>

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class Backend;
struct LayerData;

class Device {
 public:
  virtual ~Device() = default;

  /* What builds pipelines for this hardware, and answers what it can do. */
  virtual Backend &GetBackend() const = 0;

  /* Where a frame for every display on this hardware goes at once. */
  virtual AtomicCommitSink &GetAtomicCommitSink() = 0;

  /* A buffer to show while a timing is being changed.
   *
   * Changing a timing needs something to scan out, and what was on screen
   * was drawn for the timing that is going away. Upstream allocates a plain
   * black buffer for the crossing; whether one is needed at all is a
   * property of the hardware, so this may answer with nothing.
   */
  virtual std::optional<LayerData> CreateBufferForModeset(uint32_t width,
                                                          uint32_t height) = 0;
};

}  // namespace android::drm_hwcomposer
