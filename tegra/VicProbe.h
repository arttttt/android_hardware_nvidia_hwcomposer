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

#ifndef TEGRA_VIC_PROBE_H
#define TEGRA_VIC_PROBE_H

#include <cutils/native_handle.h>

namespace android {
namespace hwc {

/* Does the engine put the right pixels where it is told?
 *
 * The one thing about this path that reading cannot settle. The engine is
 * handed the allocator's own descriptors rather than any we build, which is
 * where the same attempt in the camera went wrong -- but whether that is
 * enough is a question for the hardware, not for an argument.
 *
 * So: two real layers of the running interface, merged into one buffer with
 * blending and different scales, written out to be looked at. Real layers
 * because they are arranged in blocks, and that is the whole question; ask
 * the allocator for something the processor can write and it hands back rows
 * instead, which tests nothing. The last probe of this engine, in the camera,
 * validated a flat red fill -- and a flat fill looks the same whatever the
 * arrangement, so it proved only that the engine ran.
 *
 * Throwaway by design. It keeps its own session and its own buffer rather
 * than borrowing the display's, so that nothing in the frame path has to
 * grow a branch for it, and it runs once.
 *
 * Turned on with vendor.hwc.vic_probe. The result lands in
 * /data/local/tmp/vic_probe.rgba, and its shape is said in the log.
 */
class VicProbe {
 public:
  /* Offered every buffer the composer describes, which is every layer of
   * every frame. Ignored entirely unless the probe was asked for, and after
   * it has run.
   *
   * Cheap on the path it sits on: one already-answered property and one
   * boolean, before anything else is touched. */
  static void Offer(buffer_handle_t handle);
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_VIC_PROBE_H
