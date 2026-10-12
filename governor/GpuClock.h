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

#include <optional>

#include "governor/FloorLane.h"

namespace android::hwc::governor {

class GovernorHost;

namespace tegra {

/* The GPU's clock floor, held through /dev/gpu_freq_min, and its clock as
 * devfreq reports it.
 *
 * The node is a pm_qos request on the gbus floor that lives while the
 * descriptor is open: a rate in kilohertz raises the floor -- rounded up
 * to a gbus step, with the memory clock following -- and nought lets it
 * go. podgov keeps scaling above it. A node that would not open, or a
 * write that fails, leaves the GPU to podgov alone.
 */
class GpuClock final : public FloorActuator {
 public:
  explicit GpuClock(GovernorHost &host) : host_(host) {}
  ~GpuClock() override;

  GpuClock(const GpuClock &) = delete;
  GpuClock &operator=(const GpuClock &) = delete;

  /* False, having said why, if the node would not open. */
  bool Open();

  bool usable() const override { return fd_ >= 0; }
  std::optional<uint32_t> CurrentMhz() override;
  bool SetFloor(uint32_t mhz) override;
  void Release() override;
  uint32_t floor_mhz() const override { return floor_mhz_; }

 private:
  bool Write(uint32_t khz);

  GovernorHost &host_;
  int fd_ = -1;
  int cur_fd_ = -1;
  uint32_t floor_mhz_ = 0;
};

}  // namespace tegra
}  // namespace android::hwc::governor
