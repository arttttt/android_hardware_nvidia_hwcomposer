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
#include <cstdlib>
#include <cstring>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include <hardware/gralloc.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>

#include "bufferinfo/NvGralloc.h"
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

/* How many of those words this device's library actually fills: the
 * memset in the blob zeroes eighty bytes, so the twenty words past that
 * hold the structure, and the comparison below reads exactly that far --
 * enough to cover every field the decoder has named, and never into a
 * neighbouring structure. */
constexpr size_t kSurfaceWordsUsed = 20;

/* The colour-format word within that descriptor, read as a run of
 * thirty-two bit words -- the same index the gralloc wrapper reads it
 * at (NvGralloc.cpp, SurfaceWord::kColorFormat). */
constexpr size_t kSurfaceWordFormat = 2;

/* The word that carries the surface's size in bytes. The allocator's
 * own descriptor was found, by the side-by-side comparison, to hold the
 * buffer's pitch times its height here while ours held a zero -- the one
 * difference between the two, and arithmetic told the story: 0xc00000
 * is exactly 1536 * 2048 * 4. */
constexpr size_t kSurfaceWordSize = 14;

/* The overrides the bring-up lends: a format and a gained field chosen
 * per boot, without a rebuild. Read each time a slot is allocated --
 * the cost is a property lookup against a carveout allocation, which
 * is not a frame. The comparison itself is not gated: it runs once,
 * at the first slot, and its answer is kept for the dump, which is the
 * only channel that reaches out of this process on this platform --
 * a log line written here goes nowhere. */
uint32_t SurfaceFormatOverride(uint32_t fallback) {
  char value[PROPERTY_VALUE_MAX];
  if (property_get("vendor.hwc.surffmt", value, "") <= 0)
    return fallback;
  char *end = nullptr;
  const unsigned long parsed = strtoul(value, &end, 16);
  if (end == value || *end != '\0' || parsed > 0xffffffffUL)
    return fallback;
  return static_cast<uint32_t>(parsed);
}

/* Word three, the field the engine's own parser reads as its format
 * code -- the engine's decoder does not walk the same words as libnvrm,
 * and the side-by-side laid the difference bare. Overridden per boot
 * the same way as the format, so a candidate value can be tried a
 * restart at a time. Absent or malformed, zero. The property keeps its
 * old name for the sake of the runs already taken; it is the engine's
 * format word it steers. */
uint32_t SurfaceGainedOverride() {
  char value[PROPERTY_VALUE_MAX];
  if (property_get("vendor.hwc.surfgained", value, "") <= 0)
    return 0;
  char *end = nullptr;
  const unsigned long parsed = strtoul(value, &end, 0);
  if (end == value || *end != '\0' || parsed > 0xffffffffUL)
    return 0;
  return static_cast<uint32_t>(parsed);
}

/* The caching policy of the zone's slots, overridable per boot: a
 * cacheable device buffer is a documented route to corruption in nvmap
 * (the flush happens only at free), so the policy is worth being able
 * to change without a rebuild. 0 is uncached, 1 is write-combine,
 * anything else the default cacheable. */
uint32_t ZoneCacheFlags() {
  char value[PROPERTY_VALUE_MAX];
  if (property_get("persist.vendor.hwc.zonecache", value, "") <= 0)
    return NVMAP_HANDLE_CACHEABLE;
  const int choice = atoi(value);
  switch (choice) {
    case 0:
      return NVMAP_HANDLE_UNCACHEABLE;
    case 1:
      return NVMAP_HANDLE_WRITE_COMBINE;
    default:
      return NVMAP_HANDLE_CACHEABLE;
  }
}

/* The library's own surface words for a same-sized buffer, to lay
 * against ours. The allocator describes its buffers through its own C
 * interface; a buffer of the same geometry is asked for and released
 * here, and never leaves this call. Returns the side-by-side as text
 * for the dump -- this platform's logs go nowhere, the dump is the only
 * channel out. */
std::string CompareToAllocatorsOwn(uint32_t width, uint32_t height,
                                   const uint32_t *ours) {
  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  if (gralloc == nullptr)
    return "no gralloc to ask for its own words";

  auto &allocator = android::GraphicBufferAllocator::get();
  buffer_handle_t theirs = nullptr;
  uint32_t stride = 0;
  const uint64_t usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_READ_OFTEN;
  if (allocator.allocate(width, height, android::PIXEL_FORMAT_RGBA_8888, 1,
                         usage, &theirs, &stride, 0, "surfcmp") !=
          android::NO_ERROR ||
      theirs == nullptr)
    return "cannot ask for a same-sized buffer";

  const void *raw = nullptr;
  size_t count = 0;
  if (!gralloc->GetRawSurfaces(theirs, &raw, &count) || raw == nullptr ||
      count == 0) {
    allocator.free(theirs);
    return "the buffer came without its surface words";
  }

  const auto *theirs_words = static_cast<const uint32_t *>(raw);
  std::string out = "ours vs the allocator's own, " +
                    std::to_string(width) + "x" + std::to_string(height) +
                    ":\n";
  for (size_t i = 0; i < kSurfaceWordsUsed; ++i) {
    const uint32_t ours_word = ours[i];
    const uint32_t theirs_word = theirs_words[i];
    char line[80];
    if (ours_word == theirs_word) {
      snprintf(line, sizeof(line), "  [%2zu] %08x  %08x\n", i, ours_word,
               theirs_word);
      out += line;
      continue;
    }
    /* Words the engine's own parser walks -- three as its format code,
     * fifteen, sixteen and nineteen as its plane and hardware fields.
     * A difference there is the engine's vocabulary, read directly, and
     * the value to try comes from the allocator's side of this very
     * line. */
    if (i == 3 || i == 15 || i == 16 || i == 19) {
      snprintf(line, sizeof(line),
               "  [%2zu] %08x  %08x   <- engine field, use theirs\n", i,
               ours_word, theirs_word);
      out += line;
      continue;
    }
    /* Words six and seven are pointers into each buffer's own allocation --
     * the memory handle and the offset within it. Different buffers, so
     * different values; a difference there is the rule, not the signal. */
    if (i == 6 || i == 7) {
      snprintf(line, sizeof(line),
               "  [%2zu] %08x  %08x   (expected: each buffer's own)\n", i,
               ours_word, theirs_word);
      out += line;
      continue;
    }
    /* The layout word: ours is forced to pitch by the library; the
     * allocator's own buffer may be blocklinear. A difference there is a
     * class of its own, not a defect in ours -- unless we mean to write
     * blocklinear too, which this build has never claimed to. */
    if (i == 4) {
      snprintf(line, sizeof(line),
               "  [%2zu] %08x  %08x   <- layout (ours pitch vs theirs)\n", i,
               ours_word, theirs_word);
      out += line;
      continue;
    }
    snprintf(line, sizeof(line), "  [%2zu] %08x  %08x   <- differs\n", i,
             ours_word, theirs_word);
    out += line;
  }

  /* Our margin beyond the structure the library fills: it should be
   * zero, and anything else is a library writing past what this build
   * zeroed. */
  for (size_t i = kSurfaceWordsUsed; i < kSurfaceWords; ++i) {
    if (ours[i] != 0) {
      char line[80];
      snprintf(line, sizeof(line), "  [%2zu] %08x  (ours beyond the structure)\n",
               i, ours[i]);
      out += line;
    }
  }

  allocator.free(theirs);
  return out;
}

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
    uint32_t pitch, uint32_t address) {
  ScratchBuffer buffer;
  buffer.origin_ = Origin::kCarveout;
  buffer.fd_ = std::move(fd);
  buffer.mem_handle_ = mem_handle;
  buffer.surface_ = std::move(surface);
  buffer.width_ = width;
  buffer.height_ = height;
  buffer.pitch_ = pitch;
  buffer.address_ = address;
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

  /* The resolver that turns a memory handle into the physical address the
   * engine will read and write. Present in the library's exports; absent,
   * the address line of the dump stays zero and says so. */
  ResolveOne(nvrm_library_, "NvRmMemPin", &mem_pin_);

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
  alloc.flags = ZoneCacheFlags() | (kComposerTag << 16);
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

  /* The zone does not come to the caller empty. The kernel hands a
   * carveout block out as it is -- no zeroing, unlike the page-backed
   * path -- and a block just born may hold whatever lived there before
   * it: a previous boot's data, a freed buffer's tail, a slot recycled
   * from another pool. Shown through a window that expects a picture,
   * that is noise the first time the slot is ever scanned. Paid once,
   * when a slot enters the pool for the first time: a page-touch per
   * slot of the pool's lifetime, not per frame.
   *
   * The hygiene is a courtesy, not an invariant: a slot that failed to
   * clear is still handed over, because a blank is not worth a refused
   * frame. But a dirty slot surfacing as picture noise is exactly the
   * symptom this clears, so the line below is a signal to be read, not
   * a warning to be skipped. */
  {
    const size_t length = static_cast<size_t>(bytes);
    void *pixels = mmap(nullptr, length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, buffer_fd, 0);
    if (pixels != MAP_FAILED) {
      memset(pixels, 0, length);
      munmap(pixels, length);
    } else {
      ALOGE("a zone slot came out dirty and could not be cleared: %s",
            strerror(errno));
    }
  }

  void *mem_handle = nullptr;
  if (mem_handle_from_fd_(buffer_fd, &mem_handle) != kNvSuccess ||
      mem_handle == nullptr) {
    ALOGE("the resource manager would not take fd %d", buffer_fd);
    close(buffer_fd);
    refusals_++;
    return nullptr;
  }

  /* The address the vendor resolver names for this handle, read once for
   * the dump. The engine writes through this number; if it is not the
   * slot the zone handed out, that is where the corruption lives. */
  if (mem_pin_ != nullptr)
    mem_address_ = mem_pin_(mem_handle);

  auto surface = std::make_unique<uint32_t[]>(kSurfaceWords);
  /* Nine arguments, not the seven the last published headers spell.
   * Between the height and the format this build takes a slot the
   * engine's parser reads as its format code, and the colour space
   * travels after the format -- so the pitch and the memory handle
   * both sit one place further along than the headers say. Read out of
   * the binary, after the fact: called with seven, the library writes
   * the row length into the format word -- the first boot's dump saw
   * exactly that, six thousand one hundred forty-four standing where a
   * colour should be -- and reads the memory handle out of stack that
   * was never passed, and the engine refuses every frame built on such
   * a surface. */
  const uint32_t format = SurfaceFormatOverride(kFormatA8B8G8R8);
  const uint32_t engine_format = SurfaceGainedOverride();
  surface_init_rm_pitch_(surface.get(), width, height, engine_format,
                         format, kColorSpaceLinearRgba, pitch, mem_handle,
                         0);

  /* The library fills what it knows and leaves the rest at the zero of
   * its own memset -- including the size word, which the allocator's own
   * descriptor carries: the side-by-side against a same-sized gralloc
   * buffer found exactly one difference between the two, a zero where
   * the allocator wrote the surface's size in bytes, pitch times height.
   * The engine reads it, so the zero is supplied here. */
  surface[kSurfaceWordSize] = bytes;

  /* The library's answer is read back, once, for the dump: what it
   * actually put in the colour-format word, against what was asked. A
   * library that silently renumbered its formats would still draw, but
   * with the wrong bytes per pixel, and this is the earliest place
   * that difference is visible. */
  surface_format_ = surface[kSurfaceWordFormat];
  if (surface_format_ != format && !format_mismatch_) {
    format_mismatch_ = true;
    ALOGE("the library wrote surface format 0x%08x, asked for 0x%08x",
          surface_format_, format);
  }

  static bool compared = false;
  if (!compared) {
    compared = true;
    compare_lines_ = CompareToAllocatorsOwn(width, height, surface.get());
  }

  return std::make_unique<ScratchBuffer>(ScratchBuffer::FromCarveout(
      drm_hwcomposer::MakeSharedFd(buffer_fd), mem_handle,
      std::move(surface), width, height, pitch, mem_address_));
}

}  // namespace hwc
}  // namespace android
