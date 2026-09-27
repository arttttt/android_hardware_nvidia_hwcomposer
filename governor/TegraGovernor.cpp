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
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <android/log.h>

#include "governor/Log.h"

namespace android::hwc::governor::tegra {

namespace {

int64_t NowNs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * nsPerSec + int64_t(ts.tv_nsec);
}

}  // namespace

TegraGovernor::TegraGovernor(GovernorHost &host)
    : host_(host), engine_(host), cpu_(host) {
}

bool TegraGovernor::Start() {
  if (ReadTuningFile(tuningPath, &tuning_, host_))
    host_.Log(ANDROID_LOG_INFO, "tuning read from the device");

  if (!mailbox_.Open()) {
    LogErrno(host_, ANDROID_LOG_ERROR, "eventfd", errno);
    return false;
  }

  if (!engine_.Open())
    host_.Log(ANDROID_LOG_ERROR, "no engine clock; running as a witness");
  cpu_.Open();

  thread_ = std::thread(&TegraGovernor::ThreadFn, this);
  return true;
}

TegraGovernor::~TegraGovernor() {
  mailbox_.Stop();
  if (thread_.joinable())
    thread_.join();
  /* Whatever arrived after the thread's last take is the mailbox's to
   * close, on its way out. */
}

/* The composer's side. */

void TegraGovernor::FramePlanned(const Frame &frame) {
  if (dead_.load())
    return;
  mailbox_.Plan(frame);
}

void TegraGovernor::MergeSubmitted(uint64_t seq, int merge_fence_fd) {
  if (dead_.load()) {
    if (merge_fence_fd >= 0)
      close(merge_fence_fd);
    return;
  }
  mailbox_.Submit(seq, merge_fence_fd);
}

void TegraGovernor::FramePresented(uint64_t /*seq*/) {
  /* Nothing to do with it in this version: the merge's own fence says
   * when the engine is done, and the submit says when the processor may
   * be let go. */
}

void TegraGovernor::PowerMode(Power mode) {
  mailbox_.SetPower(mode);
}

/* The thread. */

int TegraGovernor::TimeoutMs(int64_t now) const {
  int64_t due = 0;
  auto consider = [&due](int64_t when) {
    if (when > 0 && (due == 0 || when < due))
      due = when;
  };
  consider(cpu_until_ns_);
  consider(release_due_ns_);
  consider(orphan_due_ns_);
  consider(fences_.NextGiveUpNs(int64_t(tuning_.fence_patience_ms) * nsPerMs));

  if (due == 0)
    return -1;
  if (due <= now)
    return 0;
  return int((due - now + nsPerMs - 1) / nsPerMs);
}

void TegraGovernor::ThreadFn() {
  pthread_setname_np(pthread_self(), "hwc-governor");

  /* Real-time, like the composer's own binder threads: a plan is worth
   * acting on within the millisecond and a half before the composer's
   * submit, and a fair-share thread in a busy transition waited two to
   * four milliseconds for a processor -- and decided after the merge had
   * gone. The work is short and the sleeps are in the kernel's own
   * scaling; the priority is the lowest real-time one. */
  struct sched_param param = {};
  param.sched_priority = 2;
  if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
    LogErrno(host_, ANDROID_LOG_WARN, "real-time priority", errno);

  std::vector<struct pollfd> fds;
  for (;;) {
    /* The poll set: the mailbox first, then the fences in their order. */
    fds.clear();
    fds.push_back(pollfd{mailbox_.fd(), POLLIN, 0});
    fences_.AppendPollSet(&fds);

    if (poll(fds.data(), fds.size(), TimeoutMs(NowNs())) < 0 &&
        errno != EINTR) {
      /* Nothing stands after the thread: the floors go, and what the
       * composer sends from now on is closed at the door. */
      LogErrno(host_, ANDROID_LOG_ERROR, "poll; the governor stops", errno);
      dead_.store(true);
      DropEverything();
      break;
    }
    const int64_t now = NowNs();

    Mailbox::Contents mail = mailbox_.Take((fds[0].revents & POLLIN) != 0);
    if (mail.stop || mail.power != Power::on) {
      /* Stopping, or a display going dark, drops everything: no merge is
       * coming, and a floor left standing would hold the memory clock up
       * through the doze. The fences just taken are ours to close, and
       * are closed unwatched. */
      for (const Watched &w : mail.submitted)
        if (w.fd >= 0)
          close(w.fd);
      DropEverything();
      if (mail.stop)
        break;
      continue;
    }

    /* Merges that went to the engine: the processor has done its part,
     * the fence is now what says when the engine has done its. */
    if (!mail.submitted.empty()) {
      orphan_due_ns_ = 0;
      DropCpu();
    }

    /* Judged against the slots built before the sleep; only then are the
     * merges reported this round added, after those slots. */
    const bool any_done =
        fences_.Judge(fds, 1, now, int64_t(tuning_.fence_patience_ms) * nsPerMs);
    for (const Watched &w : mail.submitted)
      fences_.Add(w.fd, w.seq, now);

    /* The floor outlives the last fence by the hold -- unless a frame has
     * been planned whose merge has not reported yet: that merge is what
     * the floor stands for now, and the orphan timer covers it instead. */
    if (any_done && fences_.empty() && engine_.floor_mhz() != 0 &&
        orphan_due_ns_ == 0)
      release_due_ns_ = now + int64_t(tuning_.hold_ms) * nsPerMs;

    if (mail.planned) {
      const uint64_t seq = mail.planned->frame.seq;
      bool reported = false;
      for (const Watched &w : mail.submitted)
        reported = reported || w.seq >= seq;
      Decide(*mail.planned, now, reported);
    }

    JudgeRelease(now);

    if (cpu_until_ns_ != 0 && now >= cpu_until_ns_)
      DropCpu();
  }
}

void TegraGovernor::JudgeRelease(int64_t now) {
  if (engine_.floor_mhz() == 0 || !fences_.empty())
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

void TegraGovernor::DropCpu() {
  cpu_until_ns_ = 0;
  cpu_.Drop();
}

void TegraGovernor::Trace(const MergeEstimate &e, bool cold, int profile,
                          int64_t lag_ns) {
  host_.TraceInt("hwc_gov_lag_us", int32_t(lag_ns / nsPerUs));
  host_.TraceInt("hwc_gov_est_kcycles", int32_t(e.cycles / 1000.0));
  host_.TraceInt("hwc_gov_budget_us", int32_t(e.budget_ns / nsPerUs));
  host_.TraceInt("hwc_gov_need_mhz", int32_t(e.need_mhz));
  host_.TraceInt("hwc_gov_step_mhz", int32_t(e.step_mhz));
  host_.TraceInt("hwc_gov_cold", cold ? 1 : 0);
  host_.TraceInt("hwc_gov_late", (e.late || e.beyond) ? 1 : 0);
  host_.TraceInt("hwc_gov_slip", e.slipped ? 1 : 0);
  host_.TraceInt("hwc_gov_profile", profile);
}

void TegraGovernor::Decide(const Planned &planned, int64_t now,
                           bool merge_reported) {
  const Frame &f = planned.frame;

  if (!f.merge_planned || f.merge_reuse_predicted) {
    host_.TraceInt("hwc_gov_need_mhz", 0);
    return;
  }

  const int profile =
      profile_.Read(now, int64_t(tuning_.profile_poll_ms) * nsPerMs);
  const bool power_save = profile == PerfProfile::powerSave;
  const bool cold = EngineCold(f, last_warm_ns_, now, tuning_);
  /* Lifting the processor is for the composer's submit; once the submit
   * has happened there is nothing left to lift it for. */
  const bool lift_cpu = cold && !merge_reported && f.previous_flip_landed &&
                        !power_save && cpu_.available() &&
                        tuning_.cpu_khz != 0 && engine_.usable();

  const MergeEstimate estimate =
      EstimateMerge(f, planned.members.data(), planned.members.size(), now,
                    tuning_, cold, lift_cpu);
  Trace(estimate, cold, profile, now - f.now_ns);

  if (power_save) {
    /* The one thing allowed here: a cold engine is woken, nothing is
     * raised. */
    if (cold && !merge_reported)
      Warm(now);
    return;
  }

  /* Before the floor, and it has to be: a floor filed against a
   * powered-down engine is applied as it comes up, and coming up straight
   * onto the top step cost tens of milliseconds of sleeping in the
   * kernel's voltage scaling. The warm-up comes up at whatever the engine
   * idles at; the floor follows once it is awake. A merge already sent
   * has woken the engine itself. */
  if (cold && !merge_reported)
    Warm(now);

  /* Only upward from what the engine is already doing, by devfreq's doing
   * or by a floor still standing from the previous merge. A standing
   * floor is kept standing: this merge's fence will extend it. */
  if (engine_.usable()) {
    const uint32_t have =
        std::max(engine_.CurrentMhz().value_or(0), engine_.floor_mhz());
    if (estimate.step_mhz > have) {
      if (engine_.SetFloor(estimate.step_mhz))
        KeepFloorFor(now);
    } else if (engine_.floor_mhz() != 0) {
      KeepFloorFor(now);
    }
  }

  /* Last, and only if the submit is still ahead: the request runs the
   * processor's whole scaling step on this thread -- three milliseconds
   * when the processor was idling -- and put first it held the warm-up and
   * the floor behind it until the composer had submitted anyway. */
  if (lift_cpu && !mailbox_.HasSubmitted() && cpu_.Lift(tuning_.cpu_khz))
    cpu_until_ns_ = now + int64_t(tuning_.cpu_cap_ms) * nsPerMs;
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
  fences_.DropAll();
  release_due_ns_ = 0;
  orphan_due_ns_ = 0;
  engine_.Release();
  DropCpu();
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
