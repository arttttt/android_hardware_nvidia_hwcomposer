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

#include <poll.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include "governor/FenceWatch.h"
#include "governor/Tuning.h"

namespace android::hwc::governor {

class GovernorHost;

namespace tegra {

/* An engine's clock floor as the kernel takes it: one per engine. */
class FloorActuator {
 public:
  virtual ~FloorActuator() = default;

  virtual bool usable() const = 0;
  /* The clock the engine runs at, megahertz; empty if unread. */
  virtual std::optional<uint32_t> CurrentMhz() = 0;
  virtual bool SetFloor(uint32_t mhz) = 0;
  virtual void Release() = 0;
  virtual uint32_t floor_mhz() const = 0;
};

/* One engine's floor and how long it stands: the same for every engine.
 *
 * A floor asked for work still to come stands until the work is reported,
 * or the orphan timer runs out; asked for work in flight, it follows the
 * work's fences and outlives the last of them by the hold. A step owed --
 * withheld until the report -- is asked when the report comes. What step
 * to ask is the engine's own business; this is only how it is held. */
class FloorLane {
 public:
  /* `owed_trace` names the counter the owed step is traced under. */
  FloorLane(FloorActuator &actuator, const Tuning &tuning, GovernorHost &host,
            const char *owed_trace)
      : actuator_(actuator), tuning_(tuning), host_(host),
        owed_trace_(owed_trace) {}

  FloorLane(const FloorLane &) = delete;
  FloorLane &operator=(const FloorLane &) = delete;

  bool usable() const { return actuator_.usable(); }
  uint32_t floor_mhz() const { return actuator_.floor_mhz(); }

  /* Whether the work of frame `seq` has been reported already. */
  bool HasReported(uint64_t seq) const { return fences_.latest_seq() >= seq; }

  /* The earliest moment something here falls due, nought if nothing. */
  int64_t NextDueNs() const;

  /* This lane's fences join the poll set at its end. */
  void AppendPollSet(std::vector<struct pollfd> *fds);

  /* Work reported: the orphan timer is off and an owed step is asked.
   * Called before Follow, in the round the reports came. */
  void Reported();

  /* Judges the fences in the poll set, then takes `submitted` on; once
   * the last fence is done, the hold starts -- unless work still to come
   * keeps the floor. */
  void Follow(const std::vector<struct pollfd> &fds,
              const std::vector<Watched> &submitted, int64_t now);

  /* Asks `step` -- only upward from the floor standing and the clock the
   * engine already runs at -- for the work of a frame, still to come or
   * already `reported`; `owed` is asked once it is reported, nought if
   * nothing is owed. */
  void Ask(uint32_t step, uint32_t owed, bool reported, int64_t now);

  /* Lets the floor go once it has been held long enough or orphaned. */
  void ReleaseIfDue(int64_t now);

  /* Fences closed, timers off, floor gone. */
  void DropAll();

 private:
  FloorActuator &actuator_;
  const Tuning &tuning_;
  GovernorHost &host_;
  const char *owed_trace_;

  FenceWatch fences_;
  size_t first_slot_ = 0;
  int64_t release_due_ns_ = 0; /* when the floor may go, nought if not yet */
  int64_t orphan_due_ns_ = 0;  /* floor raised, no work reported by then */
  /* The step owed once the work is reported; nought if none is. */
  uint32_t owed_mhz_ = 0;
};

}  // namespace tegra
}  // namespace android::hwc::governor
