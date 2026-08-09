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

  /* Fills `out` from the buffer's first surface. False, with the reason
   * logged, if the buffer has none or if what was read does not agree with
   * itself -- see the implementation for why that check is not paranoia. */
  bool DescribeSurface(buffer_handle_t handle, Surface *out) const;

  /* Puts the buffer into a state the display can read.
   *
   * The GPU on this hardware writes colour compressed, and the display
   * controller cannot read that -- the engine that would decompress it on the
   * way to the panel arrived a chip generation later. The allocator undoes it
   * only when something locks the buffer to read it, and scanout locks
   * nothing, so nothing would ever undo it.
   *
   * `acquire_fence` is borrowed. The fence handed back is owned by the caller
   * and is the one to wait on before reading, whether or not any work turned
   * out to be needed: it carries the acquire fence's meaning forward, so the
   * caller should stop using its own once this returns.
   */
  void PrepareForScanout(buffer_handle_t handle, int acquire_fence,
                         SharedFd *out_fence) const;

 private:
  NvGralloc() = default;

  bool Resolve(void *library);

  int (*is_valid_)(buffer_handle_t) = nullptr;
  int (*get_memfd_)(buffer_handle_t) = nullptr;
  int (*get_format_)(buffer_handle_t) = nullptr;
  void (*get_surfaces_)(buffer_handle_t, const void **, size_t *) = nullptr;
  int (*get_compressed_)(buffer_handle_t) = nullptr;
  int (*decompress_)(buffer_handle_t, int, int *) = nullptr;
};

}  // namespace android::drm_hwcomposer
