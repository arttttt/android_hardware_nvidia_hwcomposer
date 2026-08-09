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

#ifndef UTILS_LOGGING_H
#define UTILS_LOGGING_H

/* The platform's own logging. Named Logging.h rather than Log.h on purpose:
 * this directory is on the include path, so a file of ours called utils/Log.h
 * would shadow the system header and then include itself. */
#include <utils/Log.h>

/* Errors and one-off state changes. Always emitted -- they are rare, and a
 * composer that fails silently is a composer nobody can diagnose. */
#define HWC_LOGE(...) ALOGE(__VA_ARGS__)
#define HWC_LOGW(...) ALOGW(__VA_ARGS__)
#define HWC_LOGI(...) ALOGI(__VA_ARGS__)

/* Per-frame and per-call detail: what a plan contained, which descriptors
 * went where, what the hardware answered. Off by default, because at sixty
 * frames a second it drowns everything else in the log.
 *
 * Switched with one line in the build:
 *     LOCAL_CFLAGS += -DHWC_TRACE_ENABLED=1
 *
 * The call stays inside `if (false)` when disabled rather than being cut by
 * the preprocessor, so the compiler still checks the format string against
 * its arguments and then drops the whole statement. Tracing that stops
 * compiling the moment it is switched off is tracing that rots.
 */
#ifndef HWC_TRACE_ENABLED
#define HWC_TRACE_ENABLED 0
#endif

#define HWC_LOGD(...)                                                         \
    do {                                                                      \
        if (HWC_TRACE_ENABLED)                                                \
            ALOGD(__VA_ARGS__);                                               \
    } while (0)

#endif  // UTILS_LOGGING_H
