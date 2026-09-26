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

#include <stdint.h>

#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "governor/CpuFloor.h"
#include "governor/EngineClock.h"
#include "governor/HwcGovernor.h"
#include "governor/MergePlan.h"
#include "governor/PerfProfile.h"
#include "governor/Tuning.h"

/* The composition load governor for Tegra K1: hwcgovernor.tegra.so.
 *
 * What it decides, once per planned merge: whether to wake a powered-down
 * engine ahead of the merge, what clock floor to ask of the engine for the
 * merge's duration, and whether to lift the processor's floor for the few
 * milliseconds between the plan and the submit. The kernel does the rest --
 * the memory clock follows the engine's floor, and the display's isochronous
 * share follows the engine being on.
 *
 * The parts: the model (MergePlan) turns a snapshot into a step; the engine
 * clock (EngineClock) and the processor floor (CpuFloor) are the two doors
 * into the kernel; the profile (PerfProfile) says whether the device is
 * saving power; the tuning (Tuning) holds the numbers. This class is the
 * thread that joins them: it keeps the mailbox the composer drops snapshots
 * in, the fences of merges in flight, and the timers that let the floor go.
 *
 * Everything that may sleep -- the clock request, the warm-up pass, the
 * processor floor -- runs on this thread. The composer's threads only drop
 * a snapshot in the mailbox and ring an eventfd. The thread waits in one
 * place, poll(), on that eventfd and on the fences of merges in flight, so
 * a merge finishing and a frame being planned are the same kind of wake-up.
 */

namespace android::hwc::governor::tegra {

/* A snapshot as the thread keeps it: the frame with its members copied
 * alongside, since the frame's own pointer dies with the call. */
struct Planned {
  Frame frame;
  std::vector<Member> members;
};

/* A merge fence the thread is watching. */
struct Watched {
  int fd;
  uint64_t seq;
  int64_t since_ns;
};

class TegraGovernor final : public Governor {
 public:
  explicit TegraGovernor(GovernorHost &host);
  ~TegraGovernor() override;

  TegraGovernor(const TegraGovernor &) = delete;
  TegraGovernor &operator=(const TegraGovernor &) = delete;

  /* Reads the tuning, opens the kernel's doors and starts the thread.
   * False only if the thread has nothing to wait on. */
  bool Start();

  /* Governor: the composer's side. Each copies, drops in the mailbox,
   * rings and returns. */
  void FramePlanned(const Frame &frame) override;
  void MergeSubmitted(uint64_t seq, int merge_fence_fd) override;
  void FramePresented(uint64_t seq) override;
  void PowerMode(Power mode) override;

 private:
  /* What the thread takes out of the mailbox in one go. */
  struct Mail {
    std::optional<Planned> planned;
    std::vector<Watched> submitted;
    Power power;
    bool stop;
  };

  void Ring();
  void ThreadFn();
  Mail TakeMail(bool rung);
  int TimeoutMs(int64_t now) const;
  void JudgeFences(const std::vector<struct pollfd> &fds, int64_t now);
  void JudgeRelease(int64_t now);

  void Decide(const Planned &planned, int64_t now);
  void Trace(const MergeEstimate &estimate, bool cold, int profile);
  void Warm(int64_t now);
  void KeepFloorFor(int64_t now);
  void DropEverything();

  GovernorHost &host_;
  Tuning tuning_;
  EngineClock engine_;
  CpuFloor cpu_;
  PerfProfile profile_;

  int event_fd_ = -1;

  /* The mailbox. Written by the composer's threads, emptied by ours. */
  std::mutex mailbox_mutex_;
  std::optional<Planned> pending_;
  std::vector<Watched> submitted_;
  Power power_ = Power::on;
  bool stop_ = false;

  std::thread thread_;

  /* Thread-only state from here down. */
  std::vector<Watched> watched_;
  int64_t release_due_ns_ = 0;  /* when the floor may go, nought if not yet */
  int64_t orphan_due_ns_ = 0;   /* floor raised, no merge reported by then */
  int64_t cpu_until_ns_ = 0;    /* processor lifted until, nought if not */
  int64_t last_warm_ns_ = 0;
  bool warm_refused_logged_ = false;
};

}  // namespace android::hwc::governor::tegra
