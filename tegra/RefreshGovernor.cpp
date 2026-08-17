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

#include "tegra/RefreshGovernor.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-refresh"

namespace android {
namespace hwc {

namespace {

/* The kernel's door, declared locally like every extension this composer
 * speaks: the value lands at a frame's end, the call never waits and
 * never hears back. Zero asks for the mode's own porch. */
constexpr uint32_t kSetActVfp = _IOW('D', 0x1F, __u32);

}  // namespace

std::unique_ptr<RefreshGovernor> RefreshGovernor::Probe(int dc_fd) {
  /* A release is harmless on a panel already at its native rate, which
   * makes it the honest probe: a kernel without the door answers ENOTTY,
   * a head whose stretch machinery never woke answers ENOSYS, and either
   * way the governor has no business existing. It also heals: a
   * predecessor that died on the floor left the panel slow, and this
   * very probe is what raises it. */
  uint32_t native = 0;
  if (ioctl(dc_fd, kSetActVfp, &native) != 0) {
    ALOGI("no refresh stretch on this kernel (%s), governor not started",
          strerror(errno));
    return nullptr;
  }

  return std::unique_ptr<RefreshGovernor>(new RefreshGovernor(dc_fd));
}

RefreshGovernor::RefreshGovernor(int dc_fd)
    : fd_(dc_fd), last_activity_(std::chrono::steady_clock::now()) {
  thread_ = std::thread(&RefreshGovernor::ThreadFn, this);
}

RefreshGovernor::~RefreshGovernor() {
  {
    std::lock_guard<std::mutex> guard(lock_);
    stop_ = true;
    /* The panel outlives this process; leaving it slow would hand the
     * next composer a mystery. */
    if (floored_)
      FileVfpLocked(0);
  }
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

void RefreshGovernor::FileVfpLocked(uint32_t vfp) {
  /* Fire-and-forget by kernel contract; a failure here is a lost
   * request the next decision re-files, not an error to handle. */
  ioctl(fd_, kSetActVfp, &vfp);
}

void RefreshGovernor::RaiseLocked() {
  if (!floored_)
    return;

  FileVfpLocked(0);
  const auto now = std::chrono::steady_clock::now();
  floor_total_ += now - floored_at_;
  last_raise_ = now;
  floored_ = false;
}

bool RefreshGovernor::VsyncTimestampTrustworthy() const {
  std::lock_guard<std::mutex> guard(lock_);

  if (floored_)
    return true; /* nobody listens on the floor; whoever does, woke us
                    first and moved last_raise_. */

  return std::chrono::steady_clock::now() - last_raise_ >= kRaiseShadow;
}

void RefreshGovernor::NoteActivity() {
  {
    std::lock_guard<std::mutex> guard(lock_);
    RaiseLocked();
    last_activity_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void RefreshGovernor::NoteVsyncEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (enabled) {
      /* The wake must complete before this returns: the framework's
       * first timing sample follows the enable, and it has to find the
       * panel already at its native rate. Filing here is enough even
       * though the kernel applies at the frame's end, because the
       * ticks born of the frames still finishing fall inside the
       * raise's shadow and are never delivered at all. */
      RaiseLocked();
      vsync_off_ = false;
    } else {
      vsync_off_ = true;
    }
    last_activity_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void RefreshGovernor::ThreadFn() {
  std::unique_lock<std::mutex> lk(lock_);

  while (!stop_) {
    if (floored_ || !vsync_off_) {
      /* Nothing to time: either already slow, or somebody still wants
       * frames. Sleep until told otherwise. */
      cv_.wait(lk);
      continue;
    }

    const auto deadline = last_activity_ + kQuietDelay;
    if (std::chrono::steady_clock::now() < deadline) {
      cv_.wait_until(lk, deadline);
      continue;
    }

    FileVfpLocked(kFloorVfp);
    floored_ = true;
    floored_at_ = std::chrono::steady_clock::now();
    descents_++;
  }
}

void RefreshGovernor::AppendDump(std::ostream &ss) const {
  std::lock_guard<std::mutex> guard(lock_);

  auto total = floor_total_;
  if (floored_)
    total += std::chrono::steady_clock::now() - floored_at_;

  const auto secs =
      std::chrono::duration_cast<std::chrono::seconds>(total).count();

  ss << "Refresh governor          : "
     << (floored_ ? "on the floor" : "at native rate") << "\n"
     << "  time on the floor       : " << secs << "s over " << descents_
     << " descents\n";
}

}  // namespace hwc
}  // namespace android
