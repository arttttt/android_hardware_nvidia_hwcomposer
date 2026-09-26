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

namespace android::hwc::governor {

class GovernorHost;

namespace tegra {

/* The host's log, with formatting. `prio` is an android_LogPriority. */
void Logf(GovernorHost &host, int prio, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The same, for a call that failed with `err` from errno. */
void LogErrno(GovernorHost &host, int prio, const char *what, int err);

}  // namespace tegra
}  // namespace android::hwc::governor
