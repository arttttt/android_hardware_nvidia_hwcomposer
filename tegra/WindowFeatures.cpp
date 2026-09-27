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

#include "tegra/WindowFeatures.h"

namespace android {
namespace hwc {

namespace {

/* How the controller spells its feature table: the property codes and the
 * meaning of each value slot, read from the same kernel this builds
 * against. A mismatch would show as nonsense in the line the head logs at
 * start-up. */
constexpr uint32_t kFeatureFormats = 0;
constexpr uint32_t kFeatureMaximumSize = 2;
constexpr uint32_t kFeatureMaximumScale = 3;
constexpr uint32_t kFeatureLayoutType = 5;
constexpr uint32_t kFeatureInvertType = 6;

constexpr size_t kScaleUpH = 0;
constexpr size_t kScaleUpV = 1;
constexpr size_t kScaleDownH = 2;
constexpr size_t kScaleDownV = 3;

constexpr size_t kSizeMaxWidth = 0;
constexpr size_t kSizeMinWidth = 1;
constexpr size_t kSizeMaxHeight = 2;
constexpr size_t kSizeMinHeight = 3;

constexpr size_t kLayoutPitched = 0;
constexpr size_t kLayoutTiled = 1;
constexpr size_t kLayoutBlockLinear = 2;

constexpr size_t kInvertH = 0;
constexpr size_t kInvertV = 1;
constexpr size_t kInvertScanColumn = 2;

}  // namespace

void describeWindows(const std::vector<FeatureEntry> &table,
                     std::map<uint32_t, WindowCapabilities> &out) {
    for (const FeatureEntry &entry : table) {
        WindowCapabilities &caps = out[entry.window];

        switch (entry.option) {
        case kFeatureFormats:
            /* Two words, low half then high, one bit per format code. */
            caps.formats = static_cast<uint64_t>(entry.arg[0]) |
                           (static_cast<uint64_t>(entry.arg[1]) << 32);
            break;
        case kFeatureMaximumSize:
            caps.maxWidth = entry.arg[kSizeMaxWidth];
            caps.minWidth = entry.arg[kSizeMinWidth];
            caps.maxHeight = entry.arg[kSizeMaxHeight];
            caps.minHeight = entry.arg[kSizeMinHeight];
            break;
        case kFeatureMaximumScale:
            /* All four ratios are one where the window cannot resize.
             * Kept whole rather than collapsed to that bool: the driver
             * does not enforce these on the flip -- past the limit it
             * silently clamps the stepping and the window reads memory at
             * the wrong stride -- so whoever plans frames must know the
             * numbers, not just that resizing exists. */
            caps.maxUpH = entry.arg[kScaleUpH];
            caps.maxUpV = entry.arg[kScaleUpV];
            caps.maxDownH = entry.arg[kScaleDownH];
            caps.maxDownV = entry.arg[kScaleDownV];
            caps.scaling = caps.maxUpH != 1 || caps.maxUpV != 1 ||
                           caps.maxDownH != 1 || caps.maxDownV != 1;
            break;
        case kFeatureLayoutType:
            caps.pitchLayout = entry.arg[kLayoutPitched] != 0;
            caps.tiledLayout = entry.arg[kLayoutTiled] != 0;
            caps.blocklinearLayout = entry.arg[kLayoutBlockLinear] != 0;
            break;
        case kFeatureInvertType:
            caps.invertH = entry.arg[kInvertH] != 0;
            caps.invertV = entry.arg[kInvertV] != 0;
            caps.scanColumn = entry.arg[kInvertScanColumn] != 0;
            break;
        default:
            /* Colour conversion, filtering, field order, rotation formats.
             * Nothing here plans around them yet, and a row nobody reads
             * is not an error. */
            break;
        }
    }
}

}  // namespace hwc
}  // namespace android
