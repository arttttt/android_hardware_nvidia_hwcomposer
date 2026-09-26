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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "governor/CompositionGovernor.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cutils/trace.h>
#include <log/log.h>

#include "utils/fd.h"
#include "utils/log.h"

namespace android {
namespace hwc {

namespace {

/* Fixed by name: which library runs is not a per-device choice. Looked for
 * beside the composer first, by full path: a vendor process's linker
 * searches the vendor library directory itself, not its module
 * subdirectory, and the composer's own modules live in the latter. */
constexpr const char *libraryName = "hwcgovernor.tegra.so";
constexpr const char *libraryPaths[] = {"/vendor/lib/hw/hwcgovernor.tegra.so",
                                         "hwcgovernor.tegra.so"};

/* The warm-up pass's extent. The engine's coming up costs the same
 * whatever the job's size, so the job is as small as the zone's row grain
 * lets it be. */
constexpr uint32_t warmSide = 8;

}  // namespace

std::unique_ptr<CompositionGovernor> CompositionGovernor::Load(
    VicSession *vic) {
  if (vic == nullptr)
    return nullptr;

  void *library = nullptr;
  for (const char *path : libraryPaths) {
    library = dlopen(path, RTLD_NOW);
    if (library != nullptr)
      break;
  }
  if (library == nullptr) {
    /* The usual answer on a device that ships no policy, so said quietly. */
    ALOGI("composition governor: no %s (%s), running without", libraryName,
          dlerror());
    return nullptr;
  }

  auto governor =
      std::unique_ptr<CompositionGovernor>(new CompositionGovernor(library, vic));
  if (!governor->Start())
    return nullptr;

  ALOGI("composition governor: %s loaded", libraryName);
  return governor;
}

CompositionGovernor::CompositionGovernor(void *library, VicSession *vic)
    : library_(library), vic_(vic) {
}

bool CompositionGovernor::Start() {
  auto *version = reinterpret_cast<uint32_t (*)()>(
      dlsym(library_, "hwc_governor_api_version"));
  auto *create = reinterpret_cast<governor::Governor *(*)(governor::GovernorHost *)>(
      dlsym(library_, "hwc_governor_create"));
  destroy_ = reinterpret_cast<void (*)(governor::Governor *)>(
      dlsym(library_, "hwc_governor_destroy"));
  if (version == nullptr || create == nullptr || destroy_ == nullptr) {
    ALOGE("composition governor: %s lacks the entry points", libraryName);
    return false;
  }

  const uint32_t offered = version();
  if (offered != governor::apiVersion) {
    ALOGE("composition governor: %s speaks API %u, this composer %u",
          libraryName, offered, governor::apiVersion);
    return false;
  }

  /* Before the library exists, so a warm-up it asks for on its first
   * breath finds them ready. Not fatal without them: the governor can
   * still raise the clock, it just cannot wake the engine early. */
  warm_source_ = vic_->AllocateZoneTarget(warmSide, warmSide);
  warm_target_ = vic_->AllocateZoneTarget(warmSide, warmSide);
  if (!warm_source_ || !warm_target_) {
    ALOGW("composition governor: the zone would not give the warm-up "
          "buffers; the engine will not be warmed");
    warm_source_.reset();
    warm_target_.reset();
  }

  governor::Governor *created = create(this);
  if (created == nullptr) {
    ALOGE("composition governor: %s declined to start", libraryName);
    return false;
  }

  const std::lock_guard<std::mutex> lock(callback_mutex_);
  governor_ = created;
  return true;
}

CompositionGovernor::~CompositionGovernor() {
  governor::Governor *governor = nullptr;
  {
    /* Nothing is forwarded from here on: a frame event arriving on another
     * thread finds the seat empty and drops its descriptor itself. */
    const std::lock_guard<std::mutex> lock(callback_mutex_);
    governor = governor_;
    governor_ = nullptr;
  }

  /* Outside the lock, and it has to be: the library joins its thread here,
   * and that thread may be inside WarmEngine, which needs no lock of ours
   * but does need the engine -- which the pipeline keeps alive until this
   * destructor has returned. */
  if (governor != nullptr && destroy_ != nullptr)
    destroy_(governor);

  warm_target_.reset();
  warm_source_.reset();

  if (library_ != nullptr)
    dlclose(library_);
}

void CompositionGovernor::FramePlanned(const governor::Frame &frame) {
  const std::lock_guard<std::mutex> lock(callback_mutex_);
  if (governor_ != nullptr)
    governor_->FramePlanned(frame);
}

void CompositionGovernor::MergeSubmitted(uint64_t seq, int fence_fd) {
  const std::lock_guard<std::mutex> lock(callback_mutex_);
  if (governor_ != nullptr) {
    governor_->MergeSubmitted(seq, fence_fd);
    return;
  }
  if (fence_fd >= 0)
    close(fence_fd);
}

void CompositionGovernor::FramePresented(uint64_t seq) {
  const std::lock_guard<std::mutex> lock(callback_mutex_);
  if (governor_ != nullptr)
    governor_->FramePresented(seq);
}

void CompositionGovernor::PowerMode(governor::Power mode) {
  const std::lock_guard<std::mutex> lock(callback_mutex_);
  if (governor_ != nullptr)
    governor_->PowerMode(mode);
}

int CompositionGovernor::WarmEngine() {
  if (!warm_source_ || !warm_target_)
    return -1;

  /* A source of the composer's own: the engine reads it by the words the
   * library built when the buffer was born, which is what the `vendor`
   * seat is for. Verbatim, unturned, nothing to wait on. */
  VicSession::Layer layer = {};
  layer.handle = nullptr;
  layer.vendor = warm_source_.get();
  layer.source_right = static_cast<float>(warmSide);
  layer.source_bottom = static_cast<float>(warmSide);
  layer.display_right = static_cast<int32_t>(warmSide);
  layer.display_bottom = static_cast<int32_t>(warmSide);
  layer.premultiplied = true;
  layer.alpha = 1.0F;
  layer.acquire_fence = -1;

  const drm_hwcomposer::SharedFd done =
      vic_->CopyLayer(*warm_target_, layer, warmSide, warmSide);
  if (!done)
    return -1;

  return drm_hwcomposer::DupFd(done);
}

void CompositionGovernor::TraceInt(const char *name, int32_t value) {
  ATRACE_INT(name, value);
}

void CompositionGovernor::Log(int prio, const char *msg) {
  __android_log_write(prio, "hwc-governor", msg);
}

}  // namespace hwc
}  // namespace android
