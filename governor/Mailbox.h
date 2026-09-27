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

#include <mutex>
#include <optional>
#include <vector>

#include "governor/FenceWatch.h"
#include "governor/HwcGovernor.h"

namespace android::hwc::governor::tegra {

/* A snapshot as the thread keeps it: the frame with its members copied
 * alongside, since the frame's own pointer dies with the call. */
struct Planned {
  Frame frame;
  std::vector<Member> members;
};

/* What the composer's threads leave for the governor's thread.
 *
 * Snapshots replace each other -- the last plan is the one that will be
 * shown -- while merges and the power mode accumulate. Every deposit
 * rings an eventfd, which is how the thread, waiting in poll() on that
 * descriptor and the fences, learns there is mail. */
class Mailbox {
 public:
  struct Contents {
    std::optional<Planned> planned;
    std::vector<Watched> submitted;  /* since_ns not yet set */
    Power power;
    bool stop;
  };

  Mailbox() = default;
  ~Mailbox();

  Mailbox(const Mailbox &) = delete;
  Mailbox &operator=(const Mailbox &) = delete;

  /* False, if there is nothing to ring. */
  bool Open();

  /* The descriptor to poll for mail. */
  int fd() const { return event_fd_; }

  void Plan(const Frame &frame);
  void Submit(uint64_t seq, int fence_fd);
  void SetPower(Power mode);
  void Stop();

  /* Empties the box. `rung` says the descriptor reported readable and is
   * to be drained. */
  Contents Take(bool rung);

  /* Whether a merge has been reported since the last take. */
  bool HasSubmitted();

  /* Closes what was submitted and never taken. */
  void Discard();

 private:
  void Ring();

  int event_fd_ = -1;
  std::mutex mutex_;
  std::optional<Planned> pending_;
  std::vector<Watched> submitted_;
  Power power_ = Power::on;
  bool stop_ = false;
};

}  // namespace android::hwc::governor::tegra
