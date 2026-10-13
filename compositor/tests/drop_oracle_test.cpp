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

/* Host test for the drop oracle.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. compositor/tests/drop_oracle_test.cpp \
 *       compositor/DropOracle.cpp -o /tmp/drop_oracle_test && /tmp/drop_oracle_test
 */

#include <cstdio>

#include "compositor/DropOracle.h"

using namespace android::drm_hwcomposer;

namespace {

int failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                            \
    }                                                        \
  } while (0)

constexpr int64_t kMs = 1000000;
constexpr uint64_t kScreen = 1536 * 2048;
constexpr uint64_t kStrips = 420864;

/* Two cells, a warm and a cold engine: the same screen costs 3.8 and 7 ms. */
enum Cell : size_t { kWarm, kCold };
DropOracle Make(int64_t slack = 0) {
  return DropOracle({{0.6 * kMs, 1.0}, {2.0 * kMs, 1.6}}, slack);
}

/* The cell decides: a screen 6 ms before the latch is on time warm, late
 * cold; the flip's margin counts against the job; slack moves the line. */
void TestJudge() {
  DropOracle o = Make();
  CHECK(!o.Judge(kWarm, 0, 6 * kMs, kScreen).late);
  CHECK(o.Judge(kCold, 0, 6 * kMs, kScreen).late);
  CHECK(!o.Judge(kWarm, 0, 2 * kMs, kStrips).late);
  CHECK(o.Judge(kWarm, 0, int64_t(1.3 * kMs), kStrips).late);
  CHECK(!Make(2 * kMs).Judge(kWarm, 0, int64_t(1.3 * kMs), kStrips).late);
}

/* A cell learns only from its own jobs, towards their longer end: jobs
 * slower than it thinks move it fast, faster ones move it slowly. */
void TestLearnsPerCell() {
  DropOracle o = Make();
  const double warm = o.cell(kWarm).ns_per_pixel;
  for (int i = 0; i < 30; ++i) {
    o.Track(kCold, o.Judge(kCold, 0, 100 * kMs, kScreen), 0, kScreen);
    o.Ended(int64_t(2.0 * kMs + 3.0 * double(kScreen)));
  }
  CHECK(o.cell(kCold).ns_per_pixel > 2.5);
  CHECK(o.cell(kWarm).ns_per_pixel == warm);

  /* Half the jobs at 1x, half at 2x the warm cost: the cell settles above
   * the middle, towards the slower half. */
  for (int i = 0; i < 400; ++i) {
    const double per = i % 2 ? 2.0 : 1.0;
    o.Track(kWarm, o.Judge(kWarm, 0, 100 * kMs, kScreen), 0, kScreen);
    o.Ended(int64_t(0.6 * kMs + per * double(kScreen)));
  }
  CHECK(o.cell(kWarm).ns_per_pixel > 1.6);
  CHECK(o.cell(kWarm).ns_per_pixel <= 2.05);

  /* Small jobs teach the fixed cost. */
  for (int i = 0; i < 40; ++i) {
    o.Track(kWarm, o.Judge(kWarm, 0, 100 * kMs, 1000), 0, 1000);
    o.Ended(2 * kMs);
  }
  CHECK(o.cell(kWarm).fixed_ns > 1.8 * kMs);
}

/* Each verdict is scored against the job's real end, in its own count. */
void TestScores() {
  DropOracle o = Make();
  o.Track(kCold, o.Judge(kCold, 0, 2 * kMs, kScreen), 0, kScreen);
  o.Ended(5 * kMs); /* said late, was late */
  o.Track(kCold, o.Judge(kCold, 0, 2 * kMs, kScreen), 0, kScreen);
  o.Ended(1 * kMs); /* said late, was not */
  o.Track(kWarm, o.Judge(kWarm, 0, 10 * kMs, kStrips), 0, kStrips);
  o.Ended(12 * kMs); /* said on time, was late */
  o.Track(kWarm, o.Judge(kWarm, 0, 10 * kMs, kStrips), 0, kStrips);
  o.Ended(1 * kMs);
  const auto &c = o.counts();
  CHECK(c.judged == 4);
  CHECK(c.said_late_was_late == 1);
  CHECK(c.said_late_was_on_time == 1);
  CHECK(c.said_on_time_was_late == 1);
  CHECK(c.said_on_time_was_on_time == 1);
}

/* Tracking is bounded; a lost end, an end with nothing tracked and a cell
 * that does not exist are harmless. */
void TestBounds() {
  DropOracle o = Make();
  o.Ended(1);
  o.Lost();
  o.Track(7, o.Judge(7, 0, 10 * kMs, 1), 0, 1);
  CHECK(!o.Tracking());
  for (size_t i = 0; i < DropOracle::kMostTracked + 5; ++i)
    o.Track(kWarm, o.Judge(kWarm, 0, 10 * kMs, 1), 0, 1);
  size_t n = 0;
  while (o.Tracking()) {
    o.Lost();
    n++;
  }
  CHECK(n == DropOracle::kMostTracked);
}

}  // namespace

int main() {
  TestJudge();
  TestLearnsPerCell();
  TestScores();
  TestBounds();
  if (failures == 0)
    std::printf("drop_oracle_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
