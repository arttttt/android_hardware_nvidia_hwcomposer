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
#include <memory>
#include <optional>
#include <string>
#include <vector>

/* A complete type, not a declaration: an optional of it is returned by value
 * and so has to know how large it is and how to take it apart. */
#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class Backend;
class Connector;

class Device {
 public:
  virtual ~Device() = default;

  /* Which kind of hardware this is. Upstream reads the DRM driver's own
   * name; it is what decides which backend gets built, and what a property
   * override overrides. */
  virtual std::string GetName() const = 0;

  /* What builds pipelines for this hardware, and answers what it can do. */
  virtual Backend &GetBackend() const = 0;

  /* The displays this hardware has, in whatever order it found them.
   *
   * Upstream's resource manager watches the kernel and reports these as they
   * come and go. Here they are found once and do not change, but what the
   * composer does with them is the same: build a pipeline for each and hand
   * it to the frontend.
   *
   * Owned by the device and outliving every pipeline bound to them.
   */
  virtual const std::vector<std::unique_ptr<Connector>> &GetConnectors()
      const = 0;

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

/* Opens the display hardware of the machine this was built for.
 *
 * Declared here and defined by whichever backend is in the build -- one per
 * image, chosen when it is put together rather than when it runs. It is the
 * one place above the backends that has to name a particular kind of
 * hardware, and it names it by not naming it.
 *
 * Returns null if there is nothing to drive, which is not a state worth
 * carrying on from.
 */
std::unique_ptr<Device> CreateDevice();

}  // namespace android::drm_hwcomposer
