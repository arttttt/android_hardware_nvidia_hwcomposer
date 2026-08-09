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

namespace android::drm_hwcomposer {

/* What the framework of the release this is built for does, where releases
 * differ and the composer has to differ with them.
 *
 * Plain constants rather than conditional compilation, deliberately: both
 * sides of every decision below stay in the source and are compiled on every
 * build, so neither can rot while the other is the one in use. What the
 * constant decides is which of two compiled paths runs.
 */

/* Whether the framework handles a display appearing while it is still inside
 * the call that registered the callbacks -- on that same thread, asking the
 * composer about the display before returning.
 *
 * It does on Android 9 and it does not on later releases, where the news is
 * taken away and dealt with afterwards. The difference decides how far the
 * composer's lock may extend around announcing a display: no further than
 * the registration itself where the framework calls back, and over the whole
 * of it where it does not, which is what upstream does.
 *
 * Set from the platform version at build time; see Android.mk.
 */
#ifndef HWC_FRAMEWORK_HOTPLUG_IS_REENTRANT
#define HWC_FRAMEWORK_HOTPLUG_IS_REENTRANT 0
#endif

constexpr bool kFrameworkHotplugIsReentrant = HWC_FRAMEWORK_HOTPLUG_IS_REENTRANT;

}  // namespace android::drm_hwcomposer
