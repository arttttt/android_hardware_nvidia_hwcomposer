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

namespace android::hwc::governor::tegra {

/* The engine's clock steps on this chip, megahertz. The kernel rounds a
 * request to one of these itself; the table is here so the decision can be
 * made and traced in the same terms, and so "one step above" means one
 * step. */
inline constexpr uint32_t clockStepsMhz[] = {180, 258, 336, 378, 420, 462,
                                             504, 552, 600, 684, 720, 756};
constexpr uint32_t topStepMhz = 756;

/* The smallest step that covers `need_mhz`, or nought if none does. */
inline uint32_t StepAtLeast(uint32_t need_mhz) {
  for (uint32_t step : clockStepsMhz)
    if (step >= need_mhz)
      return step;
  return 0;
}

}  // namespace android::hwc::governor::tegra
