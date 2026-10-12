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

#include "governor/FloorLane.h"

#include "governor/Clock.h"
#include "governor/HwcGovernor.h"
#include "governor/MergePlan.h"

namespace android::hwc::governor::tegra {

int64_t FloorLane::NextDueNs() const {
  int64_t due = 0;
  auto consider = [&due](int64_t when) {
    if (when > 0 && (due == 0 || when < due))
      due = when;
  };
  consider(release_due_ns_);
  consider(orphan_due_ns_);
  consider(fences_.NextGiveUpNs(int64_t(tuning_.fence_patience_ms) * nsPerMs));
  return due;
}

void FloorLane::AppendPollSet(std::vector<struct pollfd> *fds) {
  first_slot_ = fds->size();
  fences_.AppendPollSet(fds);
}

void FloorLane::Reported() {
  orphan_due_ns_ = 0;

  const uint32_t step = owed_mhz_;
  owed_mhz_ = 0;
  if (step == 0 || !actuator_.usable() || step <= actuator_.floor_mhz())
    return;
  /* The work is running at the step asked before; the ramp this starts
   * lands partway through it, or before the next. */
  actuator_.SetFloor(step);
  host_.TraceInt(owed_trace_, 0);
}

void FloorLane::Follow(const std::vector<struct pollfd> &fds,
                       const std::vector<Watched> &submitted, int64_t now) {
  /* Judged against the slots built before the sleep; only then are the
   * fences reported this round added, after those slots. */
  const bool any_done = fences_.Judge(
      fds, first_slot_, now, int64_t(tuning_.fence_patience_ms) * nsPerMs);
  for (const Watched &w : submitted)
    fences_.Add(w.fd, w.seq, now);

  /* The floor outlives the last fence by the hold -- unless a frame has
   * been planned whose work has not reported yet: that work is what the
   * floor stands for now, and the orphan timer covers it instead. */
  if (any_done && fences_.empty() && actuator_.floor_mhz() != 0 &&
      orphan_due_ns_ == 0)
    release_due_ns_ = now + int64_t(tuning_.hold_ms) * nsPerMs;
}

void FloorLane::Ask(uint32_t step, uint32_t owed, bool reported,
                    int64_t now) {
  owed_mhz_ = owed;
  host_.TraceInt(owed_trace_, int32_t(owed));

  /* Only upward from what the engine is already doing, by a floor still
   * standing from the previous work or by devfreq's doing -- devfreq
   * asked only when the floor alone would not answer. A standing floor is
   * kept standing: this work's fence will extend it. */
  const bool raised = step > actuator_.floor_mhz() &&
                      step > actuator_.CurrentMhz().value_or(0) &&
                      actuator_.SetFloor(step);
  if (!raised && actuator_.floor_mhz() == 0)
    return;

  /* A floor for work still to come waits for its report; one for work
   * already in flight follows its fence -- and if the fence is already
   * gone, the hold starts now. */
  if (!reported) {
    release_due_ns_ = 0;
    orphan_due_ns_ = now + int64_t(tuning_.orphan_ms) * nsPerMs;
  } else {
    orphan_due_ns_ = 0;
    release_due_ns_ =
        fences_.empty() ? now + int64_t(tuning_.hold_ms) * nsPerMs : 0;
  }
}

void FloorLane::ReleaseIfDue(int64_t now) {
  if (actuator_.floor_mhz() == 0 || !fences_.empty())
    return;
  const bool held_long_enough = release_due_ns_ != 0 && now >= release_due_ns_;
  const bool orphaned = orphan_due_ns_ != 0 && now >= orphan_due_ns_;
  if (held_long_enough || orphaned) {
    release_due_ns_ = 0;
    orphan_due_ns_ = 0;
    owed_mhz_ = 0;
    actuator_.Release();
  }
}

void FloorLane::DropAll() {
  fences_.DropAll();
  release_due_ns_ = 0;
  orphan_due_ns_ = 0;
  owed_mhz_ = 0;
  actuator_.Release();
}

}  // namespace android::hwc::governor::tegra
