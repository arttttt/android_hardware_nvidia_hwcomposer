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

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace android::drm_hwcomposer {

/* Whether an engine's job will be done before the panel takes its frame.
 *
 * Asked when a job is queued, while the engine has not got it yet: a job
 * predicted late could be dropped then and the last picture shown instead
 * -- one frame repeated, the panel's cadence kept, nothing piled up behind.
 *
 * What a job takes depends less on its pixels than on the state the engine
 * is found in. Measured on the device: the image engine merges a screen in
 * 3.8 ms warm and 7 ms when it was powered down after half a second idle;
 * the GPU draws in about 1 ms on the heels of its last drawing, 2 ms after
 * a pause, 8.6 ms once its rail was gated. One cost per pixel averaged
 * those, and called warm screens late and cold ones on time. So the caller
 * names the job's cell -- the engine's state, and for the image engine the
 * job's size -- and each cell keeps its own model: a fixed cost and a cost
 * per pixel, learnt from that cell's jobs alone.
 *
 * Learnt towards the upper end of what a cell's jobs take, not the middle:
 * a job longer than predicted moves the model four times as far as one
 * shorter, which settles near the fifth of jobs that take longest -- the
 * ones that decide whether a frame is late.
 *
 * Only a judge: nothing here drops anything. Each job's real end, told
 * back once its fence is due, is learnt from and scores the verdict, in
 * one of four counts.
 */
class DropOracle {
 public:
  struct Seed {
    double fixed_ns;
    double ns_per_pixel;
  };

  /* One cell for each of `seeds`, starting from it; `slack_ns` is added
   * to the latch before a verdict. */
  explicit DropOracle(std::vector<Seed> seeds, int64_t slack_ns = 0);

  struct Verdict {
    bool late = false;
    int64_t predicted_end_ns = 0;
    int64_t deadline_ns = 0;
  };

  /* The job of cell `cell`, queued at `now_ns`, writing `pixels`, for the
   * panel's latch at `deadline_ns`. */
  Verdict Judge(size_t cell, int64_t now_ns, int64_t deadline_ns,
                uint64_t pixels) const;

  /* A job judged and then handed to its engine: kept until its end is
   * told. Older ones past the bound are forgotten unscored. */
  void Track(size_t cell, const Verdict &verdict, int64_t queued_ns,
             uint64_t pixels);

  /* The oldest tracked job ended at `end_ns`: learnt from, and scored. */
  void Ended(int64_t end_ns);
  /* The oldest tracked job's end cannot be known; forgotten unscored. */
  void Lost();
  bool Tracking() const { return !tracked_.empty(); }

  /* What the counts and the cells say, one line. */
  std::string Describe() const;

  /* A job is late when its fence comes due after this much before the
   * latch: the flip must be programmed after the fence and before the
   * latch. */
  static constexpr int64_t kFlipMarginNs = 500000;
  static constexpr size_t kMostTracked = 8;

  struct Counts {
    uint64_t judged = 0;
    uint64_t said_late_was_late = 0;
    uint64_t said_late_was_on_time = 0;
    uint64_t said_on_time_was_late = 0;
    uint64_t said_on_time_was_on_time = 0;
  };
  const Counts &counts() const { return counts_; }
  const Seed &cell(size_t i) const { return cells_[i]; }
  int64_t slack_ns() const { return slack_ns_; }

 private:
  struct Tracked {
    size_t cell;
    Verdict verdict;
    int64_t queued_ns;
    uint64_t pixels;
  };

  double Predict(size_t cell, uint64_t pixels) const;
  void Learn(size_t cell, uint64_t pixels, int64_t took_ns);

  std::vector<Seed> cells_;
  int64_t slack_ns_ = 0;
  std::deque<Tracked> tracked_;
  Counts counts_;
};

}  // namespace android::drm_hwcomposer
