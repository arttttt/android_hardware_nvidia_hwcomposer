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

/* An engine's floor lane, checked on a workstation:
 *
 *     c++ -std=c++17 -I. governor/tests/floor_lane_test.cpp \
 *         governor/FloorLane.cpp governor/FenceWatch.cpp \
 *         -o /tmp/floor_lane_test && /tmp/floor_lane_test
 *
 * The engine is a fake that remembers its floor; a fence is the read end
 * of a pipe, and the poll's answer is written into the set by hand. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <map>
#include <string>

#include "governor/FloorLane.h"
#include "governor/HwcGovernor.h"
#include "governor/MergePlan.h"

using namespace android::hwc::governor;
using namespace android::hwc::governor::tegra;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,  \
              #cond);                                                   \
      failures++;                                                       \
    }                                                                   \
  } while (0)

constexpr int64_t t0 = 1000 * nsPerSec;

class FakeHost final : public GovernorHost {
 public:
  int WarmEngine() override { return -1; }
  void TraceInt(const char *name, int32_t value) override {
    traced[name] = value;
  }
  void Log(int, const char *) override {}

  std::map<std::string, int32_t> traced;
};

class FakeEngine final : public FloorActuator {
 public:
  bool usable() const override { return usable_; }
  std::optional<uint32_t> CurrentMhz() override { return current_mhz; }
  bool SetFloor(uint32_t mhz) override {
    floor_ = mhz;
    ++sets;
    return true;
  }
  void Release() override {
    if (floor_ != 0)
      ++releases;
    floor_ = 0;
  }
  uint32_t floor_mhz() const override { return floor_; }

  bool usable_ = true;
  std::optional<uint32_t> current_mhz = 0;
  uint32_t floor_ = 0;
  int sets = 0;
  int releases = 0;
};

/* A fence the test signals by hand: the read end of a pipe. */
int Fence() {
  int p[2];
  if (pipe(p) != 0)
    abort();
  close(p[1]);
  return p[0];
}

bool IsOpen(int fd) { return fcntl(fd, F_GETFD) != -1; }

struct Rig {
  Tuning tuning;
  FakeHost host;
  FakeEngine engine;
  FloorLane lane{engine, tuning, host, "owed"};
  std::vector<struct pollfd> fds;

  /* One round of the thread: the poll set built, `signaled` of its fence
   * slots answered, then the reports taken. */
  void Round(int64_t now, std::vector<Watched> submitted = {},
             bool signal_all = false) {
    fds.clear();
    fds.push_back(pollfd{-1, 0, 0}); /* the mailbox's slot */
    lane.AppendPollSet(&fds);
    if (signal_all)
      for (size_t i = 1; i < fds.size(); ++i)
        fds[i].revents = POLLIN;
    if (!submitted.empty())
      lane.Reported();
    lane.Follow(fds, submitted, now);
    lane.ReleaseIfDue(now);
  }
};

int64_t Ms(int64_t ms) { return ms * nsPerMs; }

void AFloorForWorkThatNeverComesIsOrphaned() {
  Rig r;
  r.lane.Ask(400, 0, false, t0);
  CHECK(r.engine.floor_mhz() == 400);

  r.Round(t0 + Ms(r.tuning.orphan_ms) - 1);
  CHECK(r.engine.floor_mhz() == 400);
  r.Round(t0 + Ms(r.tuning.orphan_ms));
  CHECK(r.engine.floor_mhz() == 0);
  CHECK(r.lane.NextDueNs() == 0);
}

void TheFloorOutlivesTheLastFenceByTheHold() {
  Rig r;
  r.lane.Ask(400, 0, false, t0);
  r.Round(t0 + Ms(2), {Watched{Fence(), 1, 0}});
  CHECK(r.lane.HasReported(1));
  CHECK(!r.lane.HasReported(2));

  /* In flight past the orphan timer: the fence is what holds it now. */
  r.Round(t0 + Ms(r.tuning.orphan_ms) + Ms(5));
  CHECK(r.engine.floor_mhz() == 400);

  const int64_t done = t0 + Ms(r.tuning.orphan_ms) + Ms(6);
  r.Round(done, {}, true);
  CHECK(r.engine.floor_mhz() == 400);
  CHECK(r.lane.NextDueNs() == done + Ms(r.tuning.hold_ms));
  r.Round(done + Ms(r.tuning.hold_ms));
  CHECK(r.engine.floor_mhz() == 0);
}

void WorkPlannedButNotReportedKeepsTheFloorPastAFence() {
  Rig r;
  r.lane.Ask(400, 0, false, t0);
  r.Round(t0 + Ms(1), {Watched{Fence(), 1, 0}});
  /* The next frame's floor asked before the first one's fence is done. */
  r.lane.Ask(400, 0, false, t0 + Ms(10));
  r.Round(t0 + Ms(11), {}, true);
  /* No hold started: the orphan timer covers the second frame. */
  r.Round(t0 + Ms(11) + Ms(r.tuning.hold_ms));
  CHECK(r.engine.floor_mhz() == 400);
  r.Round(t0 + Ms(10) + Ms(r.tuning.orphan_ms));
  CHECK(r.engine.floor_mhz() == 0);
}

void AnOwedStepIsAskedOnTheReportOnlyUpward() {
  Rig r;
  r.lane.Ask(300, 600, false, t0);
  CHECK(r.engine.floor_mhz() == 300);
  CHECK(r.host.traced["owed"] == 600);
  r.Round(t0 + Ms(1), {Watched{-1, 1, 0}});
  CHECK(r.engine.floor_mhz() == 600);
  CHECK(r.host.traced["owed"] == 0);

  /* Owed no more than stands: nothing asked. */
  r.lane.Ask(600, 500, false, t0 + Ms(2));
  const int sets = r.engine.sets;
  r.Round(t0 + Ms(3), {Watched{-1, 2, 0}});
  CHECK(r.engine.sets == sets);

  /* An engine that would not take a request is owed nothing. */
  Rig unusable;
  unusable.engine.usable_ = false;
  unusable.lane.Ask(300, 600, false, t0);
  unusable.engine.floor_ = 0;
  unusable.Round(t0 + Ms(1), {Watched{-1, 1, 0}});
  CHECK(unusable.engine.floor_mhz() == 0);
}

void AnEngineAlreadyFastEnoughIsNotAsked() {
  Rig r;
  r.engine.current_mhz = 500;
  r.lane.Ask(400, 0, false, t0);
  CHECK(r.engine.sets == 0);
  /* Nothing stands, so nothing is timed. */
  CHECK(r.lane.NextDueNs() == 0);

  /* A standing floor is kept standing: its timers move on. */
  r.engine.current_mhz = 0;
  r.lane.Ask(400, 0, false, t0);
  r.lane.Ask(300, 0, true, t0 + Ms(5));
  CHECK(r.engine.floor_mhz() == 400);
  CHECK(r.lane.NextDueNs() == t0 + Ms(5) + Ms(r.tuning.hold_ms));
}

void AFenceNeverSignaledIsGivenUpOn() {
  Rig r;
  const int fd = Fence();
  r.Round(t0, {Watched{fd, 1, 0}});
  /* Reported before the plan was taken: the floor follows the fence. */
  r.lane.Ask(400, 0, true, t0);
  CHECK(r.lane.NextDueNs() == t0 + Ms(r.tuning.fence_patience_ms));
  r.Round(t0 + Ms(r.tuning.fence_patience_ms));
  CHECK(!IsOpen(fd));
  r.Round(t0 + Ms(r.tuning.fence_patience_ms) + Ms(r.tuning.hold_ms));
  CHECK(r.engine.floor_mhz() == 0);
}

void DroppingClosesTheFencesAndLetsTheFloorGo() {
  Rig r;
  r.lane.Ask(400, 600, false, t0);
  const int fd = Fence();
  r.Round(t0 + Ms(1), {Watched{fd, 1, 0}});
  r.lane.DropAll();
  CHECK(!IsOpen(fd));
  CHECK(r.engine.floor_mhz() == 0);
  CHECK(r.lane.NextDueNs() == 0);
}

}  // namespace

int main() {
  AFloorForWorkThatNeverComesIsOrphaned();
  TheFloorOutlivesTheLastFenceByTheHold();
  WorkPlannedButNotReportedKeepsTheFloorPastAFence();
  AnOwedStepIsAskedOnTheReportOnlyUpward();
  AnEngineAlreadyFastEnoughIsNotAsked();
  AFenceNeverSignaledIsGivenUpOn();
  DroppingClosesTheFencesAndLetsTheFloorGo();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("floor_lane_test: all checks passed\n");
  return EXIT_SUCCESS;
}
