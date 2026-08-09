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

namespace android::drm_hwcomposer {

/* What the display controller calls what everything else calls by its
 * four-character code. Zero for a format it cannot scan out, which is how the
 * caller learns to leave that layer to the framework.
 *
 * The pairing is not guessable from the names: one side names a format by the
 * order of its bytes in memory, the other by the order of its channels in a
 * word, so the two read as mirror images of each other and agree anyway.
 */
uint32_t TegraFormatFromDrm(uint32_t drm_format);

/* Turns a format modifier into the flags the controller wants and, where the
 * memory is arranged in blocks, how tall one is. False for an arrangement
 * this controller cannot read.
 */
bool TegraLayoutFromModifier(uint64_t modifier, uint32_t *out_flags,
                             uint8_t *out_block_height_log2);

}  // namespace android::drm_hwcomposer
