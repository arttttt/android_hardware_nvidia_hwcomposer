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

namespace android::hwc::governor {

class GovernorHost;

namespace tegra {

/* What the policy is tuned by.
 *
 * The defaults are the measured ones and live here. A file on the device
 * overrides any of them with lines of `name=value` -- the names are the
 * fields below, verbatim -- read once when the library starts. No file is
 * the usual answer. */
struct Tuning {
  /* The model's safety margin, per cent. Underestimating the merge by a
   * third loses nearly every rescue; overestimating by a third keeps them
   * at the cost of a few needless requests. */
  uint32_t factor_pct = 125;

  /* Pixels the engine moves per clock. Two on this chip. */
  uint32_t pixels_per_clock = 2;

  /* The floor asked of a cold engine when the model wants less: until the
   * model is calibrated its underestimates are covered from here. Nought
   * trusts the model. */
  uint32_t min_cold_mhz = 504;

  /* How long the floor outlives the last merge's fence, so the merges of
   * one transition do not each pay for the clock coming and going. */
  uint32_t hold_ms = 17;

  /* After which the engine is taken to have powered down. The kernel's
   * powergate delay. */
  uint32_t powergate_ms = 500;

  /* The processor floor's step, kilohertz, and how long it may be held.
   * Nought as the step lifts the processor never. */
  uint32_t cpu_khz = 1044000;
  uint32_t cpu_cap_ms = 5;

  /* The path from validate to the submit, measured: the composer's lead
   * and the submit itself on a warm engine, on a cold one with the
   * processor lifted, and on a cold one without. */
  uint32_t lead_us = 1500;
  uint32_t submit_warm_us = 250;
  uint32_t submit_cold_us = 1600;
  uint32_t submit_cold_slow_us = 4000;

  /* How far before the latch the merge has to be done. */
  uint32_t latch_margin_us = 700;

  /* Fallbacks for a frame planned before the composer has a vsync phase. */
  uint32_t budget_empty_us = 7200;
  uint32_t budget_waited_us = 23000;

  /* How long a fence is waited for before it is given up on, and how long a
   * floor is kept when the merge it was raised for never reports. */
  uint32_t fence_patience_ms = 500;
  uint32_t orphan_ms = 100;

  /* How often the performance profile is asked for. */
  uint32_t profile_poll_ms = 250;
};

constexpr const char *tuningPath = "/vendor/etc/hwc_governor.conf";

/* Reads `path` over `tuning`, field by field. A line that names nothing or
 * carries no number is said in the log and skipped. False if there was no
 * file to read, which is not a fault. */
bool ReadTuningFile(const char *path, Tuning *tuning, GovernorHost &log);

}  // namespace tegra
}  // namespace android::hwc::governor
