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

#include <cstdint>

#include <cutils/native_handle.h>

#include "utils/fd.h"

namespace android::drm_hwcomposer {

/* The allocator this board ships, reached through its own C interface.
 *
 * Everything anyone here needs to know about a buffer belongs to whoever
 * allocated it, and on this board that is a proprietary library. Two
 * different questions are asked of it -- what the memory looks like, and
 * whether it is in a state the display can read -- so it is one object with
 * two answers rather than two copies of the same dlopen.
 *
 * Not linked against, for two reasons. The library is declared in the vendor
 * blob repository by installation path rather than as a vendor module, so the
 * build has nothing to link a vendor module against; and that repository is
 * shared with the branch that already works, which is not a thing to change
 * for our convenience.
 */
class NvGralloc {
 public:
  /* The allocator, or nullptr if it could not be reached. Resolved on the
   * first call: a second attempt after a failure would fail the same way and
   * say so on every frame. */
  static NvGralloc *GetInstance();

  bool IsValid(buffer_handle_t handle) const;

  /* The framework's own format code for the buffer. */
  int GetHalFormat(buffer_handle_t handle) const;

  /* dma-buf for the pixels, borrowed. Owned by the buffer and valid as long
   * as the handle is. */
  int GetMemFd(buffer_handle_t handle) const;

  /* How the allocator describes the memory holding one image. */
  struct Surface {
    uint32_t width;
    uint32_t height;
    uint32_t layout;
    uint32_t pitch;  /* bytes */
    uint32_t offset;
    uint32_t kind;
    uint8_t block_height_log2;
  };

  /* The allocator's own names for how memory is arranged. */
  enum Layout {
    kLayoutPitch = 1,
    kLayoutTiled = 2,
    kLayoutBlocklinear = 3,
  };

  /* How long one of the allocator's surface descriptors is, in words: the
   * stride at which a buffer with more than one surface -- a plane of luma
   * and a plane of chroma -- keeps them one after another. Twenty words,
   * eighty bytes, read out of the device's own libnvrm and written down in
   * docs/nvrm-format-table.txt with the word map beside it. Not the room
   * the image compositor gives a descriptor it builds itself, which is
   * more than the record needs and was once mistaken for this. */
  static constexpr size_t kSurfaceWords = 20;

  /* The most surfaces one buffer is read for: luma and one plane of chroma
   * pairs. Fully planar arrangements carry a third, which nothing here
   * scans out. */
  static constexpr size_t kMostSurfaces = 2;

  /* Fills `out` from the buffer's first surface. False, with the reason
   * logged, if the buffer has none or if what was read does not agree with
   * itself -- see the implementation for why that check is not paranoia. */
  bool DescribeSurface(buffer_handle_t handle, Surface *out) const;

  /* Fills up to kMostSurfaces of `out` from the buffer's surfaces in the
   * order the allocator keeps them, and says how many it filled. A buffer
   * with more than that many is described up to the limit, not refused:
   * whoever reads the count decides what to do with a third plane. False,
   * with the reason logged, if the first surface fails the same checks
   * DescribeSurface makes, or any later one does. */
  bool DescribeSurfaces(buffer_handle_t handle, Surface *out,
                        size_t *count) const;

  /* The allocator's own descriptors, handed on untouched.
   *
   * For anything that has to give them back to another of this vendor's
   * libraries rather than read them: the image compositor takes exactly this
   * array, so passing it through means no part of the description is ever
   * rebuilt by us -- which is where the same attempt in the camera went
   * wrong. `out` belongs to the buffer and lives as long as the handle.
   *
   * False, quietly, if the buffer is not one of this allocator's. */
  bool GetRawSurfaces(buffer_handle_t handle, const void **out,
                      size_t *count) const;


 private:
  NvGralloc() = default;

  bool Resolve(void *library);

  /* One descriptor, read by word index and checked against itself. The
   * checks are the same for every surface of a buffer: a chroma plane is
   * half the size and the same arrangement, and passes them as the luma
   * does. */
  static bool ReadSurface(const uint32_t *word, Surface *out);

  int (*is_valid_)(buffer_handle_t) = nullptr;
  int (*get_memfd_)(buffer_handle_t) = nullptr;
  int (*get_format_)(buffer_handle_t) = nullptr;
  void (*get_surfaces_)(buffer_handle_t, const void **, size_t *) = nullptr;
};

}  // namespace android::drm_hwcomposer
