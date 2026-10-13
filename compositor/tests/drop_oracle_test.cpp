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

/* A small job well ahead of the latch is on time; a screen just before it
 * is late; the flip's margin counts against the job. */
void TestJudge() {
  DropOracle o;
  CHECK(!o.Judge(0, 10 * kMs, kStrips).late);
  CHECK(o.Judge(0, 2 * kMs, kScreen).late);
  /* 0.5 ms fixed + 0.42 ms of strips = 0.92 ms: inside 1.5 ms less the
   * margin, outside 1.3 ms less it. */
  CHECK(!o.Judge(0, int64_t(1.5 * kMs), kStrips).late);
  CHECK(o.Judge(0, int64_t(1.3 * kMs), kStrips).late);
}

/* The model follows what the engine really takes: jobs three times as
 * slow as it thought make it predict three times as slow. */
void TestLearns() {
  DropOracle o;
  const double before = o.ns_per_pixel();
  for (int i = 0; i < 40; ++i) {
    const auto v = o.Judge(0, 100 * kMs, kScreen);
    o.Track(v, 0, kScreen);
    o.Ended(int64_t(o.fixed_ns() + 3.0 * double(kScreen)));
  }
  CHECK(o.ns_per_pixel() > 2.5 * before);
  for (int i = 0; i < 40; ++i) {
    const auto v = o.Judge(0, 100 * kMs, 1000);
    o.Track(v, 0, 1000);
    o.Ended(2 * kMs);
  }
  CHECK(o.fixed_ns() > 1.8 * kMs);
}

/* Each verdict is scored against the job's real end, in its own cell. */
void TestScores() {
  DropOracle o;
  auto v = o.Judge(0, 2 * kMs, kScreen); /* said late */
  o.Track(v, 0, kScreen);
  o.Ended(5 * kMs); /* was late */
  v = o.Judge(0, 2 * kMs, kScreen);
  o.Track(v, 0, kScreen);
  o.Ended(1 * kMs); /* was not */
  v = o.Judge(0, 10 * kMs, kStrips); /* said on time */
  o.Track(v, 0, kStrips);
  o.Ended(12 * kMs); /* was late */
  v = o.Judge(0, 10 * kMs, kStrips);
  o.Track(v, 0, kStrips);
  o.Ended(1 * kMs);
  const auto &c = o.counts();
  CHECK(c.judged == 4);
  CHECK(c.said_late_was_late == 1);
  CHECK(c.said_late_was_on_time == 1);
  CHECK(c.said_on_time_was_late == 1);
  CHECK(c.said_on_time_was_on_time == 1);
}

/* A run of jobs slower than the model shows in the next verdict at once:
 * strips that the model alone would call on time are called late. */
void TestRecent() {
  DropOracle o;
  CHECK(!o.Judge(0, 3 * kMs, kStrips).late);
  for (int i = 0; i < 2; ++i) {
    o.Track(o.Judge(0, 100 * kMs, kStrips), 0, kStrips);
    o.Ended(4 * kMs);
  }
  CHECK(o.recent() > 2.5);
  CHECK(o.Judge(0, 3 * kMs, kStrips).late);
  /* and back once they are quick again */
  for (int i = 0; i < 6; ++i) {
    o.Track(o.Judge(0, 100 * kMs, kStrips), 0, kStrips);
    o.Ended(1 * kMs);
  }
  CHECK(!o.Judge(0, 3 * kMs, kStrips).late);
}

/* Slack moves the threshold: a job that misses by less than it is not
 * called late. */
void TestSlack() {
  DropOracle strict;
  DropOracle lenient(2 * kMs);
  CHECK(strict.Judge(0, int64_t(1.3 * kMs), kStrips).late);
  CHECK(!lenient.Judge(0, int64_t(1.3 * kMs), kStrips).late);
  CHECK(lenient.slack_ns() == 2 * kMs);
}

/* Tracking is bounded; a lost end and an end with nothing tracked are
 * harmless. */
void TestBounds() {
  DropOracle o;
  o.Ended(1);
  o.Lost();
  for (size_t i = 0; i < DropOracle::kMostTracked + 5; ++i)
    o.Track(o.Judge(0, 10 * kMs, 1), 0, 1);
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
  TestLearns();
  TestScores();
  TestRecent();
  TestSlack();
  TestBounds();
  if (failures == 0)
    std::printf("drop_oracle_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
