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

#ifndef TEGRA_WINDOW_FEATURES_H
#define TEGRA_WINDOW_FEATURES_H

#include <cstdint>
#include <map>
#include <vector>

namespace android {
namespace hwc {

/* What one window can be asked to do.
 *
 * The windows of a head are not alike: which formats each reads, how far
 * it will scale, whether it understands memory arranged in blocks -- these
 * differ from one to the next, and a composer that assumed them equal would
 * hand the hardware a frame it cannot show and find out only when the flip
 * is refused. So they are asked for.
 */
struct WindowCapabilities {
    /* Which formats this window reads, as a bit per format code. */
    uint64_t formats = 0;

    uint32_t minWidth = 0;
    uint32_t maxWidth = 0;
    uint32_t minHeight = 0;
    uint32_t maxHeight = 0;

    bool pitchLayout = false;
    bool tiledLayout = false;
    bool blocklinearLayout = false;

    bool invertH = false;
    bool invertV = false;
    bool scanColumn = false;

    bool scaling = false;

    /* How far the window will resize, as the driver's own ratios: a
     * source may be up to maxDown times wider or taller than the window
     * shows it, and shown up to maxUp times wider or taller than it is.
     * All ones where the window cannot resize at all. */
    uint32_t maxUpH = 1;
    uint32_t maxUpV = 1;
    uint32_t maxDownH = 1;
    uint32_t maxDownV = 1;
};

/* One row of the controller's feature table: a window, a property of it,
 * and up to four values. The shape is the driver's own and is not in the
 * interface the kernel publishes -- it lives in a header private to the
 * driver (drivers/video/tegra/dc/dc_config.h) and is repeated here rather
 * than reached for, because a composer has no business including a
 * driver's private headers. */
struct FeatureEntry {
    uint32_t window;
    uint32_t option;
    uint32_t arg[4];
};

/* Where the feature table comes from.
 *
 * The table is the same whichever kernel describes the controller; the
 * call that hands it over is not. The 3.10 line answers one request with
 * the whole table; later lines dropped that request and describe the
 * windows through no other call, so a second source would be ours to add
 * on both sides. The head reads through whichever source it was given and
 * never learns the difference. */
class WindowFeatureSource {
public:
    virtual ~WindowFeatureSource() = default;

    /* Fills `table` with the controller's rows. Zero on success -- an empty
     * table is the controller declining to describe itself while it is off,
     * which is a moment and not a fault -- or a negative errno. */
    virtual int read(std::vector<FeatureEntry> &table) = 0;
};

/* Reads the table into what each window can do. A window is known from the
 * first row that names it; rows about properties nothing here plans around
 * are passed over, which is not an error. */
void describeWindows(const std::vector<FeatureEntry> &table,
                     std::map<uint32_t, WindowCapabilities> &out);

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_WINDOW_FEATURES_H
