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

#include <errno.h>
#include <string.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>

#include "utils/Logging.h"

#include "hwc/HwcDevice.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-module"

using android::hwc::HwcDevice;

namespace {

int hwcOpen(const struct hw_module_t *module, const char *name,
            struct hw_device_t **device) {
    if (strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
        HWC_LOGE("asked for \"%s\", which this module does not provide", name);
        return -EINVAL;
    }

    HwcDevice *composer = new HwcDevice();

    int err = composer->init();
    if (err) {
        /* Refusing to load beats loading and failing later. The framework
         * treats a composer that cannot be opened as a fatal condition and
         * says so plainly in the log; one that opens and then answers
         * nonsense produces a harder puzzle. */
        delete composer;
        return err;
    }

    composer->common.module = const_cast<struct hw_module_t *>(module);
    *device = &composer->common;
    return 0;
}

struct hw_module_methods_t kMethods = {
    .open = hwcOpen,
};

}  // namespace

/* The symbol the loader looks for by name after dlopen. */
hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(2, 0),
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "Tegra hardware composer",
    .author = "Artem Bambalov",
    .methods = &kMethods,
    .dso = nullptr,
    .reserved = {0},
};
