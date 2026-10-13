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

/* Host test for the engines' job queue.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. compositor/tests/job_queue_test.cpp \
 *       -o /tmp/job_queue_test && /tmp/job_queue_test
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "compositor/JobQueue.h"

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

/* A job that says when it is let go, as a job holding fences would close
 * them. */
struct Job {
  int frame;
  std::shared_ptr<int> held;
};

using Queue = JobQueue<Job, std::string>;
using State = Queue::State;

/* Queued, taken, submitted, collected. */
void TestOrdinary() {
  Queue q;
  const auto id = q.Queue(Job{1, nullptr}, true);
  auto taken = q.Take();
  CHECK(taken.has_value());
  CHECK(taken->first == id);
  CHECK(taken->second.frame == 1);
  q.Done(id, true, std::string("fence-1"));
  const auto out = q.Resolve(id, true);
  CHECK(out.state == State::kSubmitted);
  CHECK(out.result && *out.result == "fence-1");
  /* Collected once; asked again it is gone. */
  CHECK(q.Resolve(id, true).state == State::kCancelled);
}

/* A newer job drops a droppable one the worker has not taken, and lets
 * its contents go at once; a job that may not be dropped stays. */
void TestLatestWins() {
  Queue q;
  auto held = std::make_shared<int>(0);
  const auto first = q.Queue(Job{1, held}, true);
  CHECK(held.use_count() == 2);
  const auto second = q.Queue(Job{2, nullptr}, true);
  CHECK(held.use_count() == 1);
  CHECK(q.Resolve(first, true).state == State::kDropped);
  CHECK(q.dropped() == 1);

  auto taken = q.Take();
  CHECK(taken && taken->first == second);
  q.Done(second, true, std::string("fence-2"));
  CHECK(q.Resolve(second, true).state == State::kSubmitted);

  const auto keep = q.Queue(Job{3, nullptr}, false);
  q.Queue(Job{4, nullptr}, true);
  taken = q.Take();
  CHECK(taken && taken->first == keep);
}

/* Never two drops in a row: the second superseded job is drawn after all,
 * and a submission lets drops happen again. */
void TestNotTwiceInARow() {
  Queue q;
  const auto a = q.Queue(Job{1, nullptr}, true);
  const auto b = q.Queue(Job{2, nullptr}, true);
  const auto c = q.Queue(Job{3, nullptr}, true);
  CHECK(q.Resolve(a, true).state == State::kDropped);
  auto taken = q.Take();
  CHECK(taken && taken->first == b);
  q.Done(b, true, std::string("b"));
  taken = q.Take();
  CHECK(taken && taken->first == c);
  q.Done(c, true, std::string("c"));
  CHECK(q.Resolve(b, true).state == State::kSubmitted);
  CHECK(q.Resolve(c, true).state == State::kSubmitted);

  const auto d = q.Queue(Job{4, nullptr}, true);
  q.Queue(Job{5, nullptr}, true);
  CHECK(q.Resolve(d, true).state == State::kDropped);
}

/* At present, without waiting: a queued droppable job is dropped then and
 * there; one that may not be dropped is waited for. */
void TestResolveWithoutWaiting() {
  Queue q;
  const auto id = q.Queue(Job{1, nullptr}, true);
  CHECK(q.Resolve(id, false).state == State::kDropped);
  CHECK(q.waiting() == 0);

  const auto kept = q.Queue(Job{2, nullptr}, false);
  std::thread worker([&q] {
    auto taken = q.Take();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.Done(taken->first, true, std::string("late"));
  });
  const auto out = q.Resolve(kept, false);
  CHECK(out.state == State::kSubmitted);
  CHECK(out.result && *out.result == "late");
  worker.join();
}

/* A refusal comes back as such; a cancelled job is never taken, and one
 * cancelled while taken is forgotten when it is done. */
void TestRefusedAndCancelled() {
  Queue q;
  const auto refused = q.Queue(Job{1, nullptr}, false);
  auto taken = q.Take();
  q.Done(taken->first, false, std::nullopt);
  CHECK(q.Resolve(refused, true).state == State::kRefused);

  const auto gone = q.Queue(Job{2, nullptr}, false);
  q.Cancel(gone);
  const auto next = q.Queue(Job{3, nullptr}, false);
  taken = q.Take();
  CHECK(taken && taken->first == next);
  q.Cancel(next);
  q.Done(next, true, std::string("unwanted"));
  CHECK(q.Resolve(next, true).state == State::kCancelled);
}

/* Stopping answers the worker and whoever waits, and lets jobs go. */
void TestStop() {
  Queue q;
  auto held = std::make_shared<int>(0);
  const auto id = q.Queue(Job{1, held}, false);
  std::thread waiter([&q, id] {
    CHECK(q.Resolve(id, true).state == State::kCancelled);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  q.Stop();
  waiter.join();
  CHECK(held.use_count() == 1);
  CHECK(!q.Take().has_value());
}

/* A worker and a planner racing over many frames: every frame gets an
 * answer, nothing is answered twice, and drops never come two in a row
 * between submissions. */
void TestRace() {
  Queue q;
  std::atomic<int> submitted{0};
  std::thread worker([&] {
    for (;;) {
      auto taken = q.Take();
      if (!taken)
        return;
      std::this_thread::sleep_for(std::chrono::microseconds(300));
      q.Done(taken->first, true, std::to_string(taken->second.frame));
      submitted++;
    }
  });
  int dropped = 0;
  int answered = 0;
  for (int frame = 0; frame < 300; ++frame) {
    const auto id = q.Queue(Job{frame, nullptr}, true);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    const auto out = q.Resolve(id, frame % 2 == 0);
    answered++;
    if (out.state == State::kDropped)
      dropped++;
    else
      CHECK(out.state == State::kSubmitted &&
            *out.result == std::to_string(frame));
  }
  q.Stop();
  worker.join();
  CHECK(answered == 300);
  CHECK(dropped + submitted.load() >= 300);
  CHECK(static_cast<int>(q.dropped()) == dropped);
}

}  // namespace

int main() {
  TestOrdinary();
  TestLatestWins();
  TestNotTwiceInARow();
  TestResolveWithoutWaiting();
  TestRefusedAndCancelled();
  TestStop();
  TestRace();
  if (failures == 0)
    std::printf("job_queue_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
