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

#include <stddef.h>
#include <stdint.h>

#include "governor/HwcGovernor.h"
#include "governor/Tuning.h"

/* The model: from a planned frame to the clock step the merge needs.
 *
 * Pure functions of their arguments, with no device behind them, so the
 * arithmetic can be checked on a workstation (governor/tests) and the
 * numbers argued about without a tablet on the desk. */

namespace android::hwc::governor::tegra {

constexpr int64_t nsPerUs = 1000LL;
constexpr int64_t nsPerMs = 1000000LL;
constexpr int64_t nsPerSec = 1000000000LL;

/* Whether the engine has been idle long enough to have powered down: no
 * job from the composer and no warm-up from the governor within the
 * kernel's powergate delay. */
bool EngineCold(const Frame &frame, int64_t last_warm_ns, int64_t now_ns,
                const Tuning &tuning);

/* Whether the frame carries a phase worth extending: a last latch, a
 * period, and not so old that the panel may have been re-phased since. */
bool PhaseKnown(const Frame &frame, int64_t now_ns, const Tuning &tuning);

/* When the merge has to be done: the latch the frame is aiming at, less
 * the margin. The latches are the last one the display reported plus whole
 * periods. With the previous flip landed the frame targets the nearest;
 * otherwise it waits for that one to carry the previous frame and targets
 * the one after. Without a phase, the measured fallbacks. */
int64_t LatchDeadline(const Frame &frame, int64_t now_ns,
                      const Tuning &tuning);

/* The engine's work for the merge: the job's own cost, plus a clock per
 * `pixels_per_clock` pixels over the larger of what each member reads and
 * writes, with the margin. */
double MergeCycles(const Member *members, size_t count, const Tuning &tuning);

struct MergeEstimate {
  double cycles;
  int64_t deadline_ns; /* when the merge has to be done */
  int64_t budget_ns;   /* from the end of the submit to the deadline */
  uint32_t need_mhz;   /* cycles over budget, rounded up */
  uint32_t step_mhz;   /* the step to ask for */
  bool late;           /* the budget had to be clamped to its minimum */
  bool beyond;         /* the need is above the top step */
  bool slipped;        /* aimed at the latch after the nearest one */
};

/* `cold` says the engine will pay for coming up.
 *
 * A latch the merge cannot make at the top step even down the shortest
 * path is not the latch the frame will land on: with the phase known, the
 * estimate slips once, and says so -- to the following latch, or sooner,
 * to the moment the next frame's merge is expected at the engine, since
 * the engine's queue is one and a slow merge in it is that frame's loss.
 * A latch it might just make is asked the top step for -- missing by a
 * margin costs the same frame as missing by a mile, so doubt goes to the
 * higher step. */
MergeEstimate EstimateMerge(const Frame &frame, const Member *members,
                            size_t count, int64_t now_ns,
                            const Tuning &tuning, bool cold);

}  // namespace android::hwc::governor::tegra
