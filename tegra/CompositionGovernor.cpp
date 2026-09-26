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

#include "tegra/CompositionGovernor.h"

#include <unistd.h>

#include <cutils/trace.h>
#include <log/log.h>

#include "utils/log.h"

namespace android {
namespace hwc {

std::unique_ptr<CompositionGovernor> CompositionGovernor::Load(
    VicSession *vic) {
  if (vic == nullptr)
    return nullptr;

  std::unique_ptr<GovernorLibrary> library = GovernorLibrary::Open();
  if (!library)
    return nullptr;

  auto governor = std::unique_ptr<CompositionGovernor>(
      new CompositionGovernor(std::move(library), vic));
  if (!governor->Start())
    return nullptr;

  ALOGI("composition governor: loaded");
  return governor;
}

CompositionGovernor::CompositionGovernor(
    std::unique_ptr<GovernorLibrary> library, VicSession *vic)
    : library_(std::move(library)), vic_(vic) {
}

bool CompositionGovernor::Start() {
  /* Before the library exists, so a warm-up it asks for on its first
   * breath finds the buffers ready; on this thread, so the zone's device
   * is opened by one thread only. */
  warmer_ = EngineWarmer::Create(vic_);

  governor::Governor *created = library_->Create(this);
  if (created == nullptr)
    return false;

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
  if (governor != nullptr)
    library_->Destroy(governor);
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
  return warmer_ ? warmer_->Run() : -1;
}

void CompositionGovernor::TraceInt(const char *name, int32_t value) {
  ATRACE_INT(name, value);
}

void CompositionGovernor::Log(int prio, const char *msg) {
  __android_log_write(prio, "hwc-governor", msg);
}

}  // namespace hwc
}  // namespace android
