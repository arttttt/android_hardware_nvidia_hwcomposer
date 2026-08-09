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

#include <system/graphics.h>

/* One colour mode this release's graphics definitions stop short of.
 *
 * A display whose primaries are the ones recommended for ultra-high
 * definition television but which still applies the ordinary transfer
 * function. It was added a release later, and the code above knows about it
 * -- the mode is refused there anyway, because a mode with that much range
 * has to be asked for while the timing is being set rather than afterwards.
 *
 * The value is the one the interface gives it and not a number chosen here:
 * hardware/interfaces/graphics/common/1.2/types.hal, enum ColorMode,
 * DISPLAY_BT2020 = 13.
 *
 * A macro rather than an enumerator, because the enumeration it belongs to is
 * defined by the platform and cannot be added to. Guarded, so a newer
 * platform's own definition wins.
 */
#ifndef HAL_COLOR_MODE_DISPLAY_BT2020
#define HAL_COLOR_MODE_DISPLAY_BT2020 13
#endif
