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

#ifndef TEGRA_NVMAP_H
#define TEGRA_NVMAP_H

/* The four corners of the nvmap userspace interface this code uses,
 * declared here rather than included: the ioctl ABI is the kernel's own
 * linux/nvmap.h, which headers_install does not export -- and the kernel
 * tree is reached from the build only for include/video, deliberately
 * (see the note in Android.mk). Everything below is a verbatim ABI copy
 * of the structures and numbers the live kernel exchanges, not a
 * re-declaration: they must match it exactly.
 */

#include <stdint.h>
#include <sys/ioctl.h>

/* The composer's own carveout heap -- the kernel commits this code is
 * paired with add the bit and the zone it names. */
#define NVMAP_HEAP_CARVEOUT_COMPOSER (1ul << 26)

/* Allocation flags: the memory is read and written by the engine and
 * never mapped into this process. The two bits pick the caching policy:
 * write-combine (0x2), uncached (0x1), or cacheable (0x3, both set). */
#define NVMAP_HANDLE_UNCACHEABLE  (0x1ul << 0)
#define NVMAP_HANDLE_WRITE_COMBINE (0x2ul << 0)
#define NVMAP_HANDLE_CACHEABLE    (0x3ul << 0)

struct nvmap_create_handle {
  union {
    uint32_t id;   /* FromId */
    uint32_t size; /* CreateHandle */
    int32_t fd;    /* DmaBufFd or FromFd */
  };
  uint32_t handle;
};

struct nvmap_alloc_handle {
  uint32_t handle;
  uint32_t heap_mask;
  uint32_t flags;
  uint32_t align;
};

#define NVMAP_IOC_MAGIC 'N'
#define NVMAP_IOC_CREATE _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_GET_FD _IOWR(NVMAP_IOC_MAGIC, 15, struct nvmap_create_handle)

#endif /* TEGRA_NVMAP_H */
