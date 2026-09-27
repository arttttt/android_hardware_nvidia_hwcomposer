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

  /* Real-time, one step above the composer's own binder threads and the
   * framework: a plan is worth acting on within the millisecond and a
   * half before the composer's submit. A fair-share thread in a busy
   * transition waited two to four milliseconds for a processor and
   * decided after the merge had gone; at the framework's own priority it
   * still waited up to three milliseconds behind the very validate that
   * rang it. One step above, it runs within a tenth of a millisecond. The
   * work is a few microseconds of arithmetic; the long parts -- the
   * engine's clock request, the warm-up, the processor's scaling step --
   * sleep in the kernel rather than spin. */
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

    /* Merges that went to the engine: the fence is now what says when the
     * engine is done with them -- and the composer is past the submit, so
     * a step withheld from a cold engine can be asked now. */
    if (!mail.submitted.empty()) {
      orphan_due_ns_ = 0;
      DropCpu();
      RaiseDeferred();
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
      /* Reported this round or earlier: either way the merge is gone. */
      const uint64_t seq = mail.planned->frame.seq;
      Decide(*mail.planned, now, fences_.latest_seq() >= seq);
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
    deferred_mhz_ = 0;
    engine_.Release();
  }
}

void TegraGovernor::DropCpu() {
  cpu_until_ns_ = 0;
  cpu_.Drop();
}

void TegraGovernor::RaiseDeferred() {
  const uint32_t step = deferred_mhz_;
  deferred_mhz_ = 0;
  if (step == 0 || !engine_.usable() || step <= engine_.floor_mhz())
    return;
  /* The merge is running at the capped step; the ramp this starts lands
   * partway through it, or before the next one of the transition. */
  engine_.SetFloor(step);
  host_.TraceInt("hwc_gov_deferred_mhz", 0);
}

void TegraGovernor::KeepFloorFor(int64_t now) {
  release_due_ns_ = 0;
  orphan_due_ns_ = now + int64_t(tuning_.orphan_ms) * nsPerMs;
}

void TegraGovernor::FollowFences(int64_t now) {
  orphan_due_ns_ = 0;
  release_due_ns_ =
      fences_.empty() ? now + int64_t(tuning_.hold_ms) * nsPerMs : 0;
}

void TegraGovernor::Trace(const MergeEstimate &e, bool cold, int profile,
                          int64_t lag_ns, int64_t now_ns) {
  host_.TraceInt("hwc_gov_lag_us", int32_t(lag_ns / nsPerUs));
  host_.TraceInt("hwc_gov_est_kcycles", int32_t(e.cycles / 1000.0));
  host_.TraceInt("hwc_gov_deadline_us", int32_t((e.deadline_ns - now_ns) / nsPerUs));
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
  /* The first merge after a pause finds the processor idled down, cold
   * engine or not; the pause is read from the engine's last use. */
  const bool paused =
      tuning_.cpu_pause_ms != 0 && f.last_engine_use_ns != 0 &&
      now - f.last_engine_use_ns >= int64_t(tuning_.cpu_pause_ms) * nsPerMs;
  host_.TraceInt("hwc_gov_paused", paused ? 1 : 0);
  /* Lifting the processor is for the composer's submit; once the submit
   * has happened there is nothing left to lift it for. */
  const bool lift_cpu = (cold || paused) && !merge_reported &&
                        f.previous_flip_landed && !power_save &&
                        cpu_.available() && tuning_.cpu_khz != 0 &&
                        engine_.usable();

  /* An idling processor first: the engine's floor ramps the rail by I2C
   * writes served by interrupts, and at its lowest clock the processor
   * takes milliseconds to get to each -- longer than the lift, memory
   * clock and all. Above the threshold the lift goes last, where it holds
   * nothing up. A warm engine has no rail to ramp, so nothing is held up
   * by lifting first, and the processor is lifted at once whenever it is
   * below the step. */
  bool lifted = false;
  const uint32_t cpu_khz = lift_cpu ? cpu_.CurrentKhz() : 0;
  if (lift_cpu &&
      (cpu_khz <= tuning_.cpu_low_khz || (!cold && cpu_khz < tuning_.cpu_khz))) {
    lifted = cpu_.Lift(tuning_.cpu_khz);
    if (lifted)
      cpu_until_ns_ = now + int64_t(tuning_.cpu_cap_ms) * nsPerMs;
  }
  host_.TraceInt("hwc_gov_cpu_first", lifted ? 1 : 0);

  const MergeEstimate estimate =
      EstimateMerge(f, planned.members.data(), planned.members.size(), now,
                    tuning_, cold, lift_cpu);
  Trace(estimate, cold, profile, now - f.now_ns, now);

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

  if (!engine_.usable())
    return;

  /* A cold engine with the submit still ahead is asked no more than the
   * cap now: a higher step ramps the core rail with the bus clock's lock
   * held, and the composer's submit would wait behind it for
   * milliseconds. The rest is owed, and asked once the merge is reported.
   * A merge already sent, or a warm engine, is asked the step outright. */
  uint32_t ask = estimate.step_mhz;
  deferred_mhz_ = 0;
  if (cold && !merge_reported && ask > tuning_.cold_cap_mhz) {
    deferred_mhz_ = ask;
    ask = tuning_.cold_cap_mhz;
  }
  host_.TraceInt("hwc_gov_deferred_mhz", int32_t(deferred_mhz_));

  /* Only upward from what the engine is already doing, by devfreq's doing
   * or by a floor still standing from the previous merge. A standing
   * floor is kept standing: this merge's fence will extend it. */
  const uint32_t have =
      std::max(engine_.CurrentMhz().value_or(0), engine_.floor_mhz());
  const bool raised = ask > have && engine_.SetFloor(ask);
  if (raised || engine_.floor_mhz() != 0) {
    /* A floor for a merge still to come waits for its report; one for a
     * merge already in flight follows that merge's fence -- and if the
     * fence is already gone, the hold starts now. */
    if (!merge_reported)
      KeepFloorFor(now);
    else
      FollowFences(now);
  }

  /* Otherwise last, and only if the submit is still ahead: the request
   * runs the processor's whole scaling step on this thread -- three
   * milliseconds when the memory clock has to follow -- and put first on
   * a busy processor it held the warm-up and the floor behind it until
   * the composer had submitted. */
  if (lift_cpu && !lifted && !mailbox_.HasSubmitted() &&
      cpu_.Lift(tuning_.cpu_khz))
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
  deferred_mhz_ = 0;
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
