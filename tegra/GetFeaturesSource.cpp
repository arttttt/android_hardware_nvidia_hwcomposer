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

#include "tegra/GetFeaturesSource.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

/* From the kernel tree; see Android.mk for why the include path stops at
 * include/video rather than include/. */
#include <tegra_dc_ext.h>

#include "utils/Logging.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-dc-head"

namespace android {
namespace hwc {

int GetFeaturesSource::read(std::vector<FeatureEntry> &table) {
    /* There is no way to ask how long the table is first -- the call fills
     * whatever it is given and reports the length afterwards -- so the
     * buffer is made large enough for any table this driver builds. A table
     * for one head is a few rows per window. */
    constexpr size_t kMaxEntries = 256;

    std::vector<FeatureEntry> entries(kMaxEntries);

    struct tegra_dc_ext_feature request;
    memset(&request, 0, sizeof(request));
    request.entries = reinterpret_cast<__u32 *>(entries.data());

    /* The length is left at zero on the way in, and that is the whole of the
     * check below.
     *
     * This call fills the table only while the controller is running. When it
     * is not, the driver writes nothing at all -- not the entries, not the
     * length -- and returns success. So a caller that put the size of its own
     * buffer in that field would read its own number back, conclude the table
     * was that long, and parse a run of zeros: a head whose every window reads
     * no formats and is no pixels wide, described with complete confidence.
     *
     * Which is exactly what happened the first time SurfaceFlinger came back
     * to a composer that had outlived it. Zero going in makes the driver's
     * silence audible.
     */
    request.length = 0;

    /* The head logs a refusal with its own number; nothing to add here. */
    if (ioctl(fd_, TEGRA_DC_EXT_GET_FEATURES, &request) < 0)
        return -errno;

    if (request.length > kMaxEntries) {
        HWC_LOGE("feature table of %u rows, more than any this driver builds",
                 request.length);
        return -E2BIG;
    }

    entries.resize(request.length);
    table.swap(entries);
    return 0;
}

}  // namespace hwc
}  // namespace android
