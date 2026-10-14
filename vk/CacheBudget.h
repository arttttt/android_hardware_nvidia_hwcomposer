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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace android::hwc::vk {

/* How much of other processes' memory the image cache may keep alive.
 *
 * An imported buffer holds its dma-buf: as long as the cache has the
 * image, the pages cannot go back to nvmap's pool however long ago the
 * app let the buffer go. Sixty-four images by count was up to three
 * quarters of a gigabyte of full-screen buffers; on the device the
 * composer held 416 MB after three pulls of the notification shade, the
 * pool ran dry, and the shade's next buffers came page by page from the
 * kernel, into direct reclaim -- its first frame some 30 ms later. A
 * busy scene drawn on the GPU needs its three targets and a handful of
 * layers, well under the bytes allowed here.
 *
 * Plain numbers, so the choice is checked on a workstation. */
struct CacheBudget {
  size_t most_entries = 64;
  size_t most_bytes = size_t{128} << 20;

  /* Whether adding `adding` bytes to `entries` images of `bytes` in all
   * goes over. */
  bool Over(size_t entries, size_t bytes, size_t adding) const {
    return entries + 1 > most_entries || bytes + adding > most_bytes;
  }
};

/* One cached image as the choice sees it: when it was last asked for,
 * on a counter that only grows. */
struct CacheUse {
  uint64_t last_use;
};

/* The image to let go: the least recently asked for among those asked
 * for before `protect_from` -- the ones the frame being put together has
 * asked for are in its hands and stay. Nothing if every one of them is. */
inline std::optional<size_t> PickVictim(const std::vector<CacheUse> &uses,
                                        uint64_t protect_from) {
  std::optional<size_t> victim;
  for (size_t i = 0; i < uses.size(); ++i) {
    if (uses[i].last_use >= protect_from)
      continue;
    if (!victim || uses[i].last_use < uses[*victim].last_use)
      victim = i;
  }
  return victim;
}

}  // namespace android::hwc::vk
