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

#include <atomic>
#include <thread>
#include <vector>

#include "governor/CpuFloor.h"
#include "governor/EngineClock.h"
#include "governor/FenceWatch.h"
#include "governor/HwcGovernor.h"
#include "governor/Mailbox.h"
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
 * saving power; the tuning (Tuning) holds the numbers; the mailbox
 * (Mailbox) is where the composer's threads leave their news, and the
 * fence watch (FenceWatch) is how merges in flight are followed. This class
 * is the thread that joins them and the timers that let the floor go.
 *
 * Everything that may sleep -- the clock request, the warm-up pass, the
 * processor floor -- runs on this thread. The thread waits in one place,
 * poll(), on the mailbox's descriptor and on the fences, so a merge
 * finishing and a frame being planned are the same kind of wake-up.
 */

namespace android::hwc::governor::tegra {

class TegraGovernor final : public Governor {
 public:
  explicit TegraGovernor(GovernorHost &host);
  ~TegraGovernor() override;

  TegraGovernor(const TegraGovernor &) = delete;
  TegraGovernor &operator=(const TegraGovernor &) = delete;

  /* Reads the tuning, opens the kernel's doors and starts the thread.
   * False only if the thread would have nothing to wait on. */
  bool Start();

  /* Governor: the composer's side. Each leaves its news in the mailbox
   * and returns. */
  void FramePlanned(const Frame &frame) override;
  void MergeSubmitted(uint64_t seq, int merge_fence_fd) override;
  void FramePresented(uint64_t seq) override;
  void PowerMode(Power mode) override;

 private:
  void ThreadFn();
  int TimeoutMs(int64_t now) const;
  void Decide(const Planned &planned, int64_t now);
  void Trace(const MergeEstimate &estimate, bool cold, int profile);
  void Warm(int64_t now);
  void KeepFloorFor(int64_t now);
  void JudgeRelease(int64_t now);
  void DropCpu();
  void DropEverything();

  GovernorHost &host_;
  Tuning tuning_;

  /* Set by the thread on its way out after a failure: from then on plans
   * are ignored and fences are closed on arrival, since nobody would
   * take them out of the mailbox. */
  std::atomic<bool> dead_{false};

  Mailbox mailbox_;
  EngineClock engine_;
  CpuFloor cpu_;
  PerfProfile profile_;

  std::thread thread_;

  /* Thread-only state from here down. */
  FenceWatch fences_;
  int64_t release_due_ns_ = 0;  /* when the floor may go, nought if not yet */
  int64_t orphan_due_ns_ = 0;   /* floor raised, no merge reported by then */
  int64_t cpu_until_ns_ = 0;    /* processor lifted until, nought if not */
  int64_t last_warm_ns_ = 0;
  bool warm_refused_logged_ = false;
};

}  // namespace android::hwc::governor::tegra
