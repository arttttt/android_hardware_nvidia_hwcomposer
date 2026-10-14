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

#include "governor/EngineClock.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>

#include <android/log.h>

#include "governor/HwcGovernor.h"
#include "governor/Log.h"
#include "governor/SysNode.h"
#include KERNEL_NVHOST_IOCTL_H

namespace android::hwc::governor::tegra {

namespace {

constexpr const char *devicePath = "/dev/nvhost-vic";
constexpr const char *devfreqPath = "/sys/class/devfreq/vic03.0/cur_freq";

}  // namespace

EngineClock::~EngineClock() {
  if (fd_ >= 0)
    close(fd_);
  if (devfreq_fd_ >= 0)
    close(devfreq_fd_);
}

bool EngineClock::Open() {
  fd_ = open(devicePath, O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    LogErrno(host_, ANDROID_LOG_ERROR, devicePath, errno);
    return false;
  }
  /* Without it the floor is compared with itself alone, which is never
   * wrong: the kernel takes the larger of a floor and devfreq's clock. */
  devfreq_fd_ = open(devfreqPath, O_RDONLY | O_CLOEXEC);
  if (devfreq_fd_ < 0)
    LogErrno(host_, ANDROID_LOG_WARN, devfreqPath, errno);
  return true;
}

std::optional<uint32_t> EngineClock::CurrentMhz() {
  const std::optional<uint64_t> hz = ReadNumber(devfreq_fd_);
  if (!hz)
    return std::nullopt;
  return static_cast<uint32_t>(*hz / 1000000U);
}

bool EngineClock::Request(uint32_t rate_hz, const char *what) {
  if (!usable())
    return false;

  /* Rate in hertz, module nought for the engine's own clock; nought as
   * the rate lifts the request. */
  nvhost_clk_rate_args args = {rate_hz, 0};
  if (ioctl(fd_, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &args) == 0)
    return true;

  const int err = errno;
  if (err == EPERM || err == EACCES || err == ENODEV) {
    refused_ = true;
    Logf(host_, ANDROID_LOG_ERROR, "%s refused (%s); a witness from here on",
         what, strerror(err));
  } else {
    LogErrno(host_, ANDROID_LOG_WARN, what, err);
  }
  return false;
}

bool EngineClock::SetFloor(uint32_t mhz) {
  if (!Request(mhz * 1000000U, "engine clock request"))
    return false;
  floor_mhz_ = mhz;
  host_.TraceInt("hwc_vic_floor", static_cast<int32_t>(mhz));
  return true;
}

void EngineClock::Release() {
  if (floor_mhz_ == 0)
    return;
  floor_mhz_ = 0;
  Request(0, "engine clock release");
  host_.TraceInt("hwc_vic_floor", 0);
}

}  // namespace android::hwc::governor::tegra
