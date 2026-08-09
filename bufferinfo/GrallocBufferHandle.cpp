/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "bufferinfo/GrallocBufferHandle.h"

#include <cutils/native_handle.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Errors.h>

#include <memory>

#include "utils/log.h"

namespace android::drm_hwcomposer {

auto GrallocBufferHandle::Create(buffer_handle_t handle)
    -> std::shared_ptr<GrallocBufferHandle> {
  /* Adapted: the handle is kept, not imported.
   *
   * Importing means asking the mapper for a reference of this process's own,
   * which matters where a handle arrives as bare numbers and has to be made
   * real. That is not this path. The composer is loaded into the service that
   * receives the handle over the interface, which materialises its
   * descriptors on the way in, and what reaches the display controller is one
   * of those descriptors rather than anything mapped here.
   *
   * The import this replaces cannot be spelled on this platform in any case:
   * the mapper here takes a full description of the buffer alongside the
   * handle -- dimensions, format, usage, stride -- none of which this function
   * is given, and all of which are known only to the allocator it came from.
   */
  return std::shared_ptr<GrallocBufferHandle>(new GrallocBufferHandle(handle));
}

GrallocBufferHandle::~GrallocBufferHandle() = default;

}  // namespace android::drm_hwcomposer