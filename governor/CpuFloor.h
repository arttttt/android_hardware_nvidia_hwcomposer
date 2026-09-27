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

#include <stdint.h>

namespace android::hwc::governor {

class GovernorHost;

namespace tegra {

/* The processor's clock floor, held through /dev/cpu_freq_min.
 *
 * The node is a pm_qos request that lives while the descriptor is open:
 * a write of a rate in kilohertz raises the floor, a write of nought lets
 * it go, and the kernel's cpufreq clamps the floor to whatever ceiling the
 * performance profile has set. Optional: a device whose policy denies the
 * composer the node simply never lifts the processor.
 */
class CpuFloor {
 public:
  explicit CpuFloor(GovernorHost &host) : host_(host) {}
  ~CpuFloor();

  CpuFloor(const CpuFloor &) = delete;
  CpuFloor &operator=(const CpuFloor &) = delete;

  /* False, having said why, if the node would not open. */
  bool Open();

  bool available() const { return fd_ >= 0; }
  bool lifted() const { return lifted_; }

  bool Lift(uint32_t khz);
  void Drop();

 private:
  bool Write(uint32_t khz);

  GovernorHost &host_;
  int fd_ = -1;
  bool lifted_ = false;
};

}  // namespace tegra
}  // namespace android::hwc::governor
