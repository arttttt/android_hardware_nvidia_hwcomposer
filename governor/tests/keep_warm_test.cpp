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

/* Keeping an engine up, checked on a workstation:
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. governor/tests/keep_warm_test.cpp \
 *       -o /tmp/keep_warm_test && /tmp/keep_warm_test
 */

#include <stdio.h>

#include "governor/KeepWarm.h"

using android::hwc::governor::KeepWarm;

namespace {

int failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                            \
    }                                                        \
  } while (0)

constexpr int64_t kMs = 1000000;

KeepWarm Make() { return KeepWarm(1500 * kMs, 400 * kMs, 500 * kMs); }

/* Nothing before the engine has worked, nor while it is fresh. */
void TestNotBeforeWork() {
  KeepWarm k = Make();
  CHECK(!k.Due(450 * kMs));
  k.Worked(1000 * kMs);
  CHECK(!k.Due(1100 * kMs));
  CHECK(!k.Due(1399 * kMs));
  CHECK(k.Due(1400 * kMs));
}

/* Due only while the engine is still up: once it has powered down, waking
 * it is the merge's to pay for, not a frame's. */
void TestNotOnceDown() {
  KeepWarm k = Make();
  k.Worked(1000 * kMs);
  CHECK(k.Due(1499 * kMs));
  CHECK(!k.Due(1500 * kMs));
  CHECK(!k.Due(1700 * kMs));
}

/* A tiny job keeps it up for another stretch, but not past the window
 * after the last real work; real work opens the window again. */
void TestWindow() {
  KeepWarm k = Make();
  k.Worked(1000 * kMs);
  k.Warmed(1420 * kMs);
  CHECK(!k.Due(1700 * kMs));
  CHECK(k.Due(1830 * kMs));
  k.Warmed(1830 * kMs);
  CHECK(k.Due(2240 * kMs));
  k.Warmed(2240 * kMs);
  /* The next would be due at 2640 ms, past the window's end at 2500. */
  CHECK(!k.Due(2650 * kMs));
  k.Worked(2600 * kMs);
  CHECK(k.Due(3000 * kMs));
  CHECK(k.last_up_ns() == 2600 * kMs);
}

/* For a timer: the next due time follows the last job, and there is none
 * past the window or once given up -- until real work again. */
void TestTimer() {
  KeepWarm k = Make();
  CHECK(k.NextDueNs() == 0);
  k.Worked(1000 * kMs);
  CHECK(k.NextDueNs() == 1400 * kMs);
  k.Warmed(1400 * kMs);
  CHECK(k.NextDueNs() == 1800 * kMs);
  k.Warmed(2200 * kMs);
  CHECK(k.NextDueNs() == 0); /* 2600 ms is past the window */
  k.Worked(3000 * kMs);
  k.GiveUp();
  CHECK(k.NextDueNs() == 0);
  CHECK(!k.Due(3450 * kMs));
  k.Worked(4000 * kMs);
  CHECK(k.NextDueNs() == 4400 * kMs);
}

/* Nought turns it off. */
void TestOff() {
  KeepWarm k(0, 400 * kMs, 500 * kMs);
  k.Worked(1000 * kMs);
  CHECK(!k.Due(1450 * kMs));
}

}  // namespace

int main() {
  TestNotBeforeWork();
  TestNotOnceDown();
  TestWindow();
  TestTimer();
  TestOff();
  if (failures == 0)
    printf("keep_warm_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
