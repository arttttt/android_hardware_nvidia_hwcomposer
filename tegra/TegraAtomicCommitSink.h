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

#include <utility>
#include <vector>

#include "display/AtomicCommitSink.h"
#include "display/AtomicStateManager.h"

namespace android::drm_hwcomposer {

/* Where a frame for every display goes at once.
 *
 * Upstream merges the displays' requests into one atomic commit and sends it
 * down a single file descriptor, which is what makes a change across two
 * screens land on the same blank. This controller has no such call: a head is
 * flipped through its own device, and two heads are two flips.
 *
 * So the batch is carried out one display at a time. That is not a weaker
 * version of the same thing and should not be read as one -- there is no
 * arrangement of these ioctls that would make two heads change together. What
 * the interface still buys is that the decision to commit is taken once, in
 * one place, for all displays, and that a display which cannot take its frame
 * is found out before any of them is shown anything.
 *
 * On this board it is moot either way: there is one head.
 */
class TegraAtomicCommitSink : public AtomicCommitSink {
 public:
  CommitStatus TestAtomicCommit(
      const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>>
          &args) const override;

  CommitStatusOr<std::vector<std::pair<AtomicStateManager *,
                                       AtomicCommitResult>>>
  ExecuteAtomicCommit(
      const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>>
          &args) override;
};

}  // namespace android::drm_hwcomposer
