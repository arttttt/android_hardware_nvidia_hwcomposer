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

namespace android::drm_hwcomposer {

/* Whether an engine's job will be done before the panel takes its frame.
 *
 * Asked when a job is queued: from what the job is -- the pixels it
 * writes -- and from what the engine's recent jobs really took, it
 * predicts when this one will be done, and says whether that misses the
 * next latch. A job predicted late could be dropped then, before the
 * engine has it, and the last picture shown instead: one frame repeated,
 * the panel's cadence kept, nothing piled up behind.
 *
 * It learns as it goes. Each job's real end -- when its fence came due --
 * is told back once known: the model's cost per pixel and its fixed cost
 * are drawn towards what the engine actually did, so a GPU busy with an
 * application's frames is seen as a slower GPU by the next prediction, and
 * the verdict is scored against what happened.
 *
 * Only a judge: nothing here drops anything. Whoever asks decides what to
 * do with the answer, and while the answer is being weighed it is only
 * counted -- the four ways a verdict and the truth can meet.
 */
class DropOracle {
 public:
  struct Verdict {
    bool late = false;
    int64_t predicted_end_ns = 0;
    int64_t deadline_ns = 0;
  };

  /* The job queued at `now_ns`, writing `pixels`, for the panel's latch at
   * `deadline_ns`. */
  Verdict Judge(int64_t now_ns, int64_t deadline_ns, uint64_t pixels) const;

  /* A job judged and then handed to its engine: kept until its end is
   * told. Older ones past the bound are forgotten unscored. */
  void Track(const Verdict &verdict, int64_t queued_ns, uint64_t pixels);

  /* The oldest tracked job ended at `end_ns`: learnt from, and scored. */
  void Ended(int64_t end_ns);
  /* The oldest tracked job's end cannot be known; forgotten unscored. */
  void Lost();
  bool Tracking() const { return !tracked_.empty(); }

  /* What the counts and the model say, one line. */
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
  double fixed_ns() const { return fixed_ns_; }
  double ns_per_pixel() const { return ns_per_pixel_; }

 private:
  struct Tracked {
    Verdict verdict;
    int64_t queued_ns;
    uint64_t pixels;
  };

  void Learn(uint64_t pixels, int64_t took_ns);

  std::deque<Tracked> tracked_;
  Counts counts_;
  /* Seeded with the image engine's measured cost: 0.6 ms for a strip
   * group's handful of rows, a few ms for a screen. */
  double fixed_ns_ = 500000.0;
  double ns_per_pixel_ = 1.0;
};

}  // namespace android::drm_hwcomposer
