/*
 * Copyright (C) 2025 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's backend/Backend.h.
 *
 * What one kind of display hardware is, as far as everything above it is
 * concerned: something that can build a pipeline for a display, say how a
 * buffer is to be read, say where a batch of frames goes, and answer what
 * sleep states it has.
 *
 * Changed only in what the words point at: a device and a connector as this
 * composer describes them rather than as DRM does. Their constructor is
 * inline here because a two-line source file to hold it is not worth the
 * file.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class BufferInfoGetter;
class Connector;
class Device;

struct DisplayPipeline;

class Backend {
 public:
  explicit Backend(Device &device) : device_(&device) {
  }

  virtual ~Backend() = default;

  // Create a DisplayPipeline for the given Connector, including creating the
  // CompositionPlanner for this DisplayPipeline.
  virtual std::unique_ptr<DisplayPipeline> CreatePipeline(
      Connector &connector) = 0;

  // Get the BufferInfoGetter for the Backend.
  virtual std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter() = 0;

  virtual bool SupportsDoze() const {
    return false;
  }

  virtual bool SupportsDozeSuspend() const {
    return false;
  }

  virtual bool SupportsSuspend() const {
    return false;
  }

  // Get the AtomicCommitSink for the Backend.
  virtual std::unique_ptr<AtomicCommitSink> CreateAtomicCommitSink() = 0;

  virtual std::optional<std::string> Dump() {
    return std::nullopt;
  }

 protected:
  Device &GetDevice() const {
    return *device_;
  }

 private:
  // The Device owns this Backend. Guaranteed not to be nullptr because it is
  // passed to the constructor by reference.
  Device *device_;
};

}  // namespace android::drm_hwcomposer
