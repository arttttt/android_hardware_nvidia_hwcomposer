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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "tegra/MergeDescription.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include <cutils/trace.h>

#include "utils/Tracing.h"
#include "utils/fence.h"

namespace android {
namespace hwc {

namespace {

using Merge = drm_hwcomposer::TegraAtomicRequest::Merge;
using drm_hwcomposer::FenceState;
using drm_hwcomposer::QueryFence;

uint32_t SpanOf(float from, float to) {
  const float span = to - from;
  return span > 0.0F ? static_cast<uint32_t>(lroundf(span)) : 0;
}

uint32_t SpanOf(int32_t from, int32_t to) {
  return to > from ? static_cast<uint32_t>(to - from) : 0;
}

}  // namespace

std::vector<governor::Member> DescribeMembers(const Merge &merge) {
  std::vector<governor::Member> members;
  members.reserve(merge.layers.size());
  for (size_t i = 0; i < merge.layers.size(); ++i) {
    const VicSession::Layer &l = merge.layers[i];
    governor::Member m{};
    m.src_w = SpanOf(l.source_left, l.source_right);
    m.src_h = SpanOf(l.source_top, l.source_bottom);
    m.dst_w = SpanOf(l.display_left, l.display_right);
    m.dst_h = SpanOf(l.display_top, l.display_bottom);
    m.format = i < merge.formats.size() ? merge.formats[i] : 0;
    m.transform = i < merge.transforms.size() ? merge.transforms[i] : 0;
    m.premultiplied = l.premultiplied;
    /* No fence is a buffer handed over ready; one that cannot be asked
     * about reads as not yet. */
    const FenceState fence = QueryFence(l.acquire_fence);
    m.acquire_signaled = fence == FenceState::kNone || fence == FenceState::kSignaled;
    members.push_back(m);
  }
  return members;
}

bool CalibrationWanted() {
  return ATRACE_ENABLED();
}

void MarkJudged(const char *engine, bool late, uint64_t pixels,
                int64_t predicted_ns, int64_t deadline_ns) {
  if (!ATRACE_ENABLED())
    return;
  char line[112];
  snprintf(line, sizeof(line),
           "hwc_judge %s late=%d kpx=%" PRIu64 " pred=%" PRId64
           "us dl=%" PRId64 "us",
           engine, late ? 1 : 0, pixels / 1000, predicted_ns / 1000,
           deadline_ns / 1000);
  ATRACE_INSTANT(line);
}

void MarkDropped(const char *engine) {
  if (!ATRACE_ENABLED())
    return;
  char line[48];
  snprintf(line, sizeof(line), "hwc_drop %s", engine);
  ATRACE_INSTANT(line);
}

void MarkSteeringForCalibration(const drm_hwcomposer::FrameNote &note,
                                size_t merge_width, uint64_t seq) {
  if (!ATRACE_ENABLED() || note.runs.empty() || merge_width == 0)
    return;

  char line[256];
  int n = snprintf(line, sizeof(line),
                   "hwc_steer seq=%" PRIu64 " n=%zu len=%u chosen=%u runs=",
                   seq, merge_width, note.run_len, note.chosen_run);
  for (const auto &run : note.runs) {
    if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - 32)
      break;
    n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                  "%u:%u:%" PRIu64 ":%u:%u,", run.begin, run.live,
                  run.pixels / 1000, run.scale_pct, run.seatable ? 1U : 0U);
  }
  ATRACE_INSTANT(line);
}

void MarkMergeForCalibration(const Merge &merge, uint64_t seq) {
  if (!ATRACE_ENABLED())
    return;

  uint64_t src = 0;
  uint64_t dst = 0;
  for (const VicSession::Layer &l : merge.layers) {
    src += static_cast<uint64_t>(SpanOf(l.source_left, l.source_right)) *
           SpanOf(l.source_top, l.source_bottom);
    dst += static_cast<uint64_t>(SpanOf(l.display_left, l.display_right)) *
           SpanOf(l.display_top, l.display_bottom);
  }
  const unsigned turn = merge.transforms.empty() ? 0U : merge.transforms[0];

  char line[224];
  int n = snprintf(line, sizeof(line),
                   "hwc_merge_calib seq=%" PRIu64 " n=%zu src=%" PRIu64
                   " dst=%" PRIu64 " turn=%u fmt=",
                   seq, merge.layers.size(), src, dst, turn);
  for (uint32_t format : merge.formats) {
    if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - 12)
      break;
    n += snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%08x,",
                  format);
  }
  ATRACE_INSTANT(line);
}

}  // namespace hwc
}  // namespace android
