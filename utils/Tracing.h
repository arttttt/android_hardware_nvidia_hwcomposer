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

#include <cutils/trace.h>

/* One trace marker this platform's tracing does not have yet.
 *
 * Where the others mark a span of time, this marks a moment -- something
 * happened, with nothing to measure. It arrived in a later release; what it
 * does there is exactly a span opened and closed at once, which is also how
 * such a moment is drawn, so that is what it is here.
 *
 * Guarded, so a newer platform's own definition wins and this file can be
 * deleted without anything else changing.
 */
#ifndef ATRACE_INSTANT
#define ATRACE_INSTANT(name)              \
  do {                                    \
    atrace_begin(ATRACE_TAG, (name));     \
    atrace_end(ATRACE_TAG);               \
  } while (0)
#endif
