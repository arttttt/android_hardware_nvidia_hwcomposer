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

#include "compositor/DropOracle.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace android::drm_hwcomposer {

namespace {

/* How far one job moves its cell: a fifth of the error when the job took
 * longer than predicted, a twentieth when shorter -- the model settles
 * where about four jobs in five take less than it says. */
constexpr double kStepUp = 1.0 / 5.0;
constexpr double kStepDown = 1.0 / 20.0;

/* Jobs this small teach a cell what it costs to start; larger ones what
 * it costs per pixel. */
constexpr uint64_t kSmallPixels = 100000;

}  // namespace

DropOracle::DropOracle(std::vector<Seed> seeds, int64_t slack_ns)
    : cells_(std::move(seeds)), slack_ns_(slack_ns) {
}

double DropOracle::Predict(size_t cell, uint64_t pixels) const {
  if (cell >= cells_.size())
    return 0.0;
  const Seed &c = cells_[cell];
  return c.fixed_ns + c.ns_per_pixel * static_cast<double>(pixels);
}

DropOracle::Verdict DropOracle::Judge(size_t cell, int64_t now_ns,
                                      int64_t deadline_ns,
                                      uint64_t pixels) const {
  Verdict v;
  v.deadline_ns = deadline_ns;
  v.predicted_end_ns = now_ns + static_cast<int64_t>(Predict(cell, pixels));
  v.late = v.predicted_end_ns > deadline_ns - kFlipMarginNs + slack_ns_;
  return v;
}

void DropOracle::Track(size_t cell, const Verdict &verdict, int64_t queued_ns,
                       uint64_t pixels) {
  if (cell >= cells_.size())
    return;
  counts_.judged++;
  tracked_.push_back(Tracked{cell, verdict, queued_ns, pixels});
  while (tracked_.size() > kMostTracked)
    tracked_.pop_front();
}

void DropOracle::Ended(int64_t end_ns) {
  if (tracked_.empty())
    return;
  const Tracked t = tracked_.front();
  tracked_.pop_front();

  const bool was_late = end_ns > t.verdict.deadline_ns - kFlipMarginNs;
  if (t.verdict.late)
    (was_late ? counts_.said_late_was_late : counts_.said_late_was_on_time)++;
  else
    (was_late ? counts_.said_on_time_was_late
              : counts_.said_on_time_was_on_time)++;

  Learn(t.cell, t.pixels, end_ns - t.queued_ns);
}

void DropOracle::Lost() {
  if (!tracked_.empty())
    tracked_.pop_front();
}

void DropOracle::Learn(size_t cell, uint64_t pixels, int64_t took_ns) {
  if (took_ns <= 0 || cell >= cells_.size())
    return;
  Seed &c = cells_[cell];
  const double took = static_cast<double>(took_ns);
  const double error = took - Predict(cell, pixels);
  const double step = error > 0 ? kStepUp : kStepDown;
  if (pixels < kSmallPixels) {
    c.fixed_ns = std::max(0.0, c.fixed_ns + step * error);
    return;
  }
  c.ns_per_pixel = std::max(
      0.0, c.ns_per_pixel + step * error / static_cast<double>(pixels));
}

std::string DropOracle::Describe() const {
  char line[160];
  snprintf(line, sizeof(line),
           "judged %llu; said late: was %llu, was not %llu; said on time: "
           "was late %llu, was %llu; slack %.2f ms; cells",
           static_cast<unsigned long long>(counts_.judged),
           static_cast<unsigned long long>(counts_.said_late_was_late),
           static_cast<unsigned long long>(counts_.said_late_was_on_time),
           static_cast<unsigned long long>(counts_.said_on_time_was_late),
           static_cast<unsigned long long>(counts_.said_on_time_was_on_time),
           static_cast<double>(slack_ns_) / 1e6);
  std::string out = line;
  for (const Seed &c : cells_) {
    /* ns per pixel is ms per million pixels */
    char one[40];
    snprintf(one, sizeof(one), " %.2f+%.2f", c.fixed_ns / 1e6,
             c.ns_per_pixel);
    out += one;
  }
  return out;
}

}  // namespace android::drm_hwcomposer
