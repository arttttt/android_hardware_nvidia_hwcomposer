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

Frame Planned(int64_t now, int64_t last_vsync, bool landed) {
  Frame f = {};
  f.seq = 1;
  f.now_ns = now;
  f.last_vsync_ns = last_vsync;
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

  /* A stale phase still places the latch: several periods back. */
  const int64_t old = t0 - 10 * period - 3 * nsPerMs;
  CHECK(LatchDeadline(Planned(t0, old, true), t0, t) ==
        old + 11 * period - margin);

  /* No phase at all: the measured fallbacks. */
  CHECK(LatchDeadline(Planned(t0, 0, true), t0, t) ==
        t0 + int64_t(t.budget_empty_us) * nsPerUs);
  CHECK(LatchDeadline(Planned(t0, 0, false), t0, t) ==
        t0 + int64_t(t.budget_waited_us) * nsPerUs);
}

void CyclesAreHalfAClockPerPixelWithMargin() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  /* Three full screens: 3 x 1536 x 2048 / 2 x 1.25. */
  const double expected = 3.0 * 1536 * 2048 / 2 * 1.25;
  CHECK(MergeCycles(members, 3, t) == expected);

  /* The larger side counts: a member scaled up costs its destination. */
  Member scaled = FullScreen();
  scaled.src_w = 1440;
  scaled.src_h = 1920;
  CHECK(MergeCycles(&scaled, 1, t) == 1536.0 * 2048 / 2 * 1.25);

  CHECK(MergeCycles(nullptr, 0, t) == 0.0);
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

void EmptyPipelineLateInTheFrameNeedsTheTop() {
  Tuning t;
  Member members[3] = {FullScreen(), FullScreen(), FullScreen()};
  /* Ten milliseconds into the frame with the previous flip landed: the
   * nearest latch is ~6 ms away, ~1.75 ms of that goes to the submit. */
  const Frame f = Planned(t0, t0 - 10 * nsPerMs, true);
  const MergeEstimate e = EstimateMerge(f, members, 3, t0, t, false, false);
  CHECK(!e.late);
  CHECK(e.beyond);
  CHECK(e.step_mhz == topStepMhz);
  CHECK(e.need_mhz > topStepMhz);
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

void NoBudgetLeftIsClampedAndMarkedLate() {
  Tuning t;
  Member m = FullScreen();
  /* Planned right at the latch with the previous flip landed. */
  const Frame f = Planned(t0, t0 - period + 500 * nsPerUs, true);
  const MergeEstimate e = EstimateMerge(f, &m, 1, t0, t, false, false);
  CHECK(e.late);
  CHECK(e.budget_ns == nsPerMs);
  CHECK(e.step_mhz == topStepMhz);
}

}  // namespace

int main() {
  ClockStepsAreOrdered();
  ColdIsIdleBeyondThePowergate();
  DeadlineFollowsThePhase();
  CyclesAreHalfAClockPerPixelWithMargin();
  WarmMergeWithAFrameOfBudgetNeedsAModestStep();
  EmptyPipelineLateInTheFrameNeedsTheTop();
  ColdEngineGetsTheSafetyFloor();
  ColdSubmitEatsMoreBudgetUnlessTheProcessorIsLifted();
  NoBudgetLeftIsClampedAndMarkedLate();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("merge_plan_test: all checks passed\n");
  return EXIT_SUCCESS;
}
