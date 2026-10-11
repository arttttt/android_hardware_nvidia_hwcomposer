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

#include <stddef.h>
#include <stdint.h>

#include <cutils/native_handle.h>

/* The composer's own GPU executor, as a library of its own.
 *
 * What the display windows and VIC cannot take is drawn here, on the GPU,
 * by the composer -- so SurfaceFlinger is never asked to compose a frame.
 * Kept apart like the governor: Vulkan, its loader and the driver's quirks
 * stay out of the composer, which loads this by name from its own directory
 * and runs without it if it is absent or refuses.
 *
 * Zero copy both ways: every source is a client's gralloc buffer imported as
 * it is, and the target is a gralloc buffer the display then scans. The
 * library never waits on the CPU: the sources' acquire fences and the
 * target's ready fence are waited on by the GPU, and Compose returns the
 * fence the drawing will signal.
 */

namespace android::hwc::vk {

/* Bumped whenever anything below changes shape. The composer refuses a
 * library that answers another number. */
constexpr uint32_t apiVersion = 1;

/* One layer to draw. Rectangles are in pixels; `dst` is in the target's
 * own space, whose origin is wherever the composer puts the target. */
struct Layer {
  /* The client's buffer, borrowed for the call; null for a solid colour,
   * which is drawn from `color` alone. */
  const native_handle_t *buffer = nullptr;
  uint32_t width = 0;  /* the buffer's own size */
  uint32_t height = 0;
  uint32_t stride = 0; /* in pixels */
  int32_t format = 0;  /* HAL_PIXEL_FORMAT_* */
  uint64_t usage = 0;

  /* Borrowed; the library takes its own copy if it needs one. -1 for a
   * buffer that is ready. */
  int acquire_fence = -1;

  float src_left = 0, src_top = 0, src_right = 0, src_bottom = 0;
  int32_t dst_left = 0, dst_top = 0, dst_right = 0, dst_bottom = 0;

  /* hflip | vflip << 1 | rotate90 << 2, as HWC2 means them: the flips are
   * applied first, then the quarter turn clockwise. */
  uint8_t transform = 0;

  float alpha = 1.f;
  bool premultiplied = true;
  /* RGBX and the like: the buffer's alpha is not alpha. */
  bool opaque = false;

  /* For a solid colour: straight (not premultiplied) RGBA. */
  float color[4] = {0, 0, 0, 0};

  /* Rounded corners, in the target's space; radius 0 for none. */
  float corner_left = 0, corner_top = 0, corner_right = 0, corner_bottom = 0;
  float corner_radius = 0;
};

/* Where the layers are drawn. Cleared to transparent first. */
struct Target {
  const native_handle_t *buffer = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  int32_t format = 0;
  uint64_t usage = 0;

  /* Borrowed: comes due when the target may be written -- when the display
   * stopped scanning it. -1 for a target that is free now. */
  int ready_fence = -1;
};

class Host {
 public:
  virtual ~Host() = default;

  /* A line for the log, at an android_LogPriority. */
  virtual void Log(int prio, const char *msg) = 0;
};

class Compositor {
 public:
  virtual ~Compositor() = default;

  /* Draws `layers`, bottom first, into the target. Returns the fence the
   * drawing signals, owned by the caller -- or -1 when the library refused
   * the work (a format it cannot read, an import the driver turned down),
   * having drawn nothing, so the caller can take another way. Never waits
   * on the CPU for the GPU. */
  virtual int Compose(const Target &target, const Layer *layers, size_t count) = 0;
};

}  // namespace android::hwc::vk

extern "C" {
uint32_t hwc_vk_api_version();
/* Null if Vulkan does not come up on this device, having said why. */
android::hwc::vk::Compositor *hwc_vk_create(android::hwc::vk::Host *host);
/* Waits for whatever is still drawing, then releases everything. */
void hwc_vk_destroy(android::hwc::vk::Compositor *compositor);
}
