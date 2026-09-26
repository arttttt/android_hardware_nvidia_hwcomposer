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

#include <memory>
#include <mutex>

#include "governor/GovernorLibrary.h"
#include "governor/HwcGovernor.h"
#include "tegra/VicSession.h"

namespace android {
namespace hwc {

/* The composer's end of the composition load governor.
 *
 * Hands the policy library the composer's services -- the warm-up pass,
 * tracing, the log -- and forwards the frame events to it. Every call into
 * the library goes through one lock, and the same lock is what makes
 * destruction safe: once the library is being taken down, nothing further
 * is forwarded, and the library's own thread is joined before the engine
 * it may still be warming is closed. Owned by the pipeline, which destroys
 * it before the engine.
 *
 * Null where there is no library to load -- the composer then runs exactly
 * as it did without one.
 */
class CompositionGovernor final : public governor::GovernorHost {
 public:
  /* `vic` is borrowed and must outlive this; the warm-up pass runs on it.
   * Null if there is no engine, no library, or the wrong one. */
  static std::unique_ptr<CompositionGovernor> Load(VicSession *vic);

  ~CompositionGovernor() override;

  CompositionGovernor(const CompositionGovernor &) = delete;
  CompositionGovernor &operator=(const CompositionGovernor &) = delete;

  /* The events, forwarded. `fence_fd` is taken: it is handed to the
   * library, which closes it, or closed here if the library is gone. */
  void FramePlanned(const governor::Frame &frame);
  void MergeSubmitted(uint64_t seq, int fence_fd);
  void FramePresented(uint64_t seq);
  void PowerMode(governor::Power mode);

  /* GovernorHost, called from the library's thread. */
  int WarmEngine() override;
  void TraceInt(const char *name, int32_t value) override;
  void Log(int prio, const char *msg) override;

 private:
  CompositionGovernor(std::unique_ptr<GovernorLibrary> library,
                      VicSession *vic);

  bool Start();
  void AllocateWarmBuffers();

  std::unique_ptr<GovernorLibrary> library_;
  VicSession *const vic_;

  /* The two buffers the warm-up pass copies between. Allocated on the
   * main thread at creation, deliberately: the zone opens its device on
   * the first allocation, and two threads asking at once would race for
   * it. */
  std::unique_ptr<VendorBuffer> warm_source_;
  std::unique_ptr<VendorBuffer> warm_target_;

  /* Held across every call into the library. Cleared under the lock before
   * the library is destroyed, so a callback racing the teardown finds
   * nothing to call. */
  std::mutex callback_mutex_;
  governor::Governor *governor_ = nullptr;
};

}  // namespace hwc
}  // namespace android
