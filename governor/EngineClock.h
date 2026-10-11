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

namespace android::hwc::governor {

class GovernorHost;

namespace tegra {

/* The engine's clock, asked for through the composer's own descriptor on
 * /dev/nvhost-vic.
 *
 * Opened once for the life of the library: the per-descriptor clock
 * request is the one kernel entry point that also pulls the memory clock
 * along, and it lives only as long as the descriptor does. An empty
 * descriptor holds neither power nor a channel. A kernel that refuses the
 * request once is not asked again -- the clock then reads as unusable and
 * the policy runs as a witness.
 */
class EngineClock {
 public:
  explicit EngineClock(GovernorHost &host) : host_(host) {}
  ~EngineClock();

  EngineClock(const EngineClock &) = delete;
  EngineClock &operator=(const EngineClock &) = delete;

  /* False, having said why, if the device would not open. */
  bool Open();

  bool usable() const { return fd_ >= 0 && !refused_; }

  /* The clock devfreq has the engine at, megahertz, read from its sysfs
   * node. Not the engine's own clock request: the kernel switches the
   * engine on to answer that one, so on a powered-down engine asking was
   * the whole power-up, on this thread, ahead of the floor and the
   * processor lift. Empty if it could not be read. */
  std::optional<uint32_t> CurrentMhz();

  /* Files a floor of `mhz` on this descriptor; the kernel takes the larger
   * of it and what devfreq asks, and the memory clock follows. */
  bool SetFloor(uint32_t mhz);

  /* Lifts the floor. One request per series is enough: the descriptor
   * falls back to devfreq. */
  void Release();

  uint32_t floor_mhz() const { return floor_mhz_; }

 private:
  bool Request(uint32_t rate_hz, const char *what);

  GovernorHost &host_;
  int fd_ = -1;
  int devfreq_fd_ = -1;
  bool refused_ = false;
  uint32_t floor_mhz_ = 0;
};

}  // namespace tegra
}  // namespace android::hwc::governor
