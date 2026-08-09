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

#ifndef BUFFERINFO_BUFFER_INFO_H
#define BUFFERINFO_BUFFER_INFO_H

#include <cstdint>

#include <cutils/native_handle.h>

namespace android {
namespace hwc {

/* A graphics buffer, described in the terms the display controller needs.
 *
 * Everything here comes from the allocator that produced the buffer. The
 * handle it hands out is a private structure whose layout belongs to that
 * allocator and changes with it, so nothing here reads it: the questions are
 * asked through the allocator's own functions, which have outlived several
 * versions of that structure.
 */
struct BufferInfo {
    /* dma-buf for the pixels, borrowed. Owned by the buffer, valid as long
     * as the handle is. */
    int fd = -1;

    /* Where the image starts within that memory. */
    uint32_t offset = 0;

    /* Bytes per row, padding included. Not pixels: the controller counts in
     * bytes, and the allocator does not. */
    uint32_t strideBytes = 0;

    /* The controller's own format code. */
    uint32_t format = 0;

    /* Flags for the flip: whether the memory is laid out in blocks rather
     * than rows, and how tall a block is. */
    uint32_t flags = 0;
    uint8_t blockHeightLog2 = 0;
};

/* Describes `handle` for scanout. Returns 0, or a negative errno with the
 * reason logged. */
int describeBuffer(buffer_handle_t handle, BufferInfo *outInfo);

}  // namespace hwc
}  // namespace android

#endif  // BUFFERINFO_BUFFER_INFO_H
