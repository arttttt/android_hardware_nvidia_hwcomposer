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

/* The model, checked on a workstation. No device, no framework:
 *
 *     c++ -std=c++17 -I. governor/tests/merge_plan_test.cpp \
 *         governor/MergePlan.cpp -o /tmp/merge_plan_test && /tmp/merge_plan_test
 *
 * The numbers are the measured ones from the transition traces: a
 * full-screen member is 1536x2048, the panel runs at sixty hertz, and the
 * cases are the ones the decision has to get right rather than every
 * combination. */

#include <stdio.h>
#include <stdlib.h>

#include "governor/ClockSteps.h"
#include "governor/MergePlan.h"

using namespace android::hwc::governor;
using namespace android::hwc::governor::tegra;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,  \
              #cond);                                                   \
      failures++;                                                       \
    }                                                                   \
  } while (0)

constexpr int64_t period = 16666667;
constexpr int64_t t0 = 1000 * nsPerSec;  /* an arbitrary "now" */

Member FullScreen() {
  Member m = {};
  m.src_w = 1536;
  m.src_h = 2048;
  m.dst_w = 1536;
  m.dst_h = 2048;
  m.premultiplied = true;
  m.acquire_signaled = true;
  return m;
}

Frame Planned(int64_t now, int64_t last_latch, bool landed) {
  Frame f = {};
  f.seq = 1;
  f.now_ns = now;
  f.last_latch_ns = last_latch;
  f.vsync_period_ns = period;
  f.merge_planned = true;
  f.previous_flip_landed = landed;
  f.power_mode = static_cast<uint8_t>(Power::on);
  return f;
}

void ClockStepsAreOrdered() {
  CHECK(StepAtLeast(0) == 180);
  CHECK(StepAtLeast(180) == 180);
  CHECK(StepAtLeast(181) == 258);
  CHECK(StepAtLeast(504) == 504);
  CHECK(StepAtLeast(756) == 756);
  CHECK(StepAtLeast(757) == 0);
}

void ColdIsIdleBeyondThePowergate() {
  Tuning t;
  Frame f = Planned(t0, 0, true);

  f.last_engine_use_ns = 0;
  CHECK(EngineCold(f, 0, t0, t));

  f.last_engine_use_ns = t0 - 100 * nsPerMs;
  CHECK(!EngineCold(f, 0, t0, t));

  f.last_engine_use_ns = t0 - 600 * nsPerMs;
  CHECK(EngineCold(f, 0, t0, t));

  /* A warm-up of the governor's own counts as use. */
  CHECK(!EngineCold(f, t0 - 100 * nsPerMs, t0, t));
}

void DeadlineFollowsThePhase() {
  Tuning t;
  const int64_t margin = int64_t(t.latch_margin_us) * nsPerUs;

  /* Five milliseconds after a vsync, previous flip landed: the next vsync
   * is the latch. */
  const int64_t vsync = t0 - 5 * nsPerMs;
  CHECK(LatchDeadline(Planned(t0, vsync, true), t0, t) ==
        vsync + period - margin);

  /* Not landed: the one after. */
  CHECK(LatchDeadline(Planned(t0, vsync, false), t0, t) ==
        vsync + 2 * period - margin);

  /* A phase a few periods old still places the latch. */
  const int64_t old = t0 - 3 * period - 3 * nsPerMs;
  CHECK(LatchDeadline(Planned(t0, old, true), t0, t) ==
        old + 4 * period - margin);

  /* No phase at all: the measured fallbacks. */
  CHECK(LatchDeadline(Planned(t0, 0, true), t0, t) ==
        t0 + int64_t(t.budget_empty_us) * nsPerUs);
  CHECK(LatchDeadline(Planned(t0, 0, false), t0, t) ==
        t0 + int64_t(t.budget_waited_us) * nsPerUs);
}

void CyclesAreTheJobPlusHalfAClockPerPixelWithMargin() {
  Tuning t;
  const double job = t.job_kcycles * 1000.0;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  /* Three full screens: the job plus 3 x 1536 x 2048 / 2, times 1.25. */
  const double expected = (job + 3.0 * 1536 * 2048 / 2) * 1.25;
  CHECK(MergeCycles(members, 3, t) == expected);

  /* The larger side counts: a member scaled up costs its destination. */
  Member scaled = FullScreen();
  scaled.src_w = 1440;
  scaled.src_h = 1920;
  CHECK(MergeCycles(&scaled, 1, t) == (job + 1536.0 * 2048 / 2) * 1.25);

  /* An empty merge still costs a job -- and is never planned anyway. */
  CHECK(MergeCycles(nullptr, 0, t) == job * 1.25);
}

void WarmMergeWithAFrameOfBudgetNeedsAModestStep() {
  Tuning t;
  Member members[2] = {FullScreen(), FullScreen()};
  /* Right after a vsync, previous flip not landed: the frame has the
   * best part of two periods -- the long, waited budget. */
  const Frame f = Planned(t0, t0 - 1 * nsPerMs, false);
  const MergeEstimate e = EstimateMerge(f, members, 2, t0, t, false, false);
  CHECK(!e.late);
  CHECK(!e.beyond);
  /* ~3.9 M cycles over ~30 ms is well under the lowest step. */
  CHECK(e.budget_ns > 25 * nsPerMs);
  CHECK(e.step_mhz == 180);
}

void EmptyPipelineLateInTheFrameSlipsButNotPastTheNextMerge() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  /* Ten milliseconds into the frame with the previous flip landed: the
   * nearest latch is ~6.7 ms away, and 4.7 M raw cycles take 6.2 ms at
   * the top step even down the shortest path. The frame will take the
   * latch after -- but the next frame's merge arrives 6.5 ms past the
   * nearest latch, and this one has to be out of the engine by then. */
  const Frame f = Planned(t0, t0 - 10 * nsPerMs, true);
  const MergeEstimate e = EstimateMerge(f, members, 3, t0, t, false, false);
  CHECK(e.slipped);
  CHECK(!e.beyond);
  CHECK(!e.late);
  const int64_t nearest = t0 + period - 10 * nsPerMs;
  CHECK(e.deadline_ns == nearest + int64_t(t.next_submit_us) * nsPerUs);
  /* ~5.9 M cycles with margin over ~11.4 ms: 552 MHz. */
  CHECK(e.step_mhz == 552);
}

void SlipWhileWaitingForThePreviousFlip() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  /* Not landed: the frame already targets the latch after the nearest,
   * ten milliseconds into the frame that is ~23 ms away -- no slip. */
  const Frame f = Planned(t0, t0 - 10 * nsPerMs, false);
  const MergeEstimate e = EstimateMerge(f, members, 3, t0, t, false, false);
  CHECK(!e.slipped);
  CHECK(!e.beyond);
  CHECK(e.budget_ns > 20 * nsPerMs);
}

void ColdSlipStillGetsTheSafetyFloor() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  const Frame f = Planned(t0, t0 - 10 * nsPerMs, true);
  const MergeEstimate e = EstimateMerge(f, members, 3, t0, t, true, false);
  CHECK(e.slipped);
  CHECK(e.step_mhz >= t.min_cold_mhz);
}

void AStalePhaseIsNoPhase() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  /* A latch three seconds old: whatever grid the panel had then is gone.
   * The fallback budget applies and nothing slips. */
  const Frame f = Planned(t0, t0 - 3 * nsPerSec, true);
  CHECK(!PhaseKnown(f, t0, t));
  CHECK(LatchDeadline(f, t0, t) == t0 + int64_t(t.budget_empty_us) * nsPerUs);
  const MergeEstimate e = EstimateMerge(f, members, 3, t0, t, false, false);
  CHECK(!e.slipped);
  CHECK(e.beyond);

  /* A second old -- the first frame after a pause -- is still a phase. */
  const Frame g = Planned(t0, t0 - nsPerSec, true);
  CHECK(PhaseKnown(g, t0, t));
}

void AMarginalLatchIsAskedTheTopStepNotSlipped() {
  Tuning t;
  Member members[2] = {FullScreen(), FullScreen()};
  /* Ten milliseconds into the frame: ~6 ms to the latch, 3.1 M raw cycles
   * take 4.2 ms at the top step -- it fits down the shortest path, though
   * not with the margin and the usual lead. Doubt goes to the higher
   * step, not to the next frame. */
  const Frame f = Planned(t0, t0 - 10 * nsPerMs, true);
  const MergeEstimate e = EstimateMerge(f, members, 2, t0, t, false, false);
  CHECK(!e.slipped);
  CHECK(e.beyond);
  CHECK(e.step_mhz == topStepMhz);
  CHECK(e.deadline_ns < t0 + 7 * nsPerMs);
}

void SlipsOnceOnly() {
  Tuning t;
  /* A merge no latch can hold: slipped once, still beyond. */
  Member huge = FullScreen();
  huge.src_w = huge.dst_w = 8192;
  huge.src_h = huge.dst_h = 8192;
  const Frame f = Planned(t0, t0 - 10 * nsPerMs, true);
  const MergeEstimate e = EstimateMerge(f, &huge, 1, t0, t, false, false);
  CHECK(e.slipped);
  CHECK(e.beyond);
  CHECK(e.step_mhz == topStepMhz);
}

void WithoutAPhaseNothingSlips() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  const Frame f = Planned(t0, 0, true);
  const MergeEstimate e = EstimateMerge(f, members, 3, t0, t, false, false);
  CHECK(!e.slipped);
  CHECK(e.beyond);
  CHECK(e.step_mhz == topStepMhz);
}

void ColdEngineGetsTheSafetyFloor() {
  Tuning t;
  Member small = FullScreen();
  small.src_w = small.dst_w = 200;
  small.src_h = small.dst_h = 200;
  const Frame f = Planned(t0, t0 - 1 * nsPerMs, false);

  const MergeEstimate warm = EstimateMerge(f, &small, 1, t0, t, false, false);
  CHECK(warm.step_mhz == 180);

  const MergeEstimate cold = EstimateMerge(f, &small, 1, t0, t, true, false);
  CHECK(cold.step_mhz == t.min_cold_mhz);

  /* Trusting the model turns the floor off. */
  t.min_cold_mhz = 0;
  const MergeEstimate trusted = EstimateMerge(f, &small, 1, t0, t, true, false);
  CHECK(trusted.step_mhz == 180);
}

void ColdSubmitEatsMoreBudgetUnlessTheProcessorIsLifted() {
  Tuning t;
  Member m = FullScreen();
  const Frame f = Planned(t0, t0 - 1 * nsPerMs, false);
  const MergeEstimate warm = EstimateMerge(f, &m, 1, t0, t, false, false);
  const MergeEstimate lifted = EstimateMerge(f, &m, 1, t0, t, true, true);
  const MergeEstimate slow = EstimateMerge(f, &m, 1, t0, t, true, false);
  CHECK(warm.budget_ns - lifted.budget_ns ==
        int64_t(t.submit_cold_us - t.submit_warm_us) * nsPerUs);
  CHECK(lifted.budget_ns - slow.budget_ns ==
        int64_t(t.submit_cold_slow_us - t.submit_cold_us) * nsPerUs);
}

void NoBudgetLeftSlipsRatherThanAskingTheTop() {
  Tuning t;
  Member m = FullScreen();
  /* Planned half a millisecond before the latch with the previous flip
   * landed: gone even down the shortest path; the slip runs to the next
   * frame's merge, ~7 ms past that latch. */
  const Frame f = Planned(t0, t0 - period + 500 * nsPerUs, true);
  const MergeEstimate e = EstimateMerge(f, &m, 1, t0, t, false, false);
  CHECK(e.slipped);
  CHECK(!e.late);
  CHECK(e.budget_ns > 4 * nsPerMs);
  /* ~2.3 M cycles over ~5.3 ms: 462 MHz. */
  CHECK(e.step_mhz == 462);
}

}  // namespace

int main() {
  ClockStepsAreOrdered();
  ColdIsIdleBeyondThePowergate();
  DeadlineFollowsThePhase();
  CyclesAreTheJobPlusHalfAClockPerPixelWithMargin();
  WarmMergeWithAFrameOfBudgetNeedsAModestStep();
  EmptyPipelineLateInTheFrameSlipsButNotPastTheNextMerge();
  SlipWhileWaitingForThePreviousFlip();
  ColdSlipStillGetsTheSafetyFloor();
  AStalePhaseIsNoPhase();
  AMarginalLatchIsAskedTheTopStepNotSlipped();
  SlipsOnceOnly();
  WithoutAPhaseNothingSlips();
  ColdEngineGetsTheSafetyFloor();
  ColdSubmitEatsMoreBudgetUnlessTheProcessorIsLifted();
  NoBudgetLeftSlipsRatherThanAskingTheTop();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("merge_plan_test: all checks passed\n");
  return EXIT_SUCCESS;
}
