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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <android/log.h>

#include "governor/HwcGovernor.h"

namespace android::hwc::governor::tegra {

namespace {

constexpr const char *nodePath = "/dev/cpu_freq_min";

}  // namespace

CpuFloor::~CpuFloor() {
  if (fd_ >= 0)
    close(fd_);
}

bool CpuFloor::Open() {
  fd_ = open(nodePath, O_WRONLY | O_CLOEXEC);
  if (fd_ < 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: %s; the processor will not be lifted",
             nodePath, strerror(errno));
    host_.Log(ANDROID_LOG_WARN, msg);
    return false;
  }
  return true;
}

bool CpuFloor::Write(uint32_t khz) {
  if (fd_ < 0)
    return false;

  /* Text, with a line break: the node reads exactly four bytes as a binary
   * word, and anything else as decimal. */
  char text[24];
  const int n = snprintf(text, sizeof(text), "%u\n", khz);
  if (write(fd_, text, static_cast<size_t>(n)) >= 0)
    return true;

  char msg[128];
  snprintf(msg, sizeof(msg), "%s: %s; giving the node up", nodePath,
           strerror(errno));
  host_.Log(ANDROID_LOG_WARN, msg);
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
