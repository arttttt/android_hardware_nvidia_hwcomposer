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

#include <stdint.h>

/* The composition load governor: the one header shared by the composer and
 * the library that implements the policy.
 *
 * The composer knows nothing about engine clocks, nvhost or pm_qos. What it
 * knows is when a frame with a merge was planned, when the merge went to
 * the engine, and when the frame was posted -- and it says so, in plain
 * numbers, to whatever implements Governor below. Everything that turns
 * those numbers into a clock request lives in a separate library,
 * hwcgovernor.<platform>.so, loaded by name at pipeline creation; a device
 * without the library runs exactly as it did before.
 *
 * Both sides are built by one toolchain in one tree, so pure virtual classes
 * are an acceptable boundary. Nothing here is a standard-library type and
 * nothing throws: plain data and virtual calls only. Any change to what is
 * declared here raises apiVersion, and a library that answers with another
 * version is not loaded.
 */

namespace android::hwc::governor {

constexpr uint32_t apiVersion = 2;

/* The display's power mode, numbered as the composer's own enumeration. */
enum class Power : uint8_t {
  off = 0,
  doze = 1,
  dozeSuspend = 2,
  suspend = 3,
  on = 4,
};

/* One member of the merge, in flat numbers. */
struct Member {
  uint32_t src_w;
  uint32_t src_h;
  uint32_t dst_w;
  uint32_t dst_h;
  uint32_t format;     /* DRM_FORMAT_* fourcc, as the buffer describes itself */
  uint8_t transform;   /* hflip | vflip << 1 | rotate90 << 2 */
  bool premultiplied;
  /* Whether the member's acquire fence had already come due when the frame
   * was planned. Recorded for the trace only in this version: with
   * unsignaled latching the input often finishes drawing a millisecond or
   * three later, and how strictly to treat it is decided by measurement. */
  bool acquire_signaled;
};

/* What the composer decided at validate, as far as the governor cares.
 * Copied whole by the governor inside FramePlanned. */
struct Frame {
  /* Grows on every successful test of a frame. The composer keeps the
   * number of the last successful test and repeats it in MergeSubmitted and
   * FramePresented; a present without a validate in between repeats the
   * previous number, which is not an error. */
  uint64_t seq;

  int64_t now_ns;

  /* When the display last latched a frame: the moment the previous flip's
   * fence came due, which is the controller's own report of scanout
   * starting on that frame. The latches the governor aims at are this
   * moment plus whole periods. Nought before the first landed flip; the
   * framework's vsync timestamps were tried first and turned out not to
   * be locked to the latch at all. */
  int64_t last_latch_ns;
  int64_t vsync_period_ns;

  /* When the composer last used the engine for anything -- a merge or the
   * cursor's staging copy -- or nought if it never has. The governor adds
   * its own last warm-up and decides from the two whether the engine has
   * been powered down since. */
  int64_t last_engine_use_ns;

  bool merge_planned;          /* the frame carries a merge */
  bool merge_reuse_predicted;  /* same sources and shape as the last one
                                  drawn: the engine will not be woken */
  bool previous_flip_landed;   /* the previous frame's fence has come due,
                                  so this one targets the nearest latch */
  uint8_t power_mode;          /* Power, as a number */

  uint32_t target_w;
  uint32_t target_h;

  /* Valid only for the duration of FramePlanned. */
  const Member *members;
  uint32_t member_count;
};

/* What the composer offers the governor. Called from the governor's own
 * thread; every method is safe to call from there. */
class GovernorHost {
 public:
  virtual ~GovernorHost() = default;

  /* Runs one tiny pass on the engine, eight by eight pixels between two
   * buffers of the composer's own, so that a powered-down engine pays for
   * coming up now rather than on the merge. Returns the pass's fence, owned
   * by the caller, or -1 if the engine would not take it. */
  virtual int WarmEngine() = 0;

  virtual void TraceInt(const char *name, int32_t value) = 0;

  /* `prio` is an android_LogPriority. */
  virtual void Log(int prio, const char *msg) = 0;
};

/* What the library implements. No call blocks and none takes a lock of the
 * composer's: each copies what it was given and returns. */
class Governor {
 public:
  virtual ~Governor() = default;

  /* Validate, after the controller accepted the frame. */
  virtual void FramePlanned(const Frame &frame) = 0;

  /* The merge went to the engine. `merge_fence_fd` is a duplicate of the
   * engine's fence and belongs to the governor, which closes it. */
  virtual void MergeSubmitted(uint64_t seq, int merge_fence_fd) = 0;

  /* The frame was posted to the controller. */
  virtual void FramePresented(uint64_t seq) = 0;

  virtual void PowerMode(Power mode) = 0;
};

}  // namespace android::hwc::governor

/* The library's only exported symbols, with C linkage for dlsym. */
extern "C" {
uint32_t hwc_governor_api_version();
android::hwc::governor::Governor *hwc_governor_create(
    android::hwc::governor::GovernorHost *host);
/* Synchronous: stops and joins whatever the governor runs, closes every
 * descriptor it was handed, and only then returns. The host outlives it. */
void hwc_governor_destroy(android::hwc::governor::Governor *governor);
}
