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

#include "governor/CpuFloor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <android/log.h>

#include "governor/HwcGovernor.h"
#include "governor/Log.h"

namespace android::hwc::governor::tegra {

namespace {

constexpr const char *nodePath = "/dev/cpu_freq_min";
constexpr const char *currentPath =
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq";

}  // namespace

CpuFloor::~CpuFloor() {
  if (fd_ >= 0)
    close(fd_);
  if (cur_fd_ >= 0)
    close(cur_fd_);
}

bool CpuFloor::Open() {
  fd_ = open(nodePath, O_WRONLY | O_CLOEXEC);
  if (fd_ < 0) {
    LogErrno(host_, ANDROID_LOG_WARN, nodePath, errno);
    host_.Log(ANDROID_LOG_WARN, "the processor will not be lifted");
    return false;
  }
  /* Kept open and re-read from the start: a sysfs attribute is
   * regenerated on every read from offset nought. */
  cur_fd_ = open(currentPath, O_RDONLY | O_CLOEXEC);
  if (cur_fd_ < 0)
    LogErrno(host_, ANDROID_LOG_WARN, currentPath, errno);
  return true;
}

uint32_t CpuFloor::CurrentKhz() {
  if (cur_fd_ < 0)
    return 0;
  char text[32];
  const ssize_t n = pread(cur_fd_, text, sizeof(text) - 1, 0);
  if (n <= 0)
    return 0;
  text[n] = '\0';
  return static_cast<uint32_t>(strtoul(text, nullptr, 10));
}

bool CpuFloor::Write(uint32_t khz) {
  if (fd_ < 0)
    return false;

  /* Exactly four bytes: the node takes those as a binary word, and
   * anything else as hexadecimal text -- which is not what a rate written
   * in decimal means. The word is what is meant, with nothing to parse. */
  const int32_t word = static_cast<int32_t>(khz);
  if (write(fd_, &word, sizeof(word)) == static_cast<ssize_t>(sizeof(word)))
    return true;

  LogErrno(host_, ANDROID_LOG_WARN, nodePath, errno);
  host_.Log(ANDROID_LOG_WARN, "giving the processor floor up");
  close(fd_);
  fd_ = -1;
  lifted_ = false;
  return false;
}

bool CpuFloor::Lift(uint32_t khz) {
  if (!Write(khz))
    return false;
  lifted_ = true;
  host_.TraceInt("hwc_cpu_floor_khz", static_cast<int32_t>(khz));
  return true;
}

void CpuFloor::Drop() {
  if (!lifted_)
    return;
  lifted_ = false;
  Write(0);
  host_.TraceInt("hwc_cpu_floor_khz", 0);
}

}  // namespace android::hwc::governor::tegra
