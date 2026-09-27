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

#include "governor/Tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#include "governor/HwcGovernor.h"
#include "governor/Log.h"

namespace android::hwc::governor::tegra {

namespace {

struct TuningKey {
  const char *name;
  uint32_t Tuning::*field;
};

constexpr TuningKey tuningKeys[] = {
    {"factor_pct", &Tuning::factor_pct},
    {"pixels_per_clock", &Tuning::pixels_per_clock},
    {"job_kcycles", &Tuning::job_kcycles},
    {"min_cold_mhz", &Tuning::min_cold_mhz},
    {"hold_ms", &Tuning::hold_ms},
    {"powergate_ms", &Tuning::powergate_ms},
    {"cpu_khz", &Tuning::cpu_khz},
    {"cpu_cap_ms", &Tuning::cpu_cap_ms},
    {"lead_us", &Tuning::lead_us},
    {"submit_warm_us", &Tuning::submit_warm_us},
    {"submit_cold_us", &Tuning::submit_cold_us},
    {"submit_cold_slow_us", &Tuning::submit_cold_slow_us},
    {"lead_min_us", &Tuning::lead_min_us},
    {"latch_margin_us", &Tuning::latch_margin_us},
    {"phase_max_age_periods", &Tuning::phase_max_age_periods},
    {"next_submit_us", &Tuning::next_submit_us},
    {"budget_empty_us", &Tuning::budget_empty_us},
    {"budget_waited_us", &Tuning::budget_waited_us},
    {"fence_patience_ms", &Tuning::fence_patience_ms},
    {"orphan_ms", &Tuning::orphan_ms},
    {"profile_poll_ms", &Tuning::profile_poll_ms},
};

/* Strips blanks and a trailing comment; returns the first byte of what is
 * left, which is a NUL for a line with nothing on it. */
char *Trim(char *line) {
  char *hash = strchr(line, '#');
  if (hash != nullptr)
    *hash = '\0';
  while (*line == ' ' || *line == '\t')
    ++line;
  char *end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\n' || end[-1] == '\r'))
    *--end = '\0';
  return line;
}

void Complain(GovernorHost &log, const char *path, unsigned line,
              const char *what, const char *detail) {
  Logf(log, ANDROID_LOG_WARN, "%s:%u: %s%s%s", path, line, what,
       detail != nullptr ? ": " : "", detail != nullptr ? detail : "");
}

}  // namespace

bool ReadTuningFile(const char *path, Tuning *tuning, GovernorHost &log) {
  FILE *file = fopen(path, "re");
  if (file == nullptr)
    return false;

  char line[160];
  unsigned number = 0;
  while (fgets(line, sizeof(line), file) != nullptr) {
    ++number;
    char *text = Trim(line);
    if (*text == '\0')
      continue;

    char *equals = strchr(text, '=');
    if (equals == nullptr) {
      Complain(log, path, number, "not a name=value line", nullptr);
      continue;
    }
    *equals = '\0';
    const char *name = Trim(text);
    const char *value = Trim(equals + 1);

    /* strtoul takes a sign and wraps; a setting here is never negative. */
    char *rest = nullptr;
    const unsigned long parsed = strtoul(value, &rest, 10);
    if (*value == '-' || *value == '+' || rest == value || *rest != '\0' ||
        parsed > UINT32_MAX) {
      Complain(log, path, number, "not a number", value);
      continue;
    }

    bool known = false;
    for (const TuningKey &key : tuningKeys) {
      if (strcmp(key.name, name) == 0) {
        tuning->*key.field = static_cast<uint32_t>(parsed);
        known = true;
        break;
      }
    }
    if (!known)
      Complain(log, path, number, "no such setting", name);
  }
  fclose(file);

  /* Two that divide. */
  if (tuning->factor_pct == 0)
    tuning->factor_pct = 100;
  if (tuning->pixels_per_clock == 0)
    tuning->pixels_per_clock = 1;
  return true;
}

}  // namespace android::hwc::governor::tegra
