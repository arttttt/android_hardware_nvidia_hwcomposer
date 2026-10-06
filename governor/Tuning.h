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

  /* What a job costs before its first pixel, thousands of cycles: from
   * the calibration markers of twelve runs, the engine's cycles for a
   * merge fit a quarter of a million plus half a cycle per pixel, within
   * ten per cent either way on the large ones. */
  uint32_t job_kcycles = 250;

  /* The floor asked of a cold engine when the model wants less: until the
   * model is calibrated its underestimates are covered from here. Nought
   * trusts the model. */
  uint32_t min_cold_mhz = 504;

  /* The most asked of a cold engine before the composer's submit; the
   * rest is asked once the merge is reported. A higher step on a
   * just-powered engine sometimes ramps the core rail -- two to nine
   * milliseconds of the kernel stepping the regulator -- with the bus
   * clock's lock held, and the composer's own submit waits behind that
   * lock. Sometimes: two thirds of such requests cost under a
   * millisecond, and nothing the governor can see tells the two apart,
   * so capping cost more frames than it saved (14 of 211 missed against
   * 12 of 221 uncapped). Off by default -- the top step -- until the
   * kernel side of the ramp is settled; a lower value here caps. */
  uint32_t cold_cap_mhz = 756;

  /* The processor floor's step, kilohertz, and how long it may be held.
   * Nought as the step lifts the processor never. */
  uint32_t cpu_khz = 1044000;
  uint32_t cpu_cap_ms = 5;

  /* Below this clock the processor is lifted before the engine's floor
   * rather than after it. The floor's rail ramp is three I2C writes, each
   * served by an interrupt the idling processor takes about three
   * milliseconds to get to at its lowest clock and a tenth of one above
   * a gigahertz: nineteen of twenty-one ramps over two milliseconds were
   * taken at or under 312 MHz, one of eighty-three above 1.4 GHz. */
  uint32_t cpu_low_khz = 400000;

  /* A merge this long after the engine's last use is the first of a
   * transition: the processor has been compositing without a merge, held
   * at 830-900 MHz by its governor under that light load, and the
   * composer's share of the submit -- planning the merge, describing it,
   * the ioctl -- takes five to six milliseconds there against one and a
   * half in the middle of a transition. Such merges are measured apart
   * (see submit_measured). */
  uint32_t pause_ms = 100;

  /* Whether such a merge also lifts the processor as a cold one does.
   * Off: a lift to the cold rule's step buys a sixth of the clock over
   * 830-900 MHz and nothing measurable -- late merges 21 against 16,
   * hitches 10 against 11 over six runs each. */
  uint32_t cpu_pause_lift = 0;

  /* The path from validate to the submit, measured rather than assumed.
   * The composer reports the submit, stamped on its own thread, and the
   * governor keeps a running mean of the path in processor cycles --
   * the time it took times the clock the processor ran at -- and reads
   * it back at the clock of the moment. The path is the composer's own
   * work, and its length is the processor's clock: seven milliseconds at
   * or under 700 MHz, under two above 1.1 GHz, and a mean in
   * milliseconds sits three milliseconds under the merges that are late.
   * Cold merges are left out of the measure -- their path holds the
   * engine's power-up, not cycles -- and read as the warm path plus the
   * assumed cold submit. The assumed path stands in until there is a
   * measure, and when this is nought. `submit_smooth` weighs what came
   * before against the latest merge. */
  uint32_t submit_measured = 1;
  uint32_t submit_smooth = 3;

  /* How long the floor outlives the last merge's fence, so the merges of
   * one transition do not each pay for the clock coming and going. */
  uint32_t hold_ms = 17;

  /* After which the engine is taken to have powered down. The kernel's
   * powergate delay. */
  uint32_t powergate_ms = 500;

  /* Whether the first frame after a pause at least that long wakes the
   * engine though it carries no merge. On Android 11 the merges of a
   * transition come one at a time, half a second to two apart -- the
   * steered run holds still and the cache answers in between -- so four
   * in five of them found the engine down and paid for its coming up
   * inside the frame. Woken with the activity instead, it is up by the
   * time the transition's merge is planned. No clock is asked: the pass
   * runs at whatever the engine idles at.
   *
   * Off: it costs more than it saves. Two pairs of runs, interleaved, of
   * five launches from an icon: cold merges 17 -> 13 a run, but the
   * composer's present at p90 1.2-1.9 -> 6.5-9.4 ms, SurfaceFlinger's
   * frame at p90 5.6-6.3 -> 10.9-13.0 ms, frames it waited on the
   * previous one 13-31 -> 59-70. The pass lands as the animation starts,
   * the processor still at its idle clock, where bringing the engine up
   * ramps the core rail under the clock lock the flips wait on. */
  uint32_t warm_ahead = 0;

  /* The path from validate to the submit, measured: the composer's lead
   * and the submit itself on a warm engine, on a cold one with the
   * processor lifted, and on a cold one without. The cold ones are short
   * now that the governor wakes the engine before the composer gets
   * there: a quarter of a millisecond at the median, under one at the
   * ninetieth percentile on the device. */
  uint32_t lead_us = 1500;
  uint32_t submit_warm_us = 250;
  uint32_t submit_cold_us = 600;
  uint32_t submit_cold_slow_us = 1200;

  /* The shortest the path can be: what a latch is judged hopeless by. A
   * latch the merge cannot make at the top step even from here is left
   * for the next; one it might make is asked the top step for. */
  uint32_t lead_min_us = 1000;

  /* How far before the latch the merge has to be done. */
  uint32_t latch_margin_us = 700;

  /* How many periods old the last latch may be and still place the grid.
   * After a second of quiet the panel is slowed and re-phased, but the
   * re-phasing measured on the device is under three milliseconds, and a
   * grid extended across it beat the fallback budgets on the first frame
   * after the pause -- which is the frame the governor is for. Two
   * seconds, then; beyond that the fallbacks apply. */
  uint32_t phase_max_age_periods = 120;

  /* How soon after a latch the next frame's merge is expected at the
   * engine: the framework's offset and the composer's path. A slipped
   * merge has to be out of the engine by then -- the channel is one
   * queue, and a slow merge in it is the next frame's loss. */
  uint32_t next_submit_us = 6500;

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
