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

/* The composition load governor for Tegra K1: hwcgovernor.tegra.so.
 *
 * What it decides, once per planned merge: whether to wake a powered-down
 * engine ahead of the merge, what clock floor to ask of the engine for the
 * merge's duration, and whether to lift the processor's floor for the few
 * milliseconds between the plan and the submit. The kernel does the rest --
 * the memory clock follows the engine's floor, and the display's isochronous
 * share follows the engine being on.
 *
 * Everything that may sleep -- the clock request, the warm-up pass, the
 * processor floor -- runs on this library's own thread. The composer's
 * threads only drop a snapshot in a mailbox and ring an eventfd. The thread
 * waits in one place, poll(), on that eventfd and on the fences of merges
 * in flight, so a merge finishing and a frame being planned are the same
 * kind of wake-up.
 *
 * The engine is reached through /dev/nvhost-vic, opened once for the life
 * of the library: the per-descriptor clock request is the one kernel entry
 * point that also pulls the memory clock along, and it lives only as long
 * as the descriptor does. The processor floor goes through /dev/cpu_freq_min,
 * a pm_qos request held while that descriptor is open.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <android/log.h>
#include <cutils/properties.h>

#include "governor/HwcGovernor.h"

namespace {

using android::hwc::governor::Frame;
using android::hwc::governor::Governor;
using android::hwc::governor::GovernorHost;
using android::hwc::governor::Member;
using android::hwc::governor::Power;

constexpr int64_t nsPerMs = 1000000LL;
constexpr int64_t nsPerUs = 1000LL;
constexpr int64_t nsPerSec = 1000000000LL;

/* nvhost's per-descriptor clock request, spelled out rather than taken from
 * the kernel's header: that header is GPL and is not exported. Rate in
 * hertz, module nought for the engine's own clock. Nought as the rate lifts
 * the request, and the descriptor falls back to what devfreq asks. */
struct ClockRateArgs {
  uint32_t rate;
  uint32_t moduleid;
};
constexpr unsigned long ioctlGetClockRate = _IOWR('H', 9, ClockRateArgs);
constexpr unsigned long ioctlSetClockRate = _IOW('H', 10, ClockRateArgs);

/* The engine's clock steps on this chip, megahertz. The kernel rounds a
 * request to one of these itself; the table is here so the decision can
 * be traced in the same terms, and so "one step above" means one step. */
constexpr uint32_t clockStepsMhz[] = {180, 258, 336, 378, 420, 462,
                                      504, 552, 600, 684, 720, 756};
constexpr uint32_t topStepMhz = 756;

int64_t NowNs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * nsPerSec + int64_t(ts.tv_nsec);
}

/* What the policy is tuned by.
 *
 * The defaults are the measured ones and live here. A file on the device,
 * /vendor/etc/hwc_governor.conf, overrides any of them with lines of
 * `name=value` -- the names are the fields below, verbatim -- read once
 * when the library starts. No file is the usual answer; a line that names
 * nothing is said once in the log and skipped. */
constexpr const char *tuningPath = "/vendor/etc/hwc_governor.conf";

struct Tuning {
  /* The model's safety margin, per cent. Underestimating the merge by a
   * third loses nearly every rescue; overestimating by a third keeps them
   * at the cost of a few needless requests. */
  uint32_t factor_pct = 125;

  /* Pixels the engine moves per clock. Two on this chip. */
  uint32_t pixels_per_clock = 2;

  /* The floor asked of a cold engine when the model wants less: until the
   * model is calibrated its underestimates are covered from here. Nought
   * trusts the model. */
  uint32_t min_cold_mhz = 504;

  /* How long the floor outlives the last merge's fence, so the merges of
   * one transition do not each pay for the clock coming and going. */
  uint32_t hold_ms = 17;

  /* After which the engine is taken to have powered down. The kernel's
   * powergate delay. */
  uint32_t powergate_ms = 500;

  /* The processor floor's step, kilohertz, and how long it may be held.
   * Nought as the step lifts the processor never. */
  uint32_t cpu_khz = 1044000;
  uint32_t cpu_cap_ms = 5;

  /* The path from validate to the submit, measured: the composer's lead
   * and the submit itself on a warm engine, on a cold one with the
   * processor lifted, and on a cold one without. */
  uint32_t lead_us = 1500;
  uint32_t submit_warm_us = 250;
  uint32_t submit_cold_us = 1600;
  uint32_t submit_cold_slow_us = 4000;

  /* How far before the latch the merge has to be done. */
  uint32_t latch_margin_us = 700;

  /* Fallbacks for a frame planned before the composer has a vsync phase. */
  uint32_t budget_empty_us = 7200;
  uint32_t budget_waited_us = 23000;

  /* How long a fence is waited for before it is given up on, and how long a
   * floor is kept when the merge it was raised for never reports. */
  uint32_t fence_patience_ms = 500;
  uint32_t orphan_ms = 100;

  /* How often the performance profile is asked for. */
  uint32_t profile_poll_ms = 250;
};

struct TuningKey {
  const char *name;
  uint32_t Tuning::*field;
};

constexpr TuningKey tuningKeys[] = {
    {"factor_pct", &Tuning::factor_pct},
    {"pixels_per_clock", &Tuning::pixels_per_clock},
    {"min_cold_mhz", &Tuning::min_cold_mhz},
    {"hold_ms", &Tuning::hold_ms},
    {"powergate_ms", &Tuning::powergate_ms},
    {"cpu_khz", &Tuning::cpu_khz},
    {"cpu_cap_ms", &Tuning::cpu_cap_ms},
    {"lead_us", &Tuning::lead_us},
    {"submit_warm_us", &Tuning::submit_warm_us},
    {"submit_cold_us", &Tuning::submit_cold_us},
    {"submit_cold_slow_us", &Tuning::submit_cold_slow_us},
    {"latch_margin_us", &Tuning::latch_margin_us},
    {"budget_empty_us", &Tuning::budget_empty_us},
    {"budget_waited_us", &Tuning::budget_waited_us},
    {"fence_patience_ms", &Tuning::fence_patience_ms},
    {"orphan_ms", &Tuning::orphan_ms},
    {"profile_poll_ms", &Tuning::profile_poll_ms},
};

/* Strips blanks and a trailing comment; returns the first byte of what is
 * left, which is a NUL for a line with nothing on it. */
char *Trim(char *line) {
  char *hash = strchr(line, '#');
  if (hash != nullptr)
    *hash = '\0';
  while (*line == ' ' || *line == '\t')
    ++line;
  char *end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\n' || end[-1] == '\r'))
    *--end = '\0';
  return line;
}

/* A snapshot as the thread keeps it: the frame with its members copied
 * alongside, since the frame's own pointer dies with the call. */
struct Planned {
  Frame frame;
  std::vector<Member> members;
};

struct Submitted {
  uint64_t seq;
  int fd;
};

/* A merge fence the thread is watching. */
struct Watched {
  int fd;
  uint64_t seq;
  int64_t since_ns;
};

class TegraGovernor final : public Governor {
 public:
  explicit TegraGovernor(GovernorHost *host);
  ~TegraGovernor() override;

  bool Start();

  void FramePlanned(const Frame &frame) override;
  void MergeSubmitted(uint64_t seq, int merge_fence_fd) override;
  void FramePresented(uint64_t seq) override;
  void PowerMode(Power mode) override;

 private:
  void Ring();
  void ThreadFn();
  void ReadTuning();
  void Decide(const Planned &planned, int64_t now);
  void Warm(int64_t now);
  bool SetFloor(uint32_t mhz);
  std::optional<uint32_t> CurrentMhz();
  void LiftCpu(int64_t now);
  void DropCpu();
  void ReleaseFloor();
  void DropEverything();
  int ReadProfile(int64_t now);
  void Log(int prio, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

  GovernorHost *const host_;
  Tuning tuning_;

  int vic_fd_ = -1;
  int cpu_fd_ = -1;
  int event_fd_ = -1;

  /* The mailbox. Written by the composer's threads, emptied by ours. */
  std::mutex mailbox_mutex_;
  std::optional<Planned> pending_;
  std::vector<Submitted> submitted_;
  Power power_ = Power::on;
  bool stop_ = false;

  std::thread thread_;

  /* Thread-only state from here down. */
  std::vector<Watched> watched_;
  uint32_t floor_mhz_ = 0;
  int64_t release_due_ns_ = 0;  /* when the floor may go, nought if not yet */
  int64_t orphan_due_ns_ = 0;   /* floor raised, no merge reported by then */
  int64_t cpu_until_ns_ = 0;    /* processor floor held until, nought if not */
  int64_t last_warm_ns_ = 0;
  int64_t profile_read_ns_ = 0;
  int profile_ = -1;
  bool disabled_ = false;       /* the kernel refused us; a witness now */
  bool warm_refused_logged_ = false;
};

TegraGovernor::TegraGovernor(GovernorHost *host) : host_(host) {
}

void TegraGovernor::ReadTuning() {
  FILE *file = fopen(tuningPath, "re");
  if (file == nullptr)
    return;

  char line[160];
  unsigned number = 0;
  while (fgets(line, sizeof(line), file) != nullptr) {
    ++number;
    char *text = Trim(line);
    if (*text == '\0')
      continue;

    char *equals = strchr(text, '=');
    if (equals == nullptr) {
      Log(ANDROID_LOG_WARN, "%s:%u: not a name=value line", tuningPath,
          number);
      continue;
    }
    *equals = '\0';
    const char *name = Trim(text);
    const char *value = Trim(equals + 1);

    char *rest = nullptr;
    const unsigned long parsed = strtoul(value, &rest, 10);
    if (rest == value || *rest != '\0' || parsed > UINT32_MAX) {
      Log(ANDROID_LOG_WARN, "%s:%u: %s: not a number: %s", tuningPath,
          number, name, value);
      continue;
    }

    bool known = false;
    for (const TuningKey &key : tuningKeys) {
      if (strcmp(key.name, name) == 0) {
        tuning_.*key.field = static_cast<uint32_t>(parsed);
        known = true;
        break;
      }
    }
    if (!known)
      Log(ANDROID_LOG_WARN, "%s:%u: no such setting: %s", tuningPath, number,
          name);
  }
  fclose(file);

  if (tuning_.factor_pct == 0)
    tuning_.factor_pct = 100;
  if (tuning_.pixels_per_clock == 0)
    tuning_.pixels_per_clock = 1;
  Log(ANDROID_LOG_INFO, "tuning read from %s", tuningPath);
}

bool TegraGovernor::Start() {
  ReadTuning();

  event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (event_fd_ < 0) {
    Log(ANDROID_LOG_ERROR, "eventfd: %s", strerror(errno));
    return false;
  }

  /* Once, for good: the clock request lives as long as this descriptor,
   * and an empty descriptor holds neither power nor a channel. */
  vic_fd_ = open("/dev/nvhost-vic", O_RDWR | O_CLOEXEC);
  if (vic_fd_ < 0) {
    Log(ANDROID_LOG_ERROR, "/dev/nvhost-vic: %s; running as a witness",
        strerror(errno));
    disabled_ = true;
  }

  /* Optional: without it the processor is simply not lifted. The device's
   * policy has to allow the composer this node. */
  cpu_fd_ = open("/dev/cpu_freq_min", O_WRONLY | O_CLOEXEC);
  if (cpu_fd_ < 0)
    Log(ANDROID_LOG_WARN, "/dev/cpu_freq_min: %s; the processor will not be "
        "lifted", strerror(errno));

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
  for (const Submitted &s : submitted_)
    if (s.fd >= 0)
      close(s.fd);
  submitted_.clear();

  if (cpu_fd_ >= 0)
    close(cpu_fd_);
  if (vic_fd_ >= 0)
    close(vic_fd_);
  if (event_fd_ >= 0)
    close(event_fd_);
}

void TegraGovernor::Log(int prio, const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  host_->Log(prio, msg);
}

void TegraGovernor::Ring() {
  const uint64_t one = 1;
  if (write(event_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN)
    Log(ANDROID_LOG_WARN, "eventfd write: %s", strerror(errno));
}

/* The composer's side: copy, drop in the mailbox, ring, return. */

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
    submitted_.push_back(Submitted{seq, merge_fence_fd});
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

void TegraGovernor::ThreadFn() {
  pthread_setname_np(pthread_self(), "hwc-governor");

  std::vector<struct pollfd> fds;

  for (;;) {
    fds.clear();
    fds.push_back(pollfd{event_fd_, POLLIN, 0});
    for (const Watched &w : watched_)
      fds.push_back(pollfd{w.fd, POLLIN, 0});

    /* Sleep until something is due, or for good if nothing is. */
    int64_t now = NowNs();
    int64_t due = -1;
    auto consider = [&](int64_t when) {
      if (when > 0 && (due < 0 || when < due))
        due = when;
    };
    consider(cpu_until_ns_);
    consider(release_due_ns_);
    consider(orphan_due_ns_);
    for (const Watched &w : watched_)
      consider(w.since_ns + int64_t(tuning_.fence_patience_ms) * nsPerMs);

    int timeout_ms = -1;
    if (due >= 0)
      timeout_ms = due <= now ? 0 : int((due - now + nsPerMs - 1) / nsPerMs);

    if (poll(fds.data(), fds.size(), timeout_ms) < 0 && errno != EINTR) {
      Log(ANDROID_LOG_ERROR, "poll: %s", strerror(errno));
      break;
    }
    now = NowNs();

    /* Empty the mailbox. */
    std::optional<Planned> planned;
    std::vector<Submitted> submitted;
    Power power = Power::on;
    bool stop = false;
    {
      const std::lock_guard<std::mutex> lock(mailbox_mutex_);
      if (fds[0].revents & POLLIN) {
        uint64_t drained = 0;
        if (read(event_fd_, &drained, sizeof(drained)) < 0 && errno != EAGAIN)
          Log(ANDROID_LOG_WARN, "eventfd read: %s", strerror(errno));
      }
      planned = std::move(pending_);
      pending_.reset();
      submitted.swap(submitted_);
      power = power_;
      stop = stop_;
    }

    if (stop) {
      for (const Submitted &s : submitted)
        if (s.fd >= 0)
          close(s.fd);
      DropEverything();
      break;
    }

    if (power != Power::on) {
      /* A display going dark drops everything: no merge is coming, and
       * a floor left standing would hold the memory clock up through the
       * doze. */
      for (const Submitted &s : submitted)
        if (s.fd >= 0)
          close(s.fd);
      DropEverything();
      continue;
    }

    /* Merges that went to the engine: the processor has done its part,
     * the fence is now what says when the engine has done its. */
    for (const Submitted &s : submitted) {
      if (s.fd >= 0)
        watched_.push_back(Watched{s.fd, s.seq, now});
      orphan_due_ns_ = 0;
    }
    if (!submitted.empty())
      DropCpu();

    if (planned)
      Decide(*planned, now);

    /* Fences that came due, or that were waited on long enough. */
    bool any_done = false;
    for (size_t i = 0; i < watched_.size();) {
      const short revents = i + 1 < fds.size() && fds[i + 1].fd == watched_[i].fd
                                ? fds[i + 1].revents
                                : 0;
      const bool signaled = (revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
      const bool given_up =
          now - watched_[i].since_ns >= int64_t(tuning_.fence_patience_ms) * nsPerMs;
      if (!signaled && !given_up) {
        ++i;
        continue;
      }
      close(watched_[i].fd);
      watched_.erase(watched_.begin() + i);
      any_done = true;
    }
    if (any_done && watched_.empty() && floor_mhz_ != 0) {
      release_due_ns_ = now + int64_t(tuning_.hold_ms) * nsPerMs;
      orphan_due_ns_ = 0;
    }

    if (floor_mhz_ != 0 && watched_.empty()) {
      if (release_due_ns_ != 0 && now >= release_due_ns_)
        ReleaseFloor();
      else if (release_due_ns_ == 0 && orphan_due_ns_ != 0 &&
               now >= orphan_due_ns_)
        ReleaseFloor();
    }

    if (cpu_until_ns_ != 0 && now >= cpu_until_ns_)
      DropCpu();
  }
}

int TegraGovernor::ReadProfile(int64_t now) {
  if (profile_read_ns_ != 0 &&
      now - profile_read_ns_ < int64_t(tuning_.profile_poll_ms) * nsPerMs)
    return profile_;

  profile_read_ns_ = now;
  char value[PROPERTY_VALUE_MAX] = {};
  if (property_get("sys.perf.profile", value, "") <= 0) {
    profile_ = -1;
    return profile_;
  }
  profile_ = atoi(value);
  return profile_;
}

void TegraGovernor::Decide(const Planned &planned, int64_t now) {
  const Frame &f = planned.frame;

  if (!f.merge_planned || f.merge_reuse_predicted) {
    host_->TraceInt("hwc_gov_need_mhz", 0);
    return;
  }

  const int profile = ReadProfile(now);
  const bool power_save = profile == 0;

  const int64_t last_use = std::max(f.last_engine_use_ns, last_warm_ns_);
  const bool cold =
      last_use == 0 || now - last_use > int64_t(tuning_.powergate_ms) * nsPerMs;

  /* The model: half a clock per pixel, over the larger of what is read
   * and what is written, per member. */
  uint64_t area = 0;
  for (const Member &m : planned.members) {
    const uint64_t src = uint64_t(m.src_w) * m.src_h;
    const uint64_t dst = uint64_t(m.dst_w) * m.dst_h;
    area += std::max(src, dst);
  }
  const double cycles = double(area) / tuning_.pixels_per_clock *
                        tuning_.factor_pct / 100.0;

  /* The latch this frame is aiming at. With the previous flip landed the
   * frame targets the nearest vsync; otherwise it waits for that one to
   * carry the previous frame and targets the one after. */
  int64_t deadline = 0;
  if (f.last_vsync_ns > 0 && f.vsync_period_ns > 0) {
    const int64_t elapsed = now - f.last_vsync_ns;
    const int64_t periods = elapsed >= 0 ? elapsed / f.vsync_period_ns : -1;
    int64_t next = f.last_vsync_ns + (periods + 1) * f.vsync_period_ns;
    if (!f.previous_flip_landed)
      next += f.vsync_period_ns;
    deadline = next - int64_t(tuning_.latch_margin_us) * nsPerUs;
  } else {
    deadline = now + int64_t(f.previous_flip_landed ? tuning_.budget_empty_us
                                                    : tuning_.budget_waited_us) *
                         nsPerUs;
  }

  const bool lift_cpu = cold && f.previous_flip_landed && !power_save &&
                        cpu_fd_ >= 0 && tuning_.cpu_khz != 0 && !disabled_;
  const int64_t submit_us =
      cold ? (lift_cpu ? tuning_.submit_cold_us : tuning_.submit_cold_slow_us)
           : tuning_.submit_warm_us;
  const int64_t submit_end =
      now + (int64_t(tuning_.lead_us) + submit_us) * nsPerUs;

  int64_t budget = deadline - submit_end;
  bool late = false;
  if (budget < nsPerMs) {
    budget = nsPerMs;
    late = true;
  }

  const double need_hz = cycles * double(nsPerSec) / double(budget);
  uint32_t need_mhz = uint32_t(std::min<double>(need_hz / 1e6, 4000.0));

  uint32_t step = 0;
  for (uint32_t s : clockStepsMhz) {
    if (s >= need_mhz) {
      step = s;
      break;
    }
  }
  const bool beyond = step == 0;
  if (beyond)
    step = topStepMhz;
  if (cold && step < tuning_.min_cold_mhz)
    step = tuning_.min_cold_mhz;

  host_->TraceInt("hwc_gov_est_kcycles", int32_t(cycles / 1000.0));
  host_->TraceInt("hwc_gov_budget_us", int32_t(budget / nsPerUs));
  host_->TraceInt("hwc_gov_need_mhz", int32_t(need_mhz));
  host_->TraceInt("hwc_gov_cold", cold ? 1 : 0);
  host_->TraceInt("hwc_gov_late", (late || beyond) ? 1 : 0);
  host_->TraceInt("hwc_gov_profile", profile);

  host_->TraceInt("hwc_gov_step_mhz", int32_t(step));

  if (power_save) {
    /* The one thing allowed here: a cold engine is woken, nothing is
     * raised. */
    if (cold)
      Warm(now);
    return;
  }

  if (cold) {
    if (lift_cpu)
      LiftCpu(now);
    /* Before the floor, and it has to be: a floor filed against a
     * powered-down engine is applied as it comes up, and coming up
     * straight onto the top step cost tens of milliseconds of sleeping in
     * the kernel's voltage scaling. The warm-up comes up at whatever the
     * engine idles at; the floor follows once it is awake. */
    Warm(now);
  }

  if (disabled_)
    return;

  const std::optional<uint32_t> current = CurrentMhz();
  const uint32_t have = std::max(current.value_or(0), floor_mhz_);
  if (step <= have) {
    /* Already there, by devfreq's doing or by a floor still standing from
     * the previous merge. A standing floor is kept standing: this merge's
     * fence will extend it. */
    if (floor_mhz_ != 0) {
      release_due_ns_ = 0;
      orphan_due_ns_ = now + int64_t(tuning_.orphan_ms) * nsPerMs;
    }
    return;
  }

  if (SetFloor(step)) {
    release_due_ns_ = 0;
    orphan_due_ns_ = now + int64_t(tuning_.orphan_ms) * nsPerMs;
  }
}

void TegraGovernor::Warm(int64_t now) {
  const int fd = host_->WarmEngine();
  if (fd < 0) {
    if (!warm_refused_logged_) {
      Log(ANDROID_LOG_WARN, "the engine would not take the warm-up pass");
      warm_refused_logged_ = true;
    }
    return;
  }
  /* Not waited for and not watched: the submit itself is what brought the
   * engine up, and the merge queues behind it on the same channel. */
  close(fd);
  last_warm_ns_ = now;
  host_->TraceInt("hwc_gov_warm", 1);
  host_->TraceInt("hwc_gov_warm", 0);
}

std::optional<uint32_t> TegraGovernor::CurrentMhz() {
  if (vic_fd_ < 0)
    return std::nullopt;
  ClockRateArgs args = {0, 0};
  if (ioctl(vic_fd_, ioctlGetClockRate, &args) < 0)
    return std::nullopt;
  return args.rate / 1000000U;
}

bool TegraGovernor::SetFloor(uint32_t mhz) {
  if (vic_fd_ < 0 || disabled_)
    return false;

  ClockRateArgs args = {mhz * 1000000U, 0};
  if (ioctl(vic_fd_, ioctlSetClockRate, &args) < 0) {
    const int err = errno;
    if (err == EPERM || err == ENODEV || err == EACCES) {
      Log(ANDROID_LOG_ERROR, "engine clock request refused (%s); a witness "
          "from here on", strerror(err));
      disabled_ = true;
    } else {
      Log(ANDROID_LOG_WARN, "engine clock request %u MHz: %s", mhz,
          strerror(err));
    }
    return false;
  }

  floor_mhz_ = mhz;
  host_->TraceInt("hwc_vic_floor", int32_t(mhz));
  return true;
}

void TegraGovernor::ReleaseFloor() {
  release_due_ns_ = 0;
  orphan_due_ns_ = 0;
  if (floor_mhz_ == 0)
    return;
  floor_mhz_ = 0;
  if (vic_fd_ < 0)
    return;
  ClockRateArgs args = {0, 0};
  if (ioctl(vic_fd_, ioctlSetClockRate, &args) < 0)
    Log(ANDROID_LOG_WARN, "engine clock release: %s", strerror(errno));
  host_->TraceInt("hwc_vic_floor", 0);
}

void TegraGovernor::LiftCpu(int64_t now) {
  if (cpu_fd_ < 0)
    return;
  /* Text, with a line break: the node reads exactly four bytes as a binary
   * word, and anything else as decimal. */
  char text[24];
  const int n = snprintf(text, sizeof(text), "%u\n", tuning_.cpu_khz);
  if (write(cpu_fd_, text, size_t(n)) < 0) {
    Log(ANDROID_LOG_WARN, "processor floor: %s", strerror(errno));
    close(cpu_fd_);
    cpu_fd_ = -1;
    return;
  }
  cpu_until_ns_ = now + int64_t(tuning_.cpu_cap_ms) * nsPerMs;
  host_->TraceInt("hwc_cpu_floor_khz", int32_t(tuning_.cpu_khz));
}

void TegraGovernor::DropCpu() {
  if (cpu_until_ns_ == 0)
    return;
  cpu_until_ns_ = 0;
  if (cpu_fd_ < 0)
    return;
  if (write(cpu_fd_, "0\n", 2) < 0)
    Log(ANDROID_LOG_WARN, "processor floor release: %s", strerror(errno));
  host_->TraceInt("hwc_cpu_floor_khz", 0);
}

void TegraGovernor::DropEverything() {
  for (const Watched &w : watched_)
    close(w.fd);
  watched_.clear();
  ReleaseFloor();
  DropCpu();
}

}  // namespace

extern "C" uint32_t hwc_governor_api_version() {
  return android::hwc::governor::apiVersion;
}

extern "C" Governor *hwc_governor_create(GovernorHost *host) {
  if (host == nullptr)
    return nullptr;
  auto *governor = new TegraGovernor(host);
  if (!governor->Start()) {
    delete governor;
    return nullptr;
  }
  return governor;
}

extern "C" void hwc_governor_destroy(Governor *governor) {
  delete governor;
}
