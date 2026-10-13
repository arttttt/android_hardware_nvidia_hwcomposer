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

namespace android::drm_hwcomposer {

namespace {

/* How far one job moves the model: an eighth, so a few jobs under a new
 * load are enough to follow it, and one odd job is not. */
constexpr double kStep = 1.0 / 8.0;

/* Jobs this small say what the engine costs to start; larger ones what
 * it costs per pixel. */
constexpr uint64_t kSmallPixels = 100000;

}  // namespace

DropOracle::Verdict DropOracle::Judge(int64_t now_ns, int64_t deadline_ns,
                                      uint64_t pixels) const {
  Verdict v;
  v.deadline_ns = deadline_ns;
  v.predicted_end_ns = now_ns + static_cast<int64_t>(Base(pixels) * recent_);
  v.late = v.predicted_end_ns > deadline_ns - kFlipMarginNs + slack_ns_;
  return v;
}

void DropOracle::Track(const Verdict &verdict, int64_t queued_ns,
                       uint64_t pixels) {
  counts_.judged++;
  tracked_.push_back(Tracked{verdict, queued_ns, pixels});
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

  const int64_t took_ns = end_ns - t.queued_ns;
  if (took_ns > 0) {
    const double ratio = static_cast<double>(took_ns) / Base(t.pixels);
    recent_ += 0.5 * (std::clamp(ratio, kRecentMin, kRecentMax) - recent_);
  }
  Learn(t.pixels, took_ns);
}

void DropOracle::Lost() {
  if (!tracked_.empty())
    tracked_.pop_front();
}

void DropOracle::Learn(uint64_t pixels, int64_t took_ns) {
  if (took_ns <= 0)
    return;
  const double took = static_cast<double>(took_ns);
  if (pixels < kSmallPixels) {
    fixed_ns_ += kStep * (took - fixed_ns_);
    return;
  }
  const double per_pixel =
      std::max(0.0, took - fixed_ns_) / static_cast<double>(pixels);
  ns_per_pixel_ += kStep * (per_pixel - ns_per_pixel_);
}

std::string DropOracle::Describe() const {
  char line[240];
  snprintf(line, sizeof(line),
           "judged %llu; said late: was %llu, was not %llu; said on time: "
           "was late %llu, was %llu; model %.2f ms + %.2f ms/Mpx, recent "
           "x%.2f, slack %.2f ms",
           static_cast<unsigned long long>(counts_.judged),
           static_cast<unsigned long long>(counts_.said_late_was_late),
           static_cast<unsigned long long>(counts_.said_late_was_on_time),
           static_cast<unsigned long long>(counts_.said_on_time_was_late),
           static_cast<unsigned long long>(counts_.said_on_time_was_on_time),
           /* ns per pixel is ms per million pixels */
           fixed_ns_ / 1e6, ns_per_pixel_, recent_,
           static_cast<double>(slack_ns_) / 1e6);
  return line;
}

}  // namespace android::drm_hwcomposer
