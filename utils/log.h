/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifdef ANDROID

#include <log/log.h>
#include <log/log_main.h>  // IWYU pragma: export

#else

#include <cinttypes>
#include <cstdio>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGE(args...) printf("ERR: " args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGW(args...) printf("WARN: " args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGI(args...) printf("INFO: " args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGD(args...) printf("DBG:" args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGV(args...) printf("VERBOSE: " args)

#endif

#ifdef ANDROID

namespace android {
namespace hwc {

/* Whether this composer should write to the log at all.
 *
 * Answered once, on the first asking; changing it afterwards takes a restart.
 * A question asked on every line would itself be part of what the lines cost.
 */
bool LoggingWanted();

}  // namespace hwc
}  // namespace android

/* Everything this composer says goes through here, and nothing is said unless
 * it was asked for:
 *
 *     setprop vendor.hwc.log 1     (in /vendor/build.prop, read at start-up)
 *
 * Writing a line is a message to another process. On the per-frame path that
 * came to three and a half milliseconds of a sixteen millisecond frame on this
 * hardware -- taken out of the client's share of it, and quietly distorting
 * every measurement made through it. The paths that only speak when something
 * is wrong cost nothing while nothing is wrong, but a fault that repeats turns
 * one of them into the per-frame path without anyone choosing that.
 *
 * These redefine the platform's own macros rather than introducing new ones so
 * that every call site is covered, including the ones inherited from upstream,
 * without a hundred edits that would each be a chance to miss one.
 *
 * The condition is inside the statement rather than around the call, so the
 * compiler still checks every format string against its arguments whatever the
 * switch says. ALOGV is left alone: the build already removes it.
 */
#undef ALOGE
#undef ALOGW
#undef ALOGI
#undef ALOGD
#undef ALOGE_IF
#undef ALOGW_IF

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HWC_LOG_AT(priority, ...)                                     \
  do {                                                                \
    if (::android::hwc::LoggingWanted())                              \
      __android_log_print((priority), LOG_TAG, __VA_ARGS__);          \
  } while (0)

#define ALOGE(...) HWC_LOG_AT(ANDROID_LOG_ERROR, __VA_ARGS__)
#define ALOGW(...) HWC_LOG_AT(ANDROID_LOG_WARN, __VA_ARGS__)
#define ALOGI(...) HWC_LOG_AT(ANDROID_LOG_INFO, __VA_ARGS__)
#define ALOGD(...) HWC_LOG_AT(ANDROID_LOG_DEBUG, __VA_ARGS__)

#define ALOGE_IF(cond, ...)                        \
  do {                                             \
    if (cond)                                      \
      ALOGE(__VA_ARGS__);                          \
  } while (0)

#define ALOGW_IF(cond, ...)                        \
  do {                                             \
    if (cond)                                      \
      ALOGW(__VA_ARGS__);                          \
  } while (0)
// NOLINTEND(cppcoreguidelines-macro-usage)

#endif
