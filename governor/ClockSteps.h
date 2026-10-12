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

#include <stddef.h>
#include <stdint.h>

namespace android::hwc::governor::tegra {

/* The engine's clock steps on this chip, megahertz. The kernel rounds a
 * request to one of these itself; the table is here so the decision can be
 * made and traced in the same terms, and so "one step above" means one
 * step. */
inline constexpr uint32_t clockStepsMhz[] = {180, 258, 336, 378, 420, 462,
                                             504, 552, 600, 684, 720, 756};
constexpr uint32_t topStepMhz = 756;

/* An engine's clock steps, lowest first, and the highest one this governor
 * may ask for: the same decision is made against any engine's ladder. */
struct ClockLadder {
  const uint32_t *steps;
  size_t count;
  uint32_t cap_mhz; /* nought: the top step */

  /* The highest step that may be asked for. */
  uint32_t Top() const {
    uint32_t top = 0;
    for (size_t i = 0; i < count; ++i)
      if (cap_mhz == 0 || steps[i] <= cap_mhz)
        top = steps[i];
    return top;
  }

  /* The smallest step up to the top that covers `need_mhz`, or nought if
   * none does. */
  uint32_t AtLeast(uint32_t need_mhz) const {
    const uint32_t top = Top();
    for (size_t i = 0; i < count && steps[i] <= top; ++i)
      if (steps[i] >= need_mhz)
        return steps[i];
    return 0;
  }
};

inline constexpr ClockLadder vicLadder = {
    clockStepsMhz, sizeof(clockStepsMhz) / sizeof(clockStepsMhz[0]), 0};

/* The smallest step that covers `need_mhz`, or nought if none does. */
inline uint32_t StepAtLeast(uint32_t need_mhz) {
  return vicLadder.AtLeast(need_mhz);
}

}  // namespace android::hwc::governor::tegra
