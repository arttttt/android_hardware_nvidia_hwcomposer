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

#ifndef TEGRA_REFRESH_GOVERNOR_H
#define TEGRA_REFRESH_GOVERNOR_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <thread>

namespace android {
namespace hwc {

/* Slows the panel when nobody is drawing.
 *
 * A still screen still scans: sixty times a second the controller reads
 * three megapixels nobody changed. The panel was calibrated by eye down
 * its own ladder and holds thirty hertz cleanly -- below that the pixels'
 * charge visibly leaks between refreshes -- so a screen that has been
 * quiet for a while is worth exactly half its scanout.
 *
 * Quiet is declared by the framework itself: SurfaceFlinger switches its
 * vsync callbacks off when its timing model is settled and nothing wants
 * frames. That signal arms a clock; a second of silence later the porch
 * stretches. Waking is the mirror and must come first: the callbacks
 * switching back on precede the first timing sample the framework takes,
 * so the panel is back at sixty before anyone measures it -- a late rise
 * costs two hundred milliseconds of misjudged frame pacing on every
 * wake. The pointer wakes it too: the cursor moves without frames by
 * design, and a pointer at thirty hertz drags.
 *
 * The kernel side is fire-and-forget: the request lands at a frame's
 * end, nothing is waited for, nothing comes back. A kernel without the
 * door says so once at probe, and the governor simply never starts.
 */
class RefreshGovernor {
 public:
  /* The governor, or null having said why not: a kernel without the
   * ioctl, or a head that never woke its stretch machinery. */
  static std::unique_ptr<RefreshGovernor> Probe(int dc_fd);

  ~RefreshGovernor();

  RefreshGovernor(const RefreshGovernor &) = delete;
  RefreshGovernor &operator=(const RefreshGovernor &) = delete;

  /* Somebody drew, or is about to: a frame executed, the pointer moved.
   * Raises the panel back to its native rate before returning -- the
   * request is asynchronous in the kernel but instant to file -- and
   * restarts the quiet clock. Cheap on purpose: this sits on the
   * present path. */
  void NoteActivity();

  /* The framework's own word on whether anyone wants frames. Off arms
   * the descent; on is a wake and must raise before it returns, ahead
   * of the first vsync sample the framework will take. */
  void NoteVsyncEnabled(bool enabled);

  /* Whether a vsync timestamp taken now was born of a clean frame.
   * False inside the raise's shadow -- the stretch of time in which the
   * hardware may still be finishing a slowed frame after a wake. A
   * timestamp from that shadow, fed to the framework's timing model,
   * teaches it a thirty-hertz world that no longer exists; and the
   * model, once taught, self-confirms -- frames latch on every second
   * vblank of a healthy panel and the fences agree with the lie, for
   * seconds, until something forces a resample. Asked from the vsync
   * thread for every tick. */
  bool VsyncTimestampTrustworthy() const;

  /* For the dump: how long the panel has lived slow, and how often it
   * went there. Time, not frames -- on the floor there are no frames
   * to count. */
  void AppendDump(std::ostream &ss) const;

 private:
  explicit RefreshGovernor(int dc_fd);

  void ThreadFn();

  /* Both take lock_. RaiseLocked files the release and accounts the
   * floor time; callers hold the lock. */
  void RaiseLocked();
  void FileVfpLocked(uint32_t vfp);

  /* The panel's own floor, measured by eye down the calibration ladder:
   * the porch that makes an effective thirty hertz of this mode. A
   * property of the glass -- charge leak, temperature, age -- not of
   * the system; a different panel means a different number. */
  static constexpr uint32_t kFloorVfp = 2086;

  /* A second of silence before slowing. Longer than every settle and
   * transition cost measured on this panel, and long enough that the
   * status bar's once-a-minute clock costs one descent a minute, not a
   * flap. Doubles as the rate limit: transitions cannot come closer
   * than this by construction. */
  static constexpr std::chrono::milliseconds kQuietDelay{1000};

  /* How long after a raise a vsync timestamp may still carry the old,
   * stretched timing: the raise applies at a frame's end, so up to two
   * slowed frames can complete after the filing. The law, should the
   * ladder ever grow steps: at least two periods of the DEEPEST step
   * plus margin -- today two thirty-hertz frames, sixty-seven
   * milliseconds, with eight of slack. A deeper step silently outgrows
   * a shadow sized to this one. Ticks inside are not delivered. */
  static constexpr std::chrono::milliseconds kRaiseShadow{75};

  const int fd_;

  mutable std::mutex lock_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_ = false;

  /* The framework's callbacks are off -- its own declaration of idle.
   * Descent is armed only while this holds. */
  bool vsync_off_ = false;
  bool floored_ = false;
  std::chrono::steady_clock::time_point last_activity_;
  std::chrono::steady_clock::time_point floored_at_;
  std::chrono::steady_clock::time_point last_raise_;

  uint64_t descents_ = 0;
  std::chrono::nanoseconds floor_total_{0};
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_REFRESH_GOVERNOR_H
