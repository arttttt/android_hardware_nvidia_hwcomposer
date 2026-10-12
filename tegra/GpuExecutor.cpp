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

#include "tegra/GpuExecutor.h"

#include <dlfcn.h>
#include <unistd.h>

#include <utility>

#include <android/log.h>
#include <hardware/gralloc.h>

#include "utils/log.h"

namespace android {
namespace hwc {

namespace {

/* Beside the composer, by full path, as the governor is found. */
constexpr const char *libraryName = "hwcvk.tegra.so";
constexpr const char *libraryPaths[] = {"/vendor/lib/hw/hwcvk.tegra.so", "hwcvk.tegra.so"};

/* A client target's usage, which the display already takes and the GPU
 * renders into; the format is the one the executor writes. */
constexpr int kTargetUsage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER;
constexpr int kTargetFormat = HAL_PIXEL_FORMAT_RGBA_8888;

/* The library's lines go to the composer's log under its own tag. */
class LogHost : public vk::Host {
 public:
  void Log(int prio, const char *msg) override {
    __android_log_print(prio, "hwc-vk", "%s", msg);
  }
};

}  // namespace

std::unique_ptr<GpuExecutor> GpuExecutor::Open(uint32_t width, uint32_t height) {
  void *handle = nullptr;
  for (const char *path : libraryPaths) {
    handle = dlopen(path, RTLD_NOW);
    if (handle != nullptr)
      break;
  }
  if (handle == nullptr) {
    ALOGI("gpu executor: no %s (%s), the client composes", libraryName, dlerror());
    return nullptr;
  }

  std::unique_ptr<GpuExecutor> e(new GpuExecutor());
  e->library_ = handle;
  e->width_ = width;
  e->height_ = height;

  auto version = reinterpret_cast<uint32_t (*)()>(dlsym(handle, "hwc_vk_api_version"));
  auto create =
      reinterpret_cast<vk::Compositor *(*)(vk::Host *)>(dlsym(handle, "hwc_vk_create"));
  e->destroy_ = reinterpret_cast<void (*)(vk::Compositor *)>(dlsym(handle, "hwc_vk_destroy"));
  if (version == nullptr || create == nullptr || e->destroy_ == nullptr) {
    ALOGE("gpu executor: %s lacks its entry points", libraryName);
    return nullptr;
  }
  if (version() != vk::apiVersion) {
    ALOGE("gpu executor: %s speaks API %u, this composer %u", libraryName, version(),
          vk::apiVersion);
    return nullptr;
  }

  e->host_ = std::make_unique<LogHost>();
  e->compositor_ = create(e->host_.get());
  if (e->compositor_ == nullptr) {
    ALOGE("gpu executor: %s declined to start", libraryName);
    return nullptr;
  }

  const hw_module_t *module = nullptr;
  if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) != 0 ||
      gralloc_open(module, &e->alloc_) != 0) {
    ALOGE("gpu executor: no gralloc to allocate its buffers from");
    return nullptr;
  }
  for (Slot &s : e->slots_) {
    if (e->alloc_->alloc(e->alloc_, static_cast<int>(width), static_cast<int>(height),
                         kTargetFormat, kTargetUsage, &s.buffer, &s.stride) != 0) {
      ALOGE("gpu executor: gralloc would not give its buffers");
      return nullptr;
    }
  }

  ALOGI("gpu executor: %s up, three %ux%u buffers", libraryName, width, height);
  return e;
}

GpuExecutor::~GpuExecutor() {
  /* The compositor first: it waits for whatever is still drawing into the
   * buffers before they are freed. */
  if (compositor_ != nullptr && destroy_ != nullptr)
    destroy_(compositor_);
  if (alloc_ != nullptr) {
    for (Slot &s : slots_)
      if (s.buffer != nullptr)
        alloc_->free(alloc_, s.buffer);
    gralloc_close(alloc_);
  }
  if (library_ != nullptr)
    dlclose(library_);
}

void GpuExecutor::DropBegun() {
  if (begun_at_) {
    const int dropped = compositor_->Finish();
    if (dropped >= 0)
      close(dropped);
    begun_at_.reset();
  }
}

bool GpuExecutor::Begin(std::vector<vk::Layer> &layers,
                        const drm_hwcomposer::IRect *dirty) {
  DropBegun();

  const size_t at = (at_ + 1) % slots_.size();
  const Slot &slot = slots_[at];

  vk::Target target;
  target.buffer = slot.buffer;
  target.width = width_;
  target.height = height_;
  target.stride = static_cast<uint32_t>(slot.stride);
  target.format = kTargetFormat;
  target.usage = kTargetUsage;
  target.ready_fence = slot.freed_when ? *slot.freed_when : -1;
  if (dirty != nullptr) {
    target.partial = true;
    target.dirty_left = dirty->left;
    target.dirty_top = dirty->top;
    target.dirty_right = dirty->right;
    target.dirty_bottom = dirty->bottom;
  }

  if (!compositor_->Begin(target, layers.data(), layers.size()))
    return false;
  begun_at_ = at;
  return true;
}

std::optional<GpuExecutor::Drawn> GpuExecutor::Finish() {
  if (!begun_at_)
    return std::nullopt;
  const size_t at = *begun_at_;
  begun_at_.reset();
  const int fd = compositor_->Finish();
  if (fd < 0)
    return std::nullopt;
  at_ = at;
  return Drawn{slots_[at].buffer, drm_hwcomposer::MakeSharedFd(fd)};
}

GpuExecutor::Drawn GpuExecutor::Keep(drm_hwcomposer::SharedFd fence) {
  DropBegun();
  at_ = (at_ + 1) % slots_.size();
  return Drawn{slots_[at_].buffer, std::move(fence)};
}

void GpuExecutor::Presented(const drm_hwcomposer::SharedFd &fence) {
  /* The frame this fence belongs to replaces what was on screen, so that
   * buffer is free when this fence comes due; this frame's own buffer is
   * answered by the frame after it. */
  if (anything_showing_)
    slots_[showing_].freed_when = fence;
  slots_[at_].freed_when = {};
  showing_ = at_;
  anything_showing_ = true;
}

bool GpuExecutor::Warm() {
  return compositor_->Warm();
}

}  // namespace hwc
}  // namespace android
