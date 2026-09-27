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

#ifndef TEGRA_GET_FEATURES_SOURCE_H
#define TEGRA_GET_FEATURES_SOURCE_H

#include "tegra/WindowFeatures.h"

namespace android {
namespace hwc {

/* The feature table as the 3.10 kernel hands it over: one request, the
 * whole table. Borrows the head's descriptor; the head outlives it. */
class GetFeaturesSource : public WindowFeatureSource {
public:
    explicit GetFeaturesSource(int fd) : fd_(fd) {}

    int read(std::vector<FeatureEntry> &table) override;

private:
    int fd_;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_GET_FEATURES_SOURCE_H
