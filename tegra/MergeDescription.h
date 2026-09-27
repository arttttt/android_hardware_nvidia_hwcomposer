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

#include <vector>

#include "governor/HwcGovernor.h"
#include "tegra/TegraAtomicStateManager.h"

namespace android {
namespace hwc {

/* A planned merge, said in the governor's terms.
 *
 * Both readings of the same request: one for the governor at validate, one
 * for the trace at execute. Kept together so the extents the governor
 * decides by and the extents the trace is calibrated against are
 * measured the same way. */

/* The members as the governor takes them: extents rounded to pixels, the
 * buffer's own format, the turn, and whether the acquire fence has come
 * due -- asked of the kernel without waiting. */
std::vector<governor::Member> DescribeMembers(
    const drm_hwcomposer::TegraAtomicRequest::Merge &merge);

/* One trace moment before the merge goes to the engine, carrying what it
 * is made of: the key by which the governor's cost model is calibrated
 * against the engine's own accounting of the job. Nothing if tracing is
 * off. */
void MarkMergeForCalibration(
    const drm_hwcomposer::TegraAtomicRequest::Merge &merge, uint64_t seq);

/* The runs the planner weighed for this frame's merge and the one it took,
 * as an instant in the trace: "hwc_steer seq=N n=W len=L chosen=K
 * runs=b:l:kpx:s:v,..." -- begin, drawing layers, pixels in thousands,
 * steepest resize in percent, whether it could have been seated. Nothing
 * when there was no merge or no choice. */
void MarkSteeringForCalibration(const drm_hwcomposer::FrameNote &note,
                                size_t merge_width, uint64_t seq);

}  // namespace hwc
}  // namespace android
