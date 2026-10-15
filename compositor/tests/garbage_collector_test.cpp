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

/* Host test for the collector.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. compositor/tests/garbage_collector_test.cpp \
 *       -o /tmp/garbage_collector_test -lpthread && /tmp/garbage_collector_test
 */

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "compositor/GarbageCollector.h"

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

/* Jobs run on a thread of the collector's own, in the order posted. */
void TestOrderAndThread() {
  GarbageCollector r("test-gc");
  std::vector<int> seen;
  std::thread::id ran_on;
  for (int i = 0; i < 100; ++i)
    r.Post([&seen, &ran_on, i] {
      seen.push_back(i);
      ran_on = std::this_thread::get_id();
    });
  r.Flush();
  CHECK(seen.size() == 100);
  for (int i = 0; i < 100 && i < static_cast<int>(seen.size()); ++i)
    CHECK(seen[i] == i);
  CHECK(ran_on != std::this_thread::get_id());
  CHECK(r.pending() == 0);
}

/* A descriptor handed over is closed there; a negative one is nothing. */
void TestClose() {
  GarbageCollector r("test-gc");
  int fds[2];
  CHECK(pipe(fds) == 0);
  r.Close(fds[0]);
  r.Close(-1);
  r.Flush();
  CHECK(fcntl(fds[0], F_GETFD) == -1);
  CHECK(fcntl(fds[1], F_GETFD) != -1);
  close(fds[1]);
}

/* The collector going away runs every job posted first. */
void TestDrainOnDestruction() {
  std::atomic<int> ran{0};
  {
    GarbageCollector r("test-gc");
    for (int i = 0; i < 50; ++i)
      r.Post([&ran] {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        ran++;
      });
  }
  CHECK(ran.load() == 50);
}

/* A collector never posted to never starts a thread, and goes quietly. */
void TestUnused() {
  GarbageCollector r("test-gc");
  r.Flush();
  CHECK(r.pending() == 0);
}

/* Many posters at once: every job runs once. */
void TestManyPosters() {
  GarbageCollector r("test-gc");
  std::atomic<int> ran{0};
  std::vector<std::thread> posters;
  for (int t = 0; t < 4; ++t)
    posters.emplace_back([&r, &ran] {
      for (int i = 0; i < 1000; ++i)
        r.Post([&ran] { ran++; });
    });
  for (auto &p : posters)
    p.join();
  r.Flush();
  CHECK(ran.load() == 4000);
}

}  // namespace

int main() {
  TestOrderAndThread();
  TestClose();
  TestDrainOnDestruction();
  TestUnused();
  TestManyPosters();
  if (failures == 0)
    std::printf("garbage_collector_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
