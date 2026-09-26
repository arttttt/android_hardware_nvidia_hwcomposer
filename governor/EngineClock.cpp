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
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <android/log.h>

#include "governor/HwcGovernor.h"

namespace android::hwc::governor::tegra {

namespace {

constexpr const char *devicePath = "/dev/nvhost-vic";

/* nvhost's per-descriptor clock request, spelled out rather than taken from
 * the kernel's header: that header is GPL and is not exported. Rate in
 * hertz, module nought for the engine's own clock. Nought as the rate lifts
 * the request. Unsigned int, which is what the C library's request
 * argument is. */
struct ClockRateArgs {
  uint32_t rate;
  uint32_t moduleid;
};
constexpr unsigned int ioctlGetClockRate = _IOWR('H', 9, ClockRateArgs);
constexpr unsigned int ioctlSetClockRate = _IOW('H', 10, ClockRateArgs);

}  // namespace

EngineClock::~EngineClock() {
  if (fd_ >= 0)
    close(fd_);
}

bool EngineClock::Open() {
  fd_ = open(devicePath, O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: %s", devicePath, strerror(errno));
    host_.Log(ANDROID_LOG_ERROR, msg);
    return false;
  }
  return true;
}

std::optional<uint32_t> EngineClock::CurrentMhz() {
  if (!usable())
    return std::nullopt;
  ClockRateArgs args = {0, 0};
  if (ioctl(fd_, ioctlGetClockRate, &args) < 0)
    return std::nullopt;
  return args.rate / 1000000U;
}

bool EngineClock::Request(uint32_t rate_hz, const char *what) {
  if (!usable())
    return false;

  ClockRateArgs args = {rate_hz, 0};
  if (ioctl(fd_, ioctlSetClockRate, &args) == 0)
    return true;

  const int err = errno;
  char msg[160];
  if (err == EPERM || err == EACCES || err == ENODEV) {
    refused_ = true;
    snprintf(msg, sizeof(msg), "%s refused (%s); a witness from here on",
             what, strerror(err));
    host_.Log(ANDROID_LOG_ERROR, msg);
  } else {
    snprintf(msg, sizeof(msg), "%s: %s", what, strerror(err));
    host_.Log(ANDROID_LOG_WARN, msg);
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
