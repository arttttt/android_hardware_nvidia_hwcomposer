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
#include <unistd.h>

#include <algorithm>

#include <android/log.h>

#include "governor/Clock.h"
#include "governor/Log.h"

namespace android::hwc::governor::tegra {

TegraGovernor::TegraGovernor(GovernorHost &host)
    : host_(host), engine_(host), gpu_clock_(host), cpu_(host) {
}

bool TegraGovernor::Start() {
  /* Once, at start: the delay is the kernel's, and when the engine is
   * taken to be cold has to agree with it. */
  if (ReadPowergateDelay(powergatePath, &tuning_))
    Logf(host_, ANDROID_LOG_INFO, "engine powergate delay %u ms",
         tuning_.powergate_ms);
  else
    Logf(host_, ANDROID_LOG_WARN, "no engine powergate delay; assuming %u ms",
         tuning_.powergate_ms);
  if (ReadTuningFile(tuningPath, &tuning_, host_))
    host_.Log(ANDROID_LOG_INFO, "tuning read from the device");
  keep_vic_ = KeepWarm(
      int64_t(tuning_.keep_warm_ms) * nsPerMs,
      int64_t(tuning_.powergate_ms - std::min(tuning_.keep_margin_ms,
                                              tuning_.powergate_ms)) *
          nsPerMs,
      int64_t(tuning_.powergate_ms) * nsPerMs);

  if (!mailbox_.Open()) {
    LogErrno(host_, ANDROID_LOG_ERROR, "eventfd", errno);
    return false;
  }

  if (!engine_.Open())
    host_.Log(ANDROID_LOG_ERROR, "no engine clock; running as a witness");
  cpu_.Open();
  if (tuning_.gpu_boost != 0)
    gpu_clock_.Open();

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

void TegraGovernor::GpuPlanned(uint64_t seq, uint32_t pixels) {
  if (dead_.load())
    return;
  mailbox_.PlanGpu(seq, pixels);
}

void TegraGovernor::GpuSubmitted(uint64_t seq, int gpu_fence_fd) {
  if (dead_.load()) {
    if (gpu_fence_fd >= 0)
      close(gpu_fence_fd);
    return;
  }
  mailbox_.SubmitGpu(seq, gpu_fence_fd);
}

void TegraGovernor::FramePresented(uint64_t /*seq*/) {
  /* Counted only, for keeping the engine up while frames come; the thread
   * is not woken for it. The merge's own fence says when the engine is
   * done, and the submit says when the processor may be let go. */
  presented_.fetch_add(1, std::memory_order_relaxed);
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
  consider(vic_.NextDueNs());
  consider(gpu_.NextDueNs());
  consider(keep_vic_.NextDueNs());

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
    /* The poll set: the mailbox first, then each engine's fences in their
     * order. */
    fds.clear();
    fds.push_back(pollfd{mailbox_.fd(), POLLIN, 0});
    vic_.AppendPollSet(&fds);
    gpu_.AppendPollSet(&fds);

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
      for (const std::vector<Watched> *list :
           {&mail.submitted, &mail.gpu_submitted})
        for (const Watched &w : *list)
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
      DropCpu();
      vic_.Reported();
      keep_vic_.Worked(now);
      presented_at_up_ = presented_.load(std::memory_order_relaxed);
    }
    for (const Watched &w : mail.submitted)
      NoteSubmit(w.seq, w.since_ns);

    vic_.Follow(fds, mail.submitted, now);

    if (!mail.gpu_submitted.empty())
      gpu_.Reported();
    gpu_.Follow(fds, mail.gpu_submitted, now);

    if (mail.planned) {
      /* Reported this round or earlier: either way the merge is gone. */
      const uint64_t seq = mail.planned->frame.seq;
      Decide(*mail.planned, now, vic_.HasReported(seq));
      /* A merge reported in the same round as its plan is timed against
       * that plan; the pass before the plan did not know it yet. */
      for (const Watched &w : mail.submitted)
        NoteSubmit(w.seq, w.since_ns);
    }
    /* After the frame's own plan: its latch is what the drawing aims at. */
    if (mail.gpu_planned)
      DecideGpu(*mail.gpu_planned, now);

    vic_.ReleaseIfDue(now);
    gpu_.ReleaseIfDue(now);
    KeepWarmIfDue(now);

    if (cpu_until_ns_ != 0 && now >= cpu_until_ns_)
      DropCpu();
  }
}

void TegraGovernor::DropCpu() {
  cpu_until_ns_ = 0;
  cpu_.Drop();
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

  const int64_t gap_ns =
      last_plan_ns_ != 0 ? f.now_ns - last_plan_ns_ : INT64_MAX;
  last_plan_ns_ = f.now_ns;
  last_frame_ = f;

  if (!f.merge_planned || f.merge_reuse_predicted) {
    host_.TraceInt("hwc_gov_need_mhz", 0);
    WarmAhead(f, now, gap_ns);
    return;
  }

  const int profile =
      profile_.Read(now, int64_t(tuning_.profile_poll_ms) * nsPerMs);
  const bool power_save = profile == PerfProfile::powerSave;
  const bool cold = EngineCold(f, last_warm_ns_, now, tuning_);
  /* The first merge after a pause: the processor is at its light-load
   * clock and the composer's share of the submit is long. The pause is
   * read from the engine's last use. */
  const bool paused =
      tuning_.pause_ms != 0 && f.last_engine_use_ns != 0 &&
      now - f.last_engine_use_ns >= int64_t(tuning_.pause_ms) * nsPerMs;
  host_.TraceInt("hwc_gov_paused", paused ? 1 : 0);

  /* The processor's clock now: what the composer's share of the submit
   * will run at, and what the last such share is measured in. One sysfs
   * read a plan. */
  const uint32_t cpu_khz =
      cpu_.available() && (tuning_.submit_measured != 0 || cold || paused)
          ? cpu_.CurrentKhz()
          : 0;
  host_.TraceInt("hwc_gov_cpu_khz", int32_t(cpu_khz));

  /* Remembered so the merge's report can be timed against the plan. */
  planned_seq_ = f.seq;
  planned_validate_ns_ = f.now_ns;
  planned_cold_ = cold;

  /* Lifting the processor is for the composer's submit; once the submit
   * has happened there is nothing left to lift it for. */
  const bool lift_cpu = (cold || (paused && tuning_.cpu_pause_lift != 0)) &&
                        !merge_reported && f.previous_flip_landed &&
                        !power_save && cpu_.available() &&
                        tuning_.cpu_khz != 0 && vic_.usable();

  /* An idling processor first: the engine's floor ramps the rail by I2C
   * writes served by interrupts, and at its lowest clock the processor
   * takes milliseconds to get to each -- longer than the lift, memory
   * clock and all. Above the threshold the lift goes last, where it holds
   * nothing up. A warm engine has no rail to ramp, so nothing is held up
   * by lifting first, and the processor is lifted at once whenever it is
   * below the step. */
  bool lifted = false;
  if (lift_cpu &&
      (cpu_khz <= tuning_.cpu_low_khz || (!cold && cpu_khz < tuning_.cpu_khz))) {
    lifted = cpu_.Lift(tuning_.cpu_khz);
    if (lifted)
      cpu_until_ns_ = now + int64_t(tuning_.cpu_cap_ms) * nsPerMs;
  }
  host_.TraceInt("hwc_gov_cpu_first", lifted ? 1 : 0);

  /* How long the submit will take to come: the measured warm path at
   * the processor's present clock -- or at the lift's step, when the
   * processor is being lifted for this very submit -- plus, when the
   * engine has to be brought up on the way, what that measured on the
   * cold merges before, or the assumed cold submit until it has been
   * measured; the assumed path when there is no measure yet. */
  const uint32_t path_khz =
      lift_cpu ? std::max(cpu_khz, tuning_.cpu_khz) : cpu_khz;
  planned_cpu_khz_ = path_khz;
  int64_t submit_after =
      tuning_.submit_measured != 0 ? MeasuredSubmitNs(path_khz) : 0;
  if (submit_after > 0 && cold) {
    const int64_t measured_extra = MeasuredColdExtraNs(path_khz);
    submit_after += measured_extra > 0
                        ? measured_extra
                        : int64_t(lift_cpu ? tuning_.submit_cold_us
                                           : tuning_.submit_cold_slow_us) *
                              nsPerUs;
  }
  host_.TraceInt("hwc_gov_submit_after_us", int32_t(submit_after / nsPerUs));

  const MergeEstimate estimate =
      EstimateMerge(f, planned.members.data(), planned.members.size(), now,
                    tuning_, cold, lift_cpu, submit_after);
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

  if (!vic_.usable())
    return;

  /* A cold engine with the submit still ahead is asked no more than the
   * cap now: a higher step ramps the core rail with the bus clock's lock
   * held, and the composer's submit would wait behind it for
   * milliseconds. The rest is owed, and asked once the merge is reported.
   * A merge already sent, or a warm engine, is asked the step outright. */
  uint32_t ask = estimate.step_mhz;
  uint32_t owed = 0;
  if (cold && !merge_reported && ask > tuning_.cold_cap_mhz) {
    owed = ask;
    ask = tuning_.cold_cap_mhz;
  }
  vic_.Ask(ask, owed, merge_reported, now);

  /* Otherwise last, and only if the submit is still ahead: the request
   * runs the processor's whole scaling step on this thread -- three
   * milliseconds when the memory clock has to follow -- and put first on
   * a busy processor it held the warm-up and the floor behind it until
   * the composer had submitted. */
  if (lift_cpu && !lifted && !mailbox_.HasSubmitted() &&
      cpu_.Lift(tuning_.cpu_khz))
    cpu_until_ns_ = now + int64_t(tuning_.cpu_cap_ms) * nsPerMs;
}

void TegraGovernor::DecideGpu(const GpuPlan &plan, int64_t now) {
  if (tuning_.gpu_boost == 0 || !gpu_.usable())
    return;
  /* The frame's plan comes first in the same validate; a drawing whose
   * frame never reached the governor has no latch to aim at. */
  if (last_frame_.seq != plan.seq)
    return;
  /* No floor in power save: there the drawing runs at what podgov gives. */
  if (profile_.Read(now, int64_t(tuning_.profile_poll_ms) * nsPerMs) ==
      PerfProfile::powerSave)
    return;

  const WorkEstimate e = EstimateGpu(last_frame_, plan.pixels, now, tuning_);
  host_.TraceInt("hwc_gov_gpu_kpx", int32_t(plan.pixels / 1000));
  host_.TraceInt("hwc_gov_gpu_budget_us", int32_t(e.budget_ns / nsPerUs));
  host_.TraceInt("hwc_gov_gpu_need_mhz", int32_t(e.need_mhz));
  host_.TraceInt("hwc_gov_gpu_step_mhz", int32_t(e.step_mhz));
  host_.TraceInt("hwc_gov_gpu_slip", e.slipped ? 1 : 0);

  gpu_.Ask(e.step_mhz, 0, gpu_.HasReported(plan.seq), now);
}

void TegraGovernor::NoteSubmit(uint64_t seq, int64_t reported_ns) {
  if (seq != planned_seq_ || planned_validate_ns_ == 0)
    return;
  const int64_t sample_ns = reported_ns - planned_validate_ns_;
  planned_validate_ns_ = 0;
  host_.TraceInt("hwc_gov_submit_us", int32_t(sample_ns / nsPerUs));

  /* A clock unread cannot be measured in, and a report a frame or more
   * late waited on something other than the composer. */
  if (planned_cpu_khz_ == 0 || sample_ns <= 0 || sample_ns > 100 * nsPerMs)
    return;
  /* A cold merge carries the engine's power-up in its path: kept apart,
   * so the warm path stays the composer's own work. */
  if (planned_cold_) {
    NoteColdSubmit(sample_ns);
    return;
  }

  /* Nanoseconds times kilohertz over a million: cycles. */
  const int64_t cycles = sample_ns * int64_t(planned_cpu_khz_) / 1000000;
  /* A path three times the mean is a wait on something else too. Such
   * paths come in runs -- a dozen merges at a stretch, at the same point
   * of a transition, the composer waiting between present and commit at
   * full clock -- and learning from them would have the next merge at a
   * lower clock predicted twenty milliseconds long; replayed, restarting
   * the mean on such a run was worse than never doing so. */
  if (submit_cycles_ != 0 && cycles > 3 * submit_cycles_)
    return;

  const int64_t smooth = int64_t(tuning_.submit_smooth);
  submit_cycles_ = submit_cycles_ == 0
                       ? cycles
                       : (submit_cycles_ * smooth + cycles) / (smooth + 1);
  host_.TraceInt("hwc_gov_submit_kcyc", int32_t(submit_cycles_ / 1000));
}

int64_t TegraGovernor::MeasuredSubmitNs(uint32_t cpu_khz) const {
  if (submit_cycles_ == 0 || cpu_khz == 0)
    return 0;
  return submit_cycles_ * 1000000 / int64_t(cpu_khz);
}

/* What a cold engine added: the whole path in cycles at the clock the plan
 * read it at, less the warm path's mean. Read back at the same clock, so
 * whatever the lift does to the real clock in between is in the measure
 * as it will be in the prediction. The assumed extra is what Android 10
 * needed -- 0.8 ms at the median with the processor held near 2 GHz by
 * the load -- and on Android 11, with the processor idling at 204-312 MHz
 * when the plan is made, the extra measured 1.8-2.7 ms at the median and
 * 5-6 at the ninetieth percentile. */
void TegraGovernor::NoteColdSubmit(int64_t sample_ns) {
  if (submit_cycles_ == 0 || planned_cpu_khz_ == 0)
    return;
  const int64_t total = sample_ns * int64_t(planned_cpu_khz_) / 1000000;
  const int64_t extra = std::max<int64_t>(total - submit_cycles_, 0);
  /* The same guard as the warm path's: a wait on something else. */
  if (cold_extra_cycles_ != 0 && extra > 3 * cold_extra_cycles_)
    return;

  const int64_t smooth = int64_t(tuning_.submit_smooth);
  cold_extra_cycles_ =
      cold_extra_cycles_ == 0
          ? extra
          : (cold_extra_cycles_ * smooth + extra) / (smooth + 1);
  host_.TraceInt("hwc_gov_cold_extra_kcyc", int32_t(cold_extra_cycles_ / 1000));
}

int64_t TegraGovernor::MeasuredColdExtraNs(uint32_t path_khz) const {
  if (tuning_.cold_measured == 0 || cold_extra_cycles_ == 0 || path_khz == 0)
    return 0;
  return cold_extra_cycles_ * 1000000 / int64_t(path_khz);
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
  keep_vic_.Warmed(now);
  host_.TraceInt("hwc_gov_warm", 1);
  host_.TraceInt("hwc_gov_warm", 0);
}

/* A frame without a merge to draw, the first after a pause as long as the
 * engine's powergate: the screen has come to life, and the transition's
 * merge is likely a few frames behind. Only the first such frame -- an
 * animation that never merges is not kept warm frame after frame -- and
 * not in power save, where the merge pays for its own waking. */
void TegraGovernor::WarmAhead(const Frame &f, int64_t now, int64_t gap_ns) {
  if (tuning_.warm_ahead == 0 ||
      gap_ns < int64_t(tuning_.powergate_ms) * nsPerMs ||
      !EngineCold(f, last_warm_ns_, now, tuning_))
    return;
  if (profile_.Read(now, int64_t(tuning_.profile_poll_ms) * nsPerMs) ==
      PerfProfile::powerSave)
    return;
  host_.TraceInt("hwc_gov_warm_ahead", 1);
  Warm(now);
  host_.TraceInt("hwc_gov_warm_ahead", 0);
}

/* The engine still up from a merge not long ago and about to power down:
 * given its pass if frames came since its last job -- see KeepWarm -- let
 * go if none did. Let go as well when the thread woke too late, the
 * engine down already, or the pass was refused: the timer is not set
 * again for it. Not in power save. */
void TegraGovernor::KeepWarmIfDue(int64_t now) {
  const int64_t due = keep_vic_.NextDueNs();
  if (due == 0 || now < due)
    return;
  const uint64_t presented = presented_.load(std::memory_order_relaxed);
  if (!keep_vic_.Due(now) || presented == presented_at_up_ ||
      profile_.Read(now, int64_t(tuning_.profile_poll_ms) * nsPerMs) ==
          PerfProfile::powerSave) {
    keep_vic_.GiveUp();
    return;
  }
  presented_at_up_ = presented;
  host_.TraceInt("hwc_gov_keep_warm", 1);
  Warm(now);
  host_.TraceInt("hwc_gov_keep_warm", 0);
  if (keep_vic_.last_up_ns() != now)
    keep_vic_.GiveUp();
}

void TegraGovernor::DropEverything() {
  vic_.DropAll();
  gpu_.DropAll();
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
