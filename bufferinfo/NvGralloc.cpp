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

#include "bufferinfo/NvGralloc.h"

#include <dlfcn.h>
#include <unistd.h>

#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {

/* The surface descriptor, read as words.
 *
 * It is a flat run of thirty-two bit fields, and reading it by index rather
 * than through a declared structure is deliberate: the structure belongs to
 * the allocator and has gained a field since the last version of it published
 * with source, so a header copied from there would put every field after the
 * fourth word at the wrong offset. Indices instead, each established against
 * the library on this device:
 *
 *   - Colour format and pitch are the two the library itself reveals. Its
 *     stride accessor loads the third and the sixth word of the surface and
 *     divides one by the bytes-per-pixel packed into the other -- which also
 *     settles that the allocator counts a row in pixels and the descriptor in
 *     bytes.
 *   - Width and height are self-evident in a dump: they are the panel's.
 *   - The memory kind reads as the code for compressible thirty-two bit
 *     colour, which nothing else in this buffer could plausibly be, and it
 *     fixes everything around it: the layout says blocklinear in the word
 *     before the pitch, and the block height beside the kind is a sane four.
 *     (That dump predates the system-wide compression ban: with compression
 *     off, the allocator marks the same buffers with the plain kind, which
 *     is what the window's kind gate now expects.)
 */
enum SurfaceWord {
  kWidth = 0,
  kHeight = 1,
  kColorFormat = 2,
  kLayout = 4,
  kPitch = 5,
  kOffset = 7,
  kKind = 8,
  kBlockHeightLog2 = 9,
};

template <typename Fn>
bool ResolveOne(void *library, const char *name, Fn *slot) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  *slot = reinterpret_cast<Fn>(dlsym(library, name));
  if (*slot == nullptr) {
    ALOGE("libnvgr has no %s", name);
    return false;
  }
  return true;
}

}  // namespace

NvGralloc *NvGralloc::GetInstance() {
  static NvGralloc instance;
  static bool resolved = false;
  static bool usable = false;

  if (resolved)
    return usable ? &instance : nullptr;

  resolved = true;

  void *library = dlopen("libnvgr.so", RTLD_NOW);
  if (library == nullptr) {
    ALOGE("libnvgr.so: %s", dlerror());
    return nullptr;
  }

  usable = instance.Resolve(library);
  if (usable)
    ALOGI("libnvgr resolved");

  return usable ? &instance : nullptr;
}

bool NvGralloc::Resolve(void *library) {
  return ResolveOne(library, "nvgr_is_valid", &is_valid_) &&
         ResolveOne(library, "nvgr_get_memfd", &get_memfd_) &&
         ResolveOne(library, "nvgr_get_format", &get_format_) &&
         ResolveOne(library, "nvgr_get_surfaces", &get_surfaces_);
}

bool NvGralloc::IsValid(buffer_handle_t handle) const {
  return handle != nullptr && is_valid_(handle) != 0;
}

int NvGralloc::GetHalFormat(buffer_handle_t handle) const {
  return get_format_(handle);
}

int NvGralloc::GetMemFd(buffer_handle_t handle) const {
  return get_memfd_(handle);
}

bool NvGralloc::GetRawSurfaces(buffer_handle_t handle, const void **out,
                               size_t *count) const {
  *out = nullptr;
  *count = 0;

  if (!IsValid(handle))
    return false;

  get_surfaces_(handle, out, count);
  return *out != nullptr && *count != 0;
}

bool NvGralloc::DescribeSurface(buffer_handle_t handle, Surface *out) const {
  const void *surfaces = nullptr;
  size_t count = 0;
  get_surfaces_(handle, &surfaces, &count);

  if (surfaces == nullptr || count == 0) {
    ALOGE("buffer %p has no surfaces", handle);
    return false;
  }

  /* The first surface only. A second one would carry chroma for a planar
   * format, and those are not scanned out here yet. */
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *word = static_cast<const uint32_t *>(surfaces);

  /* Does the reading agree with itself?
   *
   * The word indices above were established against one build of the
   * allocator, and a different one would shift them. A shifted reading does
   * not look like an error, it looks like a plausible buffer of the wrong
   * shape, and scanning that out shows the user a broken picture with nothing
   * in the log. Under a shift each of these three lands on a field that means
   * something else entirely, and the odds of all three still holding are
   * slim.
   */
  if (word[kWidth] == 0 || word[kHeight] == 0) {
    ALOGE("surface reads as %ux%u", word[kWidth], word[kHeight]);
    return false;
  }

  if (word[kPitch] < word[kWidth]) {
    ALOGE("surface row of %u bytes cannot hold %u pixels", word[kPitch],
          word[kWidth]);
    return false;
  }

  if (word[kLayout] < kLayoutPitch || word[kLayout] > kLayoutBlocklinear) {
    ALOGE("surface layout reads as %u", word[kLayout]);
    return false;
  }

  out->width = word[kWidth];
  out->height = word[kHeight];
  out->layout = word[kLayout];
  out->pitch = word[kPitch];
  out->offset = word[kOffset];
  out->kind = word[kKind];
  out->block_height_log2 = static_cast<uint8_t>(word[kBlockHeightLog2]);

  return true;
}


}  // namespace android::drm_hwcomposer
