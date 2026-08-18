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

#include "tegra/NvMapAllocator.h"

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include "tegra/ScratchBuffer.h"
#include "tegra/nvmap.h"
#include "utils/log.h"

#undef LOG_TAG
#define LOG_TAG "hwc-nvmap"

namespace android {
namespace hwc {

namespace {

/* Success, in this vendor's numbering. */
constexpr int kNvSuccess = 0;

/* Rows in steps of the turn pool's own grain, so a slot of either
 * origin can be reused by a near-miss ask the same way. */
constexpr uint32_t kGrain = 256;

uint32_t RoundUp(uint32_t v) {
  const uint32_t rounded = ((v + kGrain - 1) / kGrain) * kGrain;
  return rounded;
}

/* A tag in the flags' upper half: the kernel prints a warning for
 * every untagged client once, and the resource manager's own clients
 * all carry one. "CC", composer carveout -- nothing reads it, it only
 * has to be nonzero. */
constexpr uint32_t kComposerTag = 0x4343;

/* Words of headroom for the surface descriptor. The structure belongs
 * to libnvrm and its true size is the library's own -- forty-eight
 * bytes in this device's build, as its memset shows. Sixty-four words
 * is not a guess about the structure but a margin: the library fills
 * what it knows, and a build that outgrew this would want to be heard,
 * not truncated. */
constexpr size_t kSurfaceWords = 64;

/* The colour-format word within that descriptor, read as a run of
 * thirty-two bit words -- the same index the gralloc wrapper reads it
 * at (NvGralloc.cpp, SurfaceWord::kColorFormat). */
constexpr size_t kSurfaceWordFormat = 2;

template <typename Fn>
bool ResolveOne(void *library, const char *name, Fn *slot) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  *slot = reinterpret_cast<Fn>(dlsym(library, name));
  if (*slot == nullptr) {
    ALOGE("libnvrm has no %s", name);
    return false;
  }
  return true;
}

}  // namespace

ScratchBuffer ScratchBuffer::FromGralloc(buffer_handle_t handle) {
  ScratchBuffer buffer;
  buffer.origin_ = Origin::kGralloc;
  buffer.handle_ = handle;
  return buffer;
}

ScratchBuffer ScratchBuffer::FromCarveout(
    drm_hwcomposer::SharedFd fd, void *mem_handle,
    std::unique_ptr<uint32_t[]> surface, uint32_t width, uint32_t height,
    uint32_t pitch) {
  ScratchBuffer buffer;
  buffer.origin_ = Origin::kCarveout;
  buffer.fd_ = std::move(fd);
  buffer.mem_handle_ = mem_handle;
  buffer.surface_ = std::move(surface);
  buffer.width_ = width;
  buffer.height_ = height;
  buffer.pitch_ = pitch;
  return buffer;
}

ScratchBuffer::~ScratchBuffer() {
  if (origin_ == Origin::kCarveout)
    NvMapAllocator::ReleaseMemHandle(mem_handle_);
}

/* The move-assign releases what the slot held before it is overwritten:
 * a raw memory handle has no destructor of its own to run, so the
 * defaulted assignment would simply drop it. */
ScratchBuffer &ScratchBuffer::operator=(ScratchBuffer &&other) {
  if (this != &other) {
    if (origin_ == Origin::kCarveout)
      NvMapAllocator::ReleaseMemHandle(mem_handle_);

    origin_ = other.origin_;
    handle_ = other.handle_;
    fd_ = std::move(other.fd_);
    mem_handle_ = other.mem_handle_;
    surface_ = std::move(other.surface_);
    width_ = other.width_;
    height_ = other.height_;
    pitch_ = other.pitch_;

    other.origin_ = Origin::kGralloc;
    other.handle_ = nullptr;
    other.mem_handle_ = nullptr;
  }
  return *this;
}

SurfaceView ScratchBuffer::View() const {
  if (origin_ == Origin::kCarveout)
    return SurfaceView{nullptr, surface_.get(), width_, height_};
  return SurfaceView::Gralloc(handle_);
}

NvMapAllocator::NvMapAllocator() {
  nvmap_fd_ = open("/dev/nvmap", O_RDWR | O_CLOEXEC);
  if (nvmap_fd_ < 0) {
    ALOGE("/dev/nvmap: %s", strerror(errno));
    return;
  }

  /* libnvrm is already loaded by the compositor session; this dlopen is
   * a reference on the same object, and a failed one here fails every
   * later allocate the same way -- see the note on the class. */
  nvrm_library_ = dlopen("libnvrm.so", RTLD_NOW);
  if (nvrm_library_ == nullptr) {
    ALOGE("libnvrm.so: %s", dlerror());
    return;
  }

  if (!ResolveOne(nvrm_library_, "NvRmMemHandleFromFd", &mem_handle_from_fd_) ||
      !ResolveOne(nvrm_library_, "NvRmMemHandleFree", &mem_handle_free_) ||
      !ResolveOne(nvrm_library_, "NvRmSurfaceInitRmPitch",
                  &surface_init_rm_pitch_))
    return;

  usable_ = true;
  ALOGI("composer carveout zone opened");
}

NvMapAllocator::~NvMapAllocator() {
  /* The libraries stay open and the device stays open, as their cousins
   * in the compositor session do: this object lives as long as the
   * process. */
}

NvMapAllocator *NvMapAllocator::GetInstance() {
  static NvMapAllocator instance;
  return instance.usable_ ? &instance : nullptr;
}

void NvMapAllocator::ReleaseMemHandle(void *mem_handle) {
  if (mem_handle == nullptr)
    return;
  if (auto *allocator = GetInstance(); allocator != nullptr &&
                                      allocator->mem_handle_free_ != nullptr)
    allocator->mem_handle_free_(mem_handle);
}

std::unique_ptr<ScratchBuffer> NvMapAllocator::Allocate(uint32_t width,
                                                        uint32_t height) {
  if (!usable_ || width == 0 || height == 0) {
    refusals_++;
    return nullptr;
  }

  const uint32_t pitch = RoundUp(width * 4);
  const uint32_t bytes = pitch * height;

  /* A handle, then its memory, then a dma-buf out. The create already
   * hands back a descriptor for the handle -- the interface speaks in
   * them -- and the allocator allocates through it; the get then makes
   * a second, this one for the engine. The first is dropped the moment
   * the second exists. */
  struct nvmap_create_handle create = {};
  create.size = bytes;
  if (ioctl(nvmap_fd_, NVMAP_IOC_CREATE, &create) != 0) {
    ALOGE("cannot create a %ux%u handle: %s", width, height,
          strerror(errno));
    refusals_++;
    return nullptr;
  }
  const int handle_fd = static_cast<int>(create.handle);

  struct nvmap_alloc_handle alloc = {};
  alloc.handle = static_cast<uint32_t>(handle_fd);
  alloc.heap_mask = NVMAP_HEAP_CARVEOUT_COMPOSER;
  alloc.flags = NVMAP_HANDLE_CACHEABLE | (kComposerTag << 16);
  alloc.align = 4096;
  if (ioctl(nvmap_fd_, NVMAP_IOC_ALLOC, &alloc) != 0) {
    ALOGE("the zone would not give %ux%u: %s", width, height,
          strerror(errno));
    ioctl(nvmap_fd_, NVMAP_IOC_FREE, static_cast<unsigned long>(handle_fd));
    refusals_++;
    return nullptr;
  }

  struct nvmap_create_handle get = {};
  get.handle = static_cast<uint32_t>(handle_fd);
  if (ioctl(nvmap_fd_, NVMAP_IOC_GET_FD, &get) != 0) {
    ALOGE("cannot take a dma-buf for %ux%u: %s", width, height,
          strerror(errno));
    ioctl(nvmap_fd_, NVMAP_IOC_FREE, static_cast<unsigned long>(handle_fd));
    refusals_++;
    return nullptr;
  }
  const int buffer_fd = get.fd;
  ioctl(nvmap_fd_, NVMAP_IOC_FREE, static_cast<unsigned long>(handle_fd));

  void *mem_handle = nullptr;
  if (mem_handle_from_fd_(buffer_fd, &mem_handle) != kNvSuccess ||
      mem_handle == nullptr) {
    ALOGE("the resource manager would not take fd %d", buffer_fd);
    close(buffer_fd);
    refusals_++;
    return nullptr;
  }

  auto surface = std::make_unique<uint32_t[]>(kSurfaceWords);
  surface_init_rm_pitch_(surface.get(), width, height, kFormatA8B8G8R8,
                         pitch, mem_handle, 0);

  /* The library's answer is read back, once, for the dump: what it
   * actually put in the colour-format word, against what was asked. A
   * library that silently renumbered its formats would still draw, but
   * with the wrong bytes per pixel, and this is the earliest place
   * that difference is visible. */
  surface_format_ = surface[kSurfaceWordFormat];
  if (surface_format_ != kFormatA8B8G8R8 && !format_mismatch_) {
    format_mismatch_ = true;
    ALOGE("the library wrote surface format 0x%08x, asked for 0x%08x",
          surface_format_, kFormatA8B8G8R8);
  }

  return std::make_unique<ScratchBuffer>(ScratchBuffer::FromCarveout(
      drm_hwcomposer::MakeSharedFd(buffer_fd), mem_handle,
      std::move(surface), width, height, pitch));
}

}  // namespace hwc
}  // namespace android
