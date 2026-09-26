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

#include "governor/TegraGovernor.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <android/log.h>

namespace android::hwc::governor::tegra {

namespace {

int64_t NowNs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * nsPerSec + int64_t(ts.tv_nsec);
}

void CloseAll(const std::vector<Watched> &fences) {
  for (const Watched &w : fences)
    if (w.fd >= 0)
      close(w.fd);
}

}  // namespace

TegraGovernor::TegraGovernor(GovernorHost &host)
    : host_(host), engine_(host), cpu_(host) {
}

bool TegraGovernor::Start() {
  if (ReadTuningFile(tuningPath, &tuning_, host_))
    host_.Log(ANDROID_LOG_INFO, "tuning read from the device");

  event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (event_fd_ < 0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "eventfd: %s", strerror(errno));
    host_.Log(ANDROID_LOG_ERROR, msg);
    return false;
  }

  if (!engine_.Open())
    host_.Log(ANDROID_LOG_ERROR, "no engine clock; running as a witness");
  cpu_.Open();

  thread_ = std::thread(&TegraGovernor::ThreadFn, this);
  return true;
}

TegraGovernor::~TegraGovernor() {
  {
    const std::lock_guard<std::mutex> lock(mailbox_mutex_);
    stop_ = true;
  }
  Ring();
  if (thread_.joinable())
    thread_.join();

  /* Whatever arrived after the thread emptied the mailbox for the last
   * time is ours to close. */
  CloseAll(submitted_);
  submitted_.clear();

  if (event_fd_ >= 0)
    close(event_fd_);
}

void TegraGovernor::Ring() {
  const uint64_t one = 1;
  if (write(event_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN)
    host_.Log(ANDROID_LOG_WARN, "eventfd write failed");
}

/* The composer's side. */

void TegraGovernor::FramePlanned(const Frame &frame) {
  Planned planned;
  planned.frame = frame;
  if (frame.members != nullptr && frame.member_count > 0)
    planned.members.assign(frame.members, frame.members + frame.member_count);
  planned.frame.members = nullptr;
  planned.frame.member_count = 0;

  {
    const std::lock_guard<std::mutex> lock(mailbox_mutex_);
    pending_ = std::move(planned);
  }
  Ring();
}

void TegraGovernor::MergeSubmitted(uint64_t seq, int merge_fence_fd) {
  {
    const std::lock_guard<std::mutex> lock(mailbox_mutex_);
    submitted_.push_back(Watched{merge_fence_fd, seq, 0});
  }
  Ring();
}

void TegraGovernor::FramePresented(uint64_t /*seq*/) {
  /* Nothing to do with it in this version: the merge's own fence says
   * when the engine is done, and the submit says when the processor may
   * be let go. */
}

void TegraGovernor::PowerMode(Power mode) {
  {
    const std::lock_guard<std::mutex> lock(mailbox_mutex_);
    power_ = mode;
    if (mode != Power::on)
      pending_.reset();
  }
  Ring();
}

/* The thread. */

TegraGovernor::Mail TegraGovernor::TakeMail(bool rung) {
  Mail mail;
  const std::lock_guard<std::mutex> lock(mailbox_mutex_);
  if (rung) {
    uint64_t drained = 0;
    if (read(event_fd_, &drained, sizeof(drained)) < 0 && errno != EAGAIN)
      host_.Log(ANDROID_LOG_WARN, "eventfd read failed");
  }
  mail.planned = std::move(pending_);
  pending_.reset();
  mail.submitted.swap(submitted_);
  mail.power = power_;
  mail.stop = stop_;
  return mail;
}

int TegraGovernor::TimeoutMs(int64_t now) const {
  int64_t due = -1;
  auto consider = [&due](int64_t when) {
    if (when > 0 && (due < 0 || when < due))
      due = when;
  };
  consider(cpu_until_ns_);
  consider(release_due_ns_);
  consider(orphan_due_ns_);
  for (const Watched &w : watched_)
    consider(w.since_ns + int64_t(tuning_.fence_patience_ms) * nsPerMs);

  if (due < 0)
    return -1;
  if (due <= now)
    return 0;
  return int((due - now + nsPerMs - 1) / nsPerMs);
}

void TegraGovernor::ThreadFn() {
  pthread_setname_np(pthread_self(), "hwc-governor");

  std::vector<struct pollfd> fds;
  for (;;) {
    /* The poll set: the eventfd first, then the watched fences in the
     * list's order -- which is what JudgeFences relies on. */
    fds.clear();
    fds.push_back(pollfd{event_fd_, POLLIN, 0});
    for (const Watched &w : watched_)
      fds.push_back(pollfd{w.fd, POLLIN, 0});

    if (poll(fds.data(), fds.size(), TimeoutMs(NowNs())) < 0 &&
        errno != EINTR) {
      host_.Log(ANDROID_LOG_ERROR, "poll failed; the governor stops");
      break;
    }
    const int64_t now = NowNs();

    Mail mail = TakeMail((fds[0].revents & POLLIN) != 0);
    if (mail.stop) {
      CloseAll(mail.submitted);
      DropEverything();
      break;
    }

    if (mail.power != Power::on) {
      /* A display going dark drops everything: no merge is coming, and
       * a floor left standing would hold the memory clock up through the
       * doze. */
      CloseAll(mail.submitted);
      DropEverything();
      continue;
    }

    /* Merges that went to the engine: the processor has done its part,
     * the fence is now what says when the engine has done its. */
    if (!mail.submitted.empty()) {
      orphan_due_ns_ = 0;
      cpu_.Drop();
      cpu_until_ns_ = 0;
    }

    JudgeFences(fds, now);

    /* On the list after this round's fences were judged, so the poll set
     * built before the sleep still matched the head of the list. */
    for (Watched &w : mail.submitted) {
      if (w.fd < 0)
        continue;
      w.since_ns = now;
      watched_.push_back(w);
    }

    if (mail.planned)
      Decide(*mail.planned, now);

    JudgeRelease(now);

    if (cpu_until_ns_ != 0 && now >= cpu_until_ns_) {
      cpu_.Drop();
      cpu_until_ns_ = 0;
    }
  }
}

void TegraGovernor::JudgeFences(const std::vector<struct pollfd> &fds,
                                int64_t now) {
  bool any_done = false;
  std::vector<Watched> kept;
  for (size_t i = 0; i < watched_.size(); ++i) {
    const bool polled = i + 1 < fds.size();
    const bool signaled =
        polled &&
        (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
    const bool given_up = now - watched_[i].since_ns >=
                          int64_t(tuning_.fence_patience_ms) * nsPerMs;
    if (signaled || given_up) {
      close(watched_[i].fd);
      any_done = true;
    } else {
      kept.push_back(watched_[i]);
    }
  }
  watched_.swap(kept);

  /* The floor outlives the last fence by the hold -- unless a frame has
   * been planned whose merge has not reported yet: that merge is what the
   * floor stands for now, and the orphan timer covers it instead. */
  if (any_done && watched_.empty() && engine_.floor_mhz() != 0 &&
      orphan_due_ns_ == 0)
    release_due_ns_ = now + int64_t(tuning_.hold_ms) * nsPerMs;
}

void TegraGovernor::JudgeRelease(int64_t now) {
  if (engine_.floor_mhz() == 0 || !watched_.empty())
    return;
  const bool held_long_enough = release_due_ns_ != 0 && now >= release_due_ns_;
  const bool orphaned = orphan_due_ns_ != 0 && now >= orphan_due_ns_;
  if (held_long_enough || orphaned) {
    release_due_ns_ = 0;
    orphan_due_ns_ = 0;
    engine_.Release();
  }
}

void TegraGovernor::KeepFloorFor(int64_t now) {
  release_due_ns_ = 0;
  orphan_due_ns_ = now + int64_t(tuning_.orphan_ms) * nsPerMs;
}

void TegraGovernor::Trace(const MergeEstimate &e, bool cold, int profile) {
  host_.TraceInt("hwc_gov_est_kcycles", int32_t(e.cycles / 1000.0));
  host_.TraceInt("hwc_gov_budget_us", int32_t(e.budget_ns / nsPerUs));
  host_.TraceInt("hwc_gov_need_mhz", int32_t(e.need_mhz));
  host_.TraceInt("hwc_gov_step_mhz", int32_t(e.step_mhz));
  host_.TraceInt("hwc_gov_cold", cold ? 1 : 0);
  host_.TraceInt("hwc_gov_late", (e.late || e.beyond) ? 1 : 0);
  host_.TraceInt("hwc_gov_profile", profile);
}

void TegraGovernor::Decide(const Planned &planned, int64_t now) {
  const Frame &f = planned.frame;

  if (!f.merge_planned || f.merge_reuse_predicted) {
    host_.TraceInt("hwc_gov_need_mhz", 0);
    return;
  }

  const int profile =
      profile_.Read(now, int64_t(tuning_.profile_poll_ms) * nsPerMs);
  const bool power_save = profile == PerfProfile::powerSave;
  const bool cold = EngineCold(f, last_warm_ns_, now, tuning_);
  const bool lift_cpu = cold && f.previous_flip_landed && !power_save &&
                        cpu_.available() && tuning_.cpu_khz != 0 &&
                        engine_.usable();

  const MergeEstimate estimate =
      EstimateMerge(f, planned.members.data(), planned.members.size(), now,
                    tuning_, cold, lift_cpu);
  Trace(estimate, cold, profile);

  if (power_save) {
    /* The one thing allowed here: a cold engine is woken, nothing is
     * raised. */
    if (cold)
      Warm(now);
    return;
  }

  if (cold) {
    if (lift_cpu && cpu_.Lift(tuning_.cpu_khz))
      cpu_until_ns_ = now + int64_t(tuning_.cpu_cap_ms) * nsPerMs;
    /* Before the floor, and it has to be: a floor filed against a
     * powered-down engine is applied as it comes up, and coming up
     * straight onto the top step cost tens of milliseconds of sleeping in
     * the kernel's voltage scaling. The warm-up comes up at whatever the
     * engine idles at; the floor follows once it is awake. */
    Warm(now);
  }

  if (!engine_.usable())
    return;

  /* Only upward from what the engine is already doing, by devfreq's doing
   * or by a floor still standing from the previous merge. A standing
   * floor is kept standing: this merge's fence will extend it. */
  const uint32_t have =
      std::max(engine_.CurrentMhz().value_or(0), engine_.floor_mhz());
  if (estimate.step_mhz <= have) {
    if (engine_.floor_mhz() != 0)
      KeepFloorFor(now);
    return;
  }

  if (engine_.SetFloor(estimate.step_mhz))
    KeepFloorFor(now);
}

void TegraGovernor::Warm(int64_t now) {
  const int fd = host_.WarmEngine();
  if (fd < 0) {
    if (!warm_refused_logged_) {
      host_.Log(ANDROID_LOG_WARN, "the engine would not take the warm-up pass");
      warm_refused_logged_ = true;
    }
    return;
  }
  /* Not waited for and not watched: the submit itself is what brought the
   * engine up, and the merge queues behind it on the same channel. */
  close(fd);
  last_warm_ns_ = now;
  host_.TraceInt("hwc_gov_warm", 1);
  host_.TraceInt("hwc_gov_warm", 0);
}

void TegraGovernor::DropEverything() {
  CloseAll(watched_);
  watched_.clear();
  release_due_ns_ = 0;
  orphan_due_ns_ = 0;
  engine_.Release();
  cpu_.Drop();
  cpu_until_ns_ = 0;
}

}  // namespace android::hwc::governor::tegra

/* The library's entry points. */

using android::hwc::governor::Governor;
using android::hwc::governor::GovernorHost;
using android::hwc::governor::tegra::TegraGovernor;

extern "C" uint32_t hwc_governor_api_version() {
  return android::hwc::governor::apiVersion;
}

extern "C" Governor *hwc_governor_create(GovernorHost *host) {
  if (host == nullptr)
    return nullptr;
  auto *governor = new TegraGovernor(*host);
  if (!governor->Start()) {
    delete governor;
    return nullptr;
  }
  return governor;
}

extern "C" void hwc_governor_destroy(Governor *governor) {
  delete governor;
}
