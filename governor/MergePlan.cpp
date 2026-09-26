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

#include "governor/MergePlan.h"

#include <algorithm>
#include <cmath>

#include "governor/ClockSteps.h"

namespace android::hwc::governor::tegra {

bool EngineCold(const Frame &frame, int64_t last_warm_ns, int64_t now_ns,
                const Tuning &tuning) {
  const int64_t last_use = std::max(frame.last_engine_use_ns, last_warm_ns);
  return last_use == 0 ||
         now_ns - last_use > int64_t(tuning.powergate_ms) * nsPerMs;
}

int64_t LatchDeadline(const Frame &frame, int64_t now_ns,
                      const Tuning &tuning) {
  if (frame.last_vsync_ns <= 0 || frame.vsync_period_ns <= 0) {
    const uint32_t budget_us = frame.previous_flip_landed
                                   ? tuning.budget_empty_us
                                   : tuning.budget_waited_us;
    return now_ns + int64_t(budget_us) * nsPerUs;
  }

  const int64_t elapsed = now_ns - frame.last_vsync_ns;
  const int64_t periods = elapsed >= 0 ? elapsed / frame.vsync_period_ns : -1;
  int64_t next = frame.last_vsync_ns + (periods + 1) * frame.vsync_period_ns;
  if (!frame.previous_flip_landed)
    next += frame.vsync_period_ns;
  return next - int64_t(tuning.latch_margin_us) * nsPerUs;
}

double MergeCycles(const Member *members, size_t count,
                   const Tuning &tuning) {
  uint64_t area = 0;
  for (size_t i = 0; i < count; ++i) {
    const Member &m = members[i];
    const uint64_t src = uint64_t(m.src_w) * m.src_h;
    const uint64_t dst = uint64_t(m.dst_w) * m.dst_h;
    area += std::max(src, dst);
  }
  return double(area) / tuning.pixels_per_clock * tuning.factor_pct / 100.0;
}

MergeEstimate EstimateMerge(const Frame &frame, const Member *members,
                            size_t count, int64_t now_ns,
                            const Tuning &tuning, bool cold,
                            bool cpu_lifted) {
  MergeEstimate e = {};
  e.cycles = MergeCycles(members, count, tuning);

  const uint32_t submit_us =
      cold ? (cpu_lifted ? tuning.submit_cold_us : tuning.submit_cold_slow_us)
           : tuning.submit_warm_us;
  const int64_t submit_end =
      now_ns + int64_t(tuning.lead_us + submit_us) * nsPerUs;

  e.budget_ns = LatchDeadline(frame, now_ns, tuning) - submit_end;
  if (e.budget_ns < nsPerMs) {
    e.budget_ns = nsPerMs;
    e.late = true;
  }

  const double need_hz = e.cycles * double(nsPerSec) / double(e.budget_ns);
  e.need_mhz =
      uint32_t(std::ceil(std::min<double>(need_hz / 1e6, 4.0 * topStepMhz)));

  e.step_mhz = StepAtLeast(e.need_mhz);
  e.beyond = e.step_mhz == 0;
  if (e.beyond)
    e.step_mhz = topStepMhz;
  if (cold && e.step_mhz < tuning.min_cold_mhz)
    e.step_mhz = tuning.min_cold_mhz;
  return e;
}

}  // namespace android::hwc::governor::tegra
