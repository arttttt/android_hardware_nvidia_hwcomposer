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

#ifndef TEGRA_DISPLAY_PIPELINE_H
#define TEGRA_DISPLAY_PIPELINE_H

#include <memory>
#include <string>
#include <vector>

#include "display/DisplayPipeline.h"
#include "tegra/DcHead.h"
#include "tegra/TegraVSyncSource.h"

namespace android {
namespace hwc {

/* One Tegra display head, assembled.
 *
 * Three devices answer for one display and none of them answers for all of
 * it: the head node posts frames, the control node carries events, and the
 * framebuffer device knows the timing and the backlight. This is where that
 * is hidden, so that above it a display is one object with modes, power and
 * a compositor.
 */
class TegraDisplayPipeline : public DisplayPipeline {
public:
    /* Opens head `index` and reads its panel timing. Returns null if either
     * fails, having logged which. */
    static std::unique_ptr<TegraDisplayPipeline> create(int index);

    ~TegraDisplayPipeline() override;

    std::string name() const override;
    const std::vector<DisplayMode> &modes() const override { return mModes; }
    size_t activeModeIndex() const override { return 0; }
    int setActiveMode(size_t index) override;
    int setPowerMode(PowerMode mode) override;

    Compositor &compositor() override { return *mCompositor; }
    VSyncSource &vsyncSource() override { return *mVSync; }

    /* Replaces the compositor. The one installed at construction shows
     * nothing, which is enough to boot; the one that drives the hardware
     * takes its place here without anything above this class noticing. */
    void setCompositor(std::unique_ptr<Compositor> compositor);

    DcHead &head() { return *mHead; }

private:
    TegraDisplayPipeline(int index, std::unique_ptr<DcHead> head,
                         std::unique_ptr<TegraVSyncSource> vsync,
                         const DisplayMode &mode);

    const int mIndex;

    std::unique_ptr<DcHead> mHead;
    std::unique_ptr<TegraVSyncSource> mVSync;
    std::unique_ptr<Compositor> mCompositor;

    /* Exactly one. The panel is fixed and has a single timing; the framework
     * still wants a list, so it gets one of length one. */
    std::vector<DisplayMode> mModes;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_DISPLAY_PIPELINE_H
