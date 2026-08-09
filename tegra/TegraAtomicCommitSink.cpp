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

#include "tegra/TegraAtomicCommitSink.h"

#include <cerrno>
#include <cinttypes>
#include <memory>
#include <utility>
#include <vector>

#include "display/CommitStatus.h"
#include "tegra/TegraAtomicStateManager.h"
#include "utils/Logging.h"
#include "utils/Time.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {

/* Turns every display's arguments into something its controller would take,
 * and says so if any of them cannot be.
 *
 * Following upstream: a display whose arguments ask for nothing is passed
 * over rather than refused, and a display that asks for something it cannot
 * have fails the whole batch. Deciding for all of them before acting on any
 * is the point of doing this in one place.
 */
bool BuildRequests(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>> &args,
    std::vector<std::pair<TegraAtomicStateManager *,
                          std::unique_ptr<AtomicRequest>>> *out) {
  for (auto &[state_manager, arg] : args) {
    if (!arg.HasInputs())
      continue;

    /* Safe: this sink is handed out by the same backend that builds the
     * state managers, so nothing else can be in this list. */
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto *tegra = static_cast<TegraAtomicStateManager *>(state_manager);

    /* A copy, because building a request may fill in what the caller asked
     * to be told. Upstream takes the same liberty. */
    AtomicCommitArgs args_copy = arg;

    auto request = tegra->GetAtomicModeReqForArgs(args_copy);
    if (!request) {
      ALOGE("Failed to create request.");
      return false;
    }

    out->emplace_back(tegra, std::move(request));
  }

  return true;
}

}  // namespace

CommitStatus TegraAtomicCommitSink::TestAtomicCommit(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>> &args)
    const {
  if (args.empty())
    return CommitStatus::InternalFailure();

  std::vector<std::pair<TegraAtomicStateManager *,
                        std::unique_ptr<AtomicRequest>>>
      requests;

  if (!BuildRequests(args, &requests))
    return CommitStatus::InternalFailure();

  if (requests.empty()) {
    ALOGD("Committing no input, success.");
    return CommitStatus::Success();
  }

  for (const auto &[state_manager, request] : requests) {
    if (!state_manager->Test(*request))
      return CommitStatus::Failure(-EINVAL);
  }

  return CommitStatus::Success();
}

CommitStatusOr<std::vector<std::pair<AtomicStateManager *, AtomicCommitResult>>>
TegraAtomicCommitSink::ExecuteAtomicCommit(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>>
        &args) {
  using Results = std::vector<
      std::pair<AtomicStateManager *, AtomicCommitResult>>;

  if (args.empty())
    return CommitStatusOr<Results>(CommitStatus::InternalFailure());

  std::vector<std::pair<TegraAtomicStateManager *,
                        std::unique_ptr<AtomicRequest>>>
      requests;

  /* Describing the frame and carrying it out, timed apart. Everything above
   * here has been measured and costs nothing; the composer's own work at the
   * bottom is about a millisecond, and this whole call is five. What is
   * between those two is this. */
  const int64_t before_build = GetTimeMonotonicNs();

  if (!BuildRequests(args, &requests))
    return CommitStatusOr<Results>(CommitStatus::InternalFailure());

  const int64_t after_build = GetTimeMonotonicNs();

  if (requests.empty()) {
    ALOGD("Committing no input, success.");
    return CommitStatusOr<Results>(Results{});
  }

  for (const auto &[state_manager, _] : args)
    state_manager->WaitLastFrame();

  Results results;
  results.reserve(requests.size());

  for (const auto &[state_manager, request] : requests) {
    AtomicCommitResult result;

    int err = state_manager->Execute(*request, &result);
    if (err) {
      /* Whatever went up before this one is on its panel and stays there.
       * There is nothing to undo it with, and pretending otherwise by
       * carrying on would leave the caller with fences for frames that were
       * never shown. */
      ALOGE("Failed to commit: %d", err);
      return CommitStatusOr<Results>(CommitStatus::Failure(err));
    }

    results.emplace_back(state_manager, std::move(result));
  }

  const int64_t after_execute = GetTimeMonotonicNs();
  if (after_execute - before_build > 3000000) {
    HWC_LOGD("slow execute: describing %" PRId64 "us, carrying out %" PRId64
             "us",
             (after_build - before_build) / 1000,
             (after_execute - after_build) / 1000);
  }

  return CommitStatusOr<Results>(results);
}

}  // namespace android::drm_hwcomposer
