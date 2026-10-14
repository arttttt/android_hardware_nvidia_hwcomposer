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

#include <cstdint>

namespace android::hwc::governor {

/* Keeping an engine up through a stretch of frames that do not use it.
 *
 * The kernel powers an engine down half a second after its last job, and
 * the job that finds it down pays for its coming up: on the device a
 * screen merged in 7 ms instead of 3.8, the GPU drew in 8.6 ms instead of
 * 1-2. Two in five of the cold merges of a transition came while frames
 * were flowing all along -- the screen alive, the engine not wanted for
 * half a second to a second and a quarter, and down by the time it was.
 *
 * So while frames come and the engine did real work not long ago, it is
 * given a tiny job shortly before it would power down. Only while it is
 * still up: waking a powered-down engine ramps the core rail under the
 * clock lock the flips wait on, which is what made warming ahead of a
 * transition cost more than it saved. A still screen asks nothing, so the
 * engine powers down as before; past the window after its last real work,
 * neither.
 *
 * Plain state, no clock of its own and no lock: its owner calls it from
 * one thread with the time it already has.
 */
class KeepWarm {
 public:
  /* `window_ns` after the last real work it is kept up, nought for never;
   * a tiny job is due `lead_ns` after the engine's last job, and no longer
   * once `powergate_ns` have passed. */
  KeepWarm(int64_t window_ns, int64_t lead_ns, int64_t powergate_ns)
      : window_ns_(window_ns), lead_ns_(lead_ns), powergate_ns_(powergate_ns) {
  }

  void Worked(int64_t now_ns) {
    last_work_ns_ = now_ns;
    last_up_ns_ = now_ns;
  }
  void Warmed(int64_t now_ns) { last_up_ns_ = now_ns; }

  /* Whether a frame at `now_ns` should give the engine its tiny job. */
  bool Due(int64_t now_ns) const {
    if (window_ns_ <= 0 || last_work_ns_ == 0 ||
        now_ns - last_work_ns_ >= window_ns_)
      return false;
    const int64_t idle = now_ns - last_up_ns_;
    return idle >= lead_ns_ && idle < powergate_ns_;
  }

  /* The engine's last job, real or tiny; nought before any. */
  int64_t last_up_ns() const { return last_up_ns_; }

 private:
  int64_t window_ns_;
  int64_t lead_ns_;
  int64_t powergate_ns_;
  int64_t last_work_ns_ = 0;
  int64_t last_up_ns_ = 0;
};

}  // namespace android::hwc::governor
