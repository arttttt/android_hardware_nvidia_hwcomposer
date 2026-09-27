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

#include "governor/Mailbox.h"

#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "governor/Clock.h"

namespace android::hwc::governor::tegra {

Mailbox::~Mailbox() {
  Discard();
  if (event_fd_ >= 0)
    close(event_fd_);
}

bool Mailbox::Open() {
  event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  return event_fd_ >= 0;
}

void Mailbox::Ring() {
  const uint64_t one = 1;
  /* A full counter means an unread ring is already there. */
  if (write(event_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN) {
    /* Nothing to do about it here; the thread's timers still fire. */
  }
}

void Mailbox::Plan(const Frame &frame) {
  Planned planned;
  planned.frame = frame;
  if (frame.members != nullptr && frame.member_count > 0)
    planned.members.assign(frame.members, frame.members + frame.member_count);
  planned.frame.members = nullptr;
  planned.frame.member_count = 0;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(planned);
  }
  Ring();
}

void Mailbox::Submit(uint64_t seq, int fence_fd) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    submitted_.push_back(Watched{fence_fd, seq, NowNs()});
  }
  Ring();
}

void Mailbox::SetPower(Power mode) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    power_ = mode;
    /* A plan for a display going dark is a plan for nothing. */
    if (mode != Power::on)
      pending_.reset();
  }
  Ring();
}

void Mailbox::Stop() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  Ring();
}

Mailbox::Contents Mailbox::Take(bool rung) {
  Contents contents;
  const std::lock_guard<std::mutex> lock(mutex_);
  if (rung) {
    uint64_t drained = 0;
    if (read(event_fd_, &drained, sizeof(drained)) < 0) {
      /* EAGAIN: rung and drained by an earlier take. Nothing lost. */
    }
  }
  contents.planned = std::move(pending_);
  pending_.reset();
  contents.submitted.swap(submitted_);
  contents.power = power_;
  contents.stop = stop_;
  return contents;
}

bool Mailbox::HasSubmitted() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return !submitted_.empty();
}

void Mailbox::Discard() {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const Watched &w : submitted_)
    if (w.fd >= 0)
      close(w.fd);
  submitted_.clear();
  pending_.reset();
}

}  // namespace android::hwc::governor::tegra
