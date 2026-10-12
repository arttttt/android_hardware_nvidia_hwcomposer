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

#include "governor/GpuClock.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <android/log.h>

#include "governor/HwcGovernor.h"
#include "governor/Log.h"
#include "governor/SysNode.h"

namespace android::hwc::governor::tegra {

namespace {

constexpr const char *nodePath = "/dev/gpu_freq_min";
constexpr const char *currentPath = "/sys/class/devfreq/gk20a.0/cur_freq";

}  // namespace

GpuClock::~GpuClock() {
  if (fd_ >= 0)
    close(fd_);
  if (cur_fd_ >= 0)
    close(cur_fd_);
}

bool GpuClock::Open() {
  fd_ = open(nodePath, O_WRONLY | O_CLOEXEC);
  if (fd_ < 0) {
    LogErrno(host_, ANDROID_LOG_WARN, nodePath, errno);
    host_.Log(ANDROID_LOG_WARN, "the GPU will not be given a floor");
    return false;
  }
  /* Without it the floor is compared with itself alone, which is never
   * wrong: the kernel takes the larger of a floor and podgov's clock. */
  cur_fd_ = open(currentPath, O_RDONLY | O_CLOEXEC);
  if (cur_fd_ < 0)
    LogErrno(host_, ANDROID_LOG_WARN, currentPath, errno);
  return true;
}

std::optional<uint32_t> GpuClock::CurrentMhz() {
  const std::optional<uint64_t> hz = ReadNumber(cur_fd_);
  if (!hz)
    return std::nullopt;
  return static_cast<uint32_t>(*hz / 1000000U);
}

bool GpuClock::Write(uint32_t khz) {
  if (fd_ < 0)
    return false;
  if (WriteQosWord(fd_, khz))
    return true;

  LogErrno(host_, ANDROID_LOG_WARN, nodePath, errno);
  host_.Log(ANDROID_LOG_WARN, "giving the GPU floor up");
  close(fd_);
  fd_ = -1;
  floor_mhz_ = 0;
  return false;
}

bool GpuClock::SetFloor(uint32_t mhz) {
  if (!Write(mhz * 1000U))
    return false;
  floor_mhz_ = mhz;
  host_.TraceInt("hwc_gpu_floor", static_cast<int32_t>(mhz));
  return true;
}

void GpuClock::Release() {
  if (floor_mhz_ == 0)
    return;
  floor_mhz_ = 0;
  Write(0);
  host_.TraceInt("hwc_gpu_floor", 0);
}

}  // namespace android::hwc::governor::tegra
