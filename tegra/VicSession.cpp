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

#include "tegra/VicSession.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>


#include "bufferinfo/NvGralloc.h"
#include "tegra/nvmap.h"
#include "utils/log.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-vic"

namespace android {
namespace hwc {

namespace {

/* Success, in this vendor's numbering. */
constexpr int kNvSuccess = 0;

/* A tag in the nvmap flags' upper half: the kernel prints a warning for
 * every untagged client once, and the resource manager's own clients all
 * carry one. "CC", composer carveout -- nothing reads it, it only has to
 * be nonzero. */
constexpr uint32_t kComposerTag = 0x4343;

/* How the engine is told to read the alpha the pixels carry. Its own
 * numbering, and the only three values there are. */
enum PerPixelAlpha {
  kAlphaIgnore = 0,
  kAlphaPremultiplied = 1,
  kAlphaNonPremultiplied = 2,
};

/* Rectangles, theirs. Four numbers each and nothing else, which is the whole
 * reason they can be declared here rather than taken from a header that would
 * bring the rest of the vendor's world with it. */
struct NvRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
};

struct NvRectF32 {
  float left;
  float top;
  float right;
  float bottom;
};

/* A point in the engine's own timeline: which counter, and what it has to
 * reach. Turned into a descriptor, and back, by the two calls in the graphics
 * half of the resource manager. */
struct NvRmFence {
  uint32_t syncpoint;
  uint32_t value;
};

/* What the engine is handed at execute: where to write, and what to read.
 *
 * The inner dimension is for formats whose planes live in separate surfaces.
 * Nothing merged here is one of those -- everything the interface draws is a
 * single surface of colour -- so only the first is ever filled, and a buffer
 * that says otherwise is refused rather than half-read.
 *
 * The stride of the input array is not confirmed by the library's body:
 * only the zeroth element of each row is used today, because multi-plane
 * sources are refused. YUV support will need this checked.
 */
constexpr size_t kSurfacesPerLayer = 8;

struct ExecParameters {
  const void *output;
  const void *inputs[VicSession::kMaxLayers][kSurfacesPerLayer];
};

/* Every wait a descriptor carries, appended to |waits| -- or the truth that
 * they could not be had.
 *
 * Two calls, by the library's own design: asked without an array it only
 * counts, and skips the capacity check while doing so, so the count is exact
 * and no fence is too wide to take. The old single call took at most eight
 * points and treated any failure as nothing to wait for -- which let the
 * engine read buffers it had just been told were not ready.
 *
 * An empty descriptor is a success carrying zero waits: already come due.
 */
static bool AppendWaits(int (*fence_from_fd)(int32_t, void *, uint32_t *),
                        int32_t fd, std::vector<NvRmFence> &waits) {
  uint32_t count = 0;
  int err = fence_from_fd(fd, nullptr, &count);
  if (err != kNvSuccess) {
    ALOGE("unreadable fence fd %d: %d", fd, err);
    return false;
  }

  /* The library on the device is newer than any source of it we can read,
   * so its answer is checked rather than believed: a descriptor names a
   * handful of points, and a count beyond any sane fence means the library
   * and we disagree about what was asked. Refused, not sized to a
   * misunderstanding. */
  constexpr uint32_t kSaneWaits = 64;
  if (count > kSaneWaits) {
    ALOGE("fence fd %d claims %u waits; refusing", fd, count);
    return false;
  }
  if (count == 0)
    return true;

  const size_t have = waits.size();
  waits.resize(have + count);
  err = fence_from_fd(fd, &waits[have], &count);
  if (err != kNvSuccess) {
    ALOGE("fence fd %d would not read back: %d", fd, err);
    return false;
  }
  waits.resize(have + count);
  return true;
}

template <typename Fn>
bool ResolveOne(void *library, const char *name, Fn *slot) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  *slot = reinterpret_cast<Fn>(dlsym(library, name));
  if (*slot == nullptr) {
    ALOGE("no %s", name);
    return false;
  }
  return true;
}

void *OpenLibrary(const char *name) {
  void *library = dlopen(name, RTLD_NOW);
  if (library == nullptr)
    ALOGE("%s: %s", name, dlerror());
  return library;
}

}  // namespace

std::unique_ptr<VicSession> VicSession::Create() {
  auto session = std::unique_ptr<VicSession>(new VicSession());

  if (!session->Open())
    return nullptr;

  ALOGI("image compositor opened");
  return session;
}

bool VicSession::Open() {
  nvrm_library_ = OpenLibrary("libnvrm.so");
  graphics_library_ = OpenLibrary("libnvrm_graphics.so");
  vic_library_ = OpenLibrary("libnvddk_vic.so");

  if (nvrm_library_ == nullptr || graphics_library_ == nullptr ||
      vic_library_ == nullptr)
    return false;

  if (!Resolve())
    return false;

  /* Their error numbers are reported as they come back. They are worth
   * printing rather than reducing to a yes or no: this whole path is reached
   * through recovered signatures, and the difference between "the engine says
   * no" and "the engine was handed something it did not expect" is the
   * difference between a wrong plan and a wrong call. */
  int err = rm_open_(&rm_device_);
  if (err != kNvSuccess || rm_device_ == nullptr) {
    ALOGE("cannot open the resource manager: %d (handle %p)", err, rm_device_);
    return false;
  }

  /* Zero for the channel: let it open its own to the engine. */
  err = create_session_(rm_device_, 0, &session_);
  if (err != kNvSuccess || session_ == nullptr) {
    ALOGE("cannot open a compositor session: %d (session %p)", err, session_);
    return false;
  }

  config_.resize(kConfigBytes);

  /* The zone path, resolved without obligation: a library build that lacks
   * this entry point simply means the path is not offered, which
   * OffersZoneBuffers() answers. What is left is the descriptor builder --
   * the import that used to make the handle the library's own is gone. */
  ResolveOne(nvrm_library_, "NvRmSurfaceInitRmPitch", &surface_init_rm_pitch_);
  return true;
}

bool VicSession::Resolve() {
  return ResolveOne(nvrm_library_, "NvRmOpenNew", &rm_open_) &&
         ResolveOne(nvrm_library_, "NvRmClose", &rm_close_) &&
         ResolveOne(graphics_library_, "NvRmFenceGetFromFile",
                    &fence_from_fd_) &&
         ResolveOne(graphics_library_, "NvRmFencePutToFile", &fence_to_fd_) &&
         ResolveOne(vic_library_, "NvDdkVicCreateSession", &create_session_) &&
         ResolveOne(vic_library_, "NvDdkVicFreeSession", &free_session_) &&
         ResolveOne(vic_library_, "NvDdkVicConfigure", &configure_) &&
         ResolveOne(vic_library_, "NvDdkVicExecute", &execute_) &&
         ResolveOne(vic_library_, "NvDdkVicConfigureSourceSurface",
                    &configure_source_) &&
         ResolveOne(vic_library_, "NvDdkVicConfigureTargetSurface",
                    &configure_target_) &&
         ResolveOne(vic_library_, "NvDdkVicConfigureBlending",
                    &configure_blending_) &&
         ResolveOne(vic_library_, "NvDdkVicConfigureClearRects",
                    &configure_clear_rects_) &&
         ResolveOne(vic_library_, "NvDdkVicConfigureTransform",
                    &configure_transform_);
}

VicSession::~VicSession() {
  /* Ordered, and it has to be: the session was made from the device and
   * holds it. */
  if (session_ != nullptr && free_session_ != nullptr)
    free_session_(session_);
  if (rm_device_ != nullptr && rm_close_ != nullptr)
    rm_close_(rm_device_);
  if (nvmap_fd_ >= 0)
    close(nvmap_fd_);

  /* The libraries are left open. Everything above was made from them and is
   * only now gone, and this object is made once per display and released when
   * the composer is going away anyway. */
}

drm_hwcomposer::SharedFd VicSession::Compose(
    const VendorBuffer &target, const std::vector<Layer> &layers,
    uint32_t width, uint32_t height, int target_ready) {
  /* The layers are still mostly the framework's buffers, and the allocator
   * is answerable for those; a turned copy among them carries its own
   * description and needs no one. */
  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  if (gralloc == nullptr) {
    refused_++;
    return {};
  }

  return ComposeInto(gralloc, target.surface.data(), target.width,
                     target.height, layers, width, height, target_ready,
                     target.surface.data());
}

drm_hwcomposer::SharedFd VicSession::ComposeInto(
    drm_hwcomposer::NvGralloc *gralloc, const void *target_surfaces,
    uint32_t buffer_width, uint32_t buffer_height,
    const std::vector<Layer> &layers, uint32_t width, uint32_t height,
    int target_ready, const uint32_t *probe_words) {
  if (layers.empty() || layers.size() > kMaxLayers) {
    refused_++;
    return {};
  }

  /* Zeroed every time rather than once.
   *
   * A slot left enabled from a frame that had more layers than this one would
   * draw a buffer nobody asked for, and the enable is a field this class does
   * not know the offset of -- so the whole thing goes back to zero and is
   * described again. It is a few kilobytes of memset against a merge of
   * several megapixels. */
  std::memset(config_.data(), 0, config_.size());
  void *config = config_.data();

  /* The region asked for, and never merely the part a layer happens to
   * cover.
   *
   * What falls outside this rectangle the engine does not touch, and these
   * buffers are taken in turn -- so an untouched corner still holds what was
   * drawn there two frames ago. Everything INSIDE the rectangle is written,
   * covered or not, which is what keeps those trails out of the picture.
   * The rectangle used to be the whole buffer, unconditionally; now the
   * caller may bound it, because a caller that scans out only a corner of
   * the target never shows what lies beyond it -- and megabytes written
   * past the corner were the dearest part of a small merge.
   *
   * The buffer's own size still comes from whoever allocated it rather
   * than from the layers: the bound is clamped to the buffer, not trusted
   * over it. */
  if (width == 0 || width > buffer_width)
    width = buffer_width;
  if (height == 0 || height > buffer_height)
    height = buffer_height;

  const NvRect target_rect = {
      .left = 0,
      .top = 0,
      .right = static_cast<int32_t>(width),
      .bottom = static_cast<int32_t>(height),
  };

  if (configure_target_(session_, config, target_surfaces, 1,
                        &target_rect) != kNvSuccess) {
    refused_++;
    return {};
  }

  /* The boundary with the library, probed where it is crossed: the target
   * is configured, the job not yet submitted. The word the reloc is built
   * from comes first, so a swapped handle is visible without reading the
   * rest; the leading words follow, so a descriptor that drifted says how.
   * Kept for the dump rather than logged -- the log on this platform does
   * not reach where a dump is read from. */
  if (probe_words != nullptr) {
    char probe[192];
    int at = snprintf(probe, sizeof(probe),
                      "  target at submit: handle 0x%08x, words",
                      probe_words[kSurfaceWordMemHandle]);
    for (size_t w = 0;
         w < 8 && at > 0 && static_cast<size_t>(at) < sizeof(probe); ++w)
      at += snprintf(probe + at, sizeof(probe) - static_cast<size_t>(at),
                     " %08x", probe_words[w]);
    last_target_probe_ = probe;
    last_target_probe_ += "\n";
  }

  ExecParameters exec = {};
  exec.output = target_surfaces;

  std::vector<NvRmFence> waits;
  waits.reserve(layers.size() + 1);

  /* The target first: until this comes due the display is still reading the
   * buffer we are about to draw into. Given to the engine exactly as the
   * layers' own are -- see the note on target_ready for why waiting here
   * instead would cost the buffers this protects. */
  if (target_ready >= 0 && !AppendWaits(fence_from_fd_, target_ready, waits)) {
    refused_++;
    return {};
  }

  for (size_t i = 0; i < layers.size(); i++) {
    const Layer &layer = layers[i];

    /* A layer the framework handed us is described by the allocator that
     * made it; a turned copy is described by the words the library built
     * when this composer cut the buffer from its zone. The engine reads
     * both the same way -- what differs is only who wrote the description,
     * and a buffer of ours must never be described by anything but the
     * library's own builder. */
    const void *surfaces = nullptr;
    size_t count = 1;
    if (layer.vendor != nullptr) {
      surfaces = layer.vendor->surface.data();
    } else if (!gralloc->GetRawSurfaces(layer.handle, &surfaces, &count) ||
               count != 1) {
      refused_++;
      return {};
    }

    const NvRectF32 source = {
        .left = layer.source_left,
        .top = layer.source_top,
        .right = layer.source_right,
        .bottom = layer.source_bottom,
    };
    const NvRect display = {
        .left = layer.display_left,
        .top = layer.display_top,
        .right = layer.display_right,
        .bottom = layer.display_bottom,
    };

    if (configure_source_(session_, config, static_cast<uint32_t>(i), surfaces,
                          static_cast<uint32_t>(count), &source,
                          &display) != kNvSuccess) {
      refused_++;
      return {};
    }

    configure_blending_(session_, config, static_cast<uint32_t>(i),
                        layer.premultiplied ? kAlphaPremultiplied
                                            : kAlphaNonPremultiplied,
                        layer.alpha);

    exec.inputs[i][0] = surfaces;

    /* The engine waits on these itself, so nothing here blocks. The
     * descriptor stays the caller's -- it is read, not taken. */
    if (layer.acquire_fence >= 0 &&
        !AppendWaits(fence_from_fd_, layer.acquire_fence, waits)) {
      refused_++;
      return {};
    }
  }

  /* Where a layer above covers one below completely there is no reason to
   * read the lower one at all, and the engine can be told so. Offered rather
   * than required: its own note says to call it once the slots are described,
   * which is here, and a failure only costs the saving. */
  configure_clear_rects_(session_, config);

  if (configure_(session_, config) != kNvSuccess) {
    refused_++;
    return {};
  }

  NvRmFence done = {};
  if (execute_(session_, &exec, waits.empty() ? nullptr : waits.data(),
               static_cast<uint32_t>(waits.size()), &done) != kNvSuccess) {
    refused_++;
    return {};
  }

  int32_t fd = -1;
  /* The library's dispatcher carries only the first three arguments of this
   * call across to the implementation; the fourth reaches it by way of the
   * stack, not by contract. It works, and is verified by the kernel's
   * fences -- but a changed library generation makes this the first place
   * to look. */
  if (fence_to_fd_("hwc-vic", &done, 1, &fd) != kNvSuccess || fd < 0) {
    /* The merge itself was accepted and is running; only the way of telling
     * anyone when it finishes was lost. Nothing can be done with a result
     * whose completion cannot be waited on, so it counts as a refusal. */
    ALOGE("the merge has no fence; dropping it");
    refused_++;
    return {};
  }

  composed_++;
  return drm_hwcomposer::MakeSharedFd(fd);
}

drm_hwcomposer::SharedFd VicSession::ComposeRotated(
    const VendorBuffer &target, const Layer &layer, uint32_t transform,
    uint32_t width, uint32_t height, int target_ready) {
  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  if (gralloc == nullptr || width == 0 || height == 0) {
    refused_++;
    return {};
  }

  /* The intermediate is one of ours, cut from the zone: its descriptor is
   * the library's own and its extent is known without asking anyone. What
   * is checked is only that the turned crop fits inside it -- the pool
   * rounds slots up, so a slot is usually larger than the ask. */
  const void *target_surfaces = target.surface.data();
  const size_t target_count = 1;
  if (width > target.width || height > target.height) {
    refused_++;
    return {};
  }

  std::memset(config_.data(), 0, config_.size());
  void *config = config_.data();

  /* The caller sized this to the turned crop: the destination is given in
   * the turned geometry -- the axis swap is the caller's, as the engine's
   * contract has it -- and the engine turns its reading of the source to
   * fill it. */
  const NvRect target_rect = {
      .left = 0,
      .top = 0,
      .right = static_cast<int32_t>(width),
      .bottom = static_cast<int32_t>(height),
  };

  if (configure_target_(session_, config, target_surfaces,
                        static_cast<uint32_t>(target_count),
                        &target_rect) != kNvSuccess) {
    refused_++;
    return {};
  }

  /* The layer being turned is the framework's own, described by its
   * allocator -- but a turned copy of a turned copy would be one of ours,
   * so the same rule as the group pass applies. */
  const void *surfaces = nullptr;
  size_t count = 1;
  if (layer.vendor != nullptr) {
    surfaces = layer.vendor->surface.data();
  } else if (!gralloc->GetRawSurfaces(layer.handle, &surfaces, &count) ||
             count != 1) {
    refused_++;
    return {};
  }

  const NvRectF32 source = {
      .left = layer.source_left,
      .top = layer.source_top,
      .right = layer.source_right,
      .bottom = layer.source_bottom,
  };

  if (configure_source_(session_, config, 0, surfaces,
                        static_cast<uint32_t>(count), &source,
                        &target_rect) != kNvSuccess) {
    refused_++;
    return {};
  }

  /* A verbatim copy: alpha rides into the intermediate untouched and is
   * honoured by the pass that composes it. The stock blit copied the same
   * way. */
  configure_blending_(session_, config, 0, kAlphaIgnore, 1.0F);

  configure_transform_(session_, config, transform);

  configure_clear_rects_(session_, config);

  if (configure_(session_, config) != kNvSuccess) {
    refused_++;
    return {};
  }

  ExecParameters exec = {};
  exec.output = target_surfaces;
  exec.inputs[0][0] = surfaces;

  std::vector<NvRmFence> waits;
  if (target_ready >= 0 && !AppendWaits(fence_from_fd_, target_ready, waits)) {
    refused_++;
    return {};
  }
  if (layer.acquire_fence >= 0 &&
      !AppendWaits(fence_from_fd_, layer.acquire_fence, waits)) {
    refused_++;
    return {};
  }

  NvRmFence done = {};
  if (execute_(session_, &exec, waits.empty() ? nullptr : waits.data(),
               static_cast<uint32_t>(waits.size()), &done) != kNvSuccess) {
    refused_++;
    return {};
  }

  int32_t fd = -1;
  /* The library's dispatcher carries only the first three arguments of this
   * call across to the implementation; the fourth reaches it by way of the
   * stack, not by contract. It works, and is verified by the kernel's
   * fences -- but a changed library generation makes this the first place
   * to look. */
  if (fence_to_fd_("hwc-vic", &done, 1, &fd) != kNvSuccess || fd < 0) {
    ALOGE("the turned copy has no fence; dropping it");
    refused_++;
    return {};
  }

  composed_++;
  return drm_hwcomposer::MakeSharedFd(fd);
}

VendorBuffer::~VendorBuffer() {
  if (pixels != nullptr)
    munmap(pixels, mapped);
}

bool VicSession::OffersZoneBuffers() const {
  return surface_init_rm_pitch_ != nullptr;
}

std::unique_ptr<VendorBuffer> VicSession::AllocateZoneTarget(
    uint32_t width, uint32_t height, uint32_t pitch_grain) {
  if (!OffersZoneBuffers() || width == 0 || height == 0 ||
      pitch_grain == 0) {
    ALOGE("zone buffer: not available in this session");
    zone_refusals_++;
    return nullptr;
  }

  /* The zone's device, opened on the first ask rather than at boot: a
   * session whose pool is gralloc's never pays for it. */
  if (nvmap_fd_ < 0) {
    nvmap_fd_ = open("/dev/nvmap", O_RDWR | O_CLOEXEC);
    if (nvmap_fd_ < 0) {
      ALOGE("zone buffer: /dev/nvmap: %s", strerror(errno));
      zone_refusals_++;
    return nullptr;
    }
  }

  /* Rows in the grain the reader wants: the engine's parser demands two
   * hundred and fifty-six for a target it writes, the cursor unit reads
   * rows with no padding at all. */
  const uint32_t grain = pitch_grain;
  const uint32_t pitch = ((width * 4 + grain - 1) / grain) * grain;
  const uint64_t raw =
      static_cast<uint64_t>(pitch) * ((height + 3) & ~3u);
  const auto size = static_cast<uint32_t>((raw + 131071) / 131072 * 131072);

  struct nvmap_create_handle create = {};
  create.size = size;
  if (ioctl(nvmap_fd_, NVMAP_IOC_CREATE, &create) != 0) {
    ALOGE("zone buffer: cannot create a %ux%u handle: %s", width, height,
          strerror(errno));
    zone_refusals_++;
    return nullptr;
  }
  const int handle_id = static_cast<int>(create.handle);

  /* The composer's own carveout heap, write-combined: the paint and the
   * dump's reads go through a mapping of this buffer, and a cacheable one
   * is what poisoned the zone's previous incarnation -- lines dirtied by
   * its zeroing were never flushed and later evicted over the engine's
   * writes. Not overridable on purpose: what this path exists to prove is
   * read off a single, known caching policy. */
  struct nvmap_alloc_handle alloc = {};
  alloc.handle = static_cast<uint32_t>(handle_id);
  alloc.heap_mask = NVMAP_HEAP_CARVEOUT_COMPOSER;
  alloc.flags = NVMAP_HANDLE_WRITE_COMBINE | (kComposerTag << 16);
  alloc.align = 4096;
  if (ioctl(nvmap_fd_, NVMAP_IOC_ALLOC, &alloc) != 0) {
    ALOGE("zone buffer: the zone would not give %ux%u: %s", width, height,
          strerror(errno));
    ioctl(nvmap_fd_, NVMAP_IOC_FREE, handle_id);
    zone_refusals_++;
    return nullptr;
  }

  struct nvmap_create_handle get = {};
  get.handle = static_cast<uint32_t>(handle_id);
  if (ioctl(nvmap_fd_, NVMAP_IOC_GET_FD, &get) != 0) {
    ALOGE("zone buffer: cannot take a dma-buf for %ux%u: %s", width, height,
          strerror(errno));
    ioctl(nvmap_fd_, NVMAP_IOC_FREE, handle_id);
    zone_refusals_++;
    return nullptr;
  }
  const int buffer_fd = get.fd;
  /* The dma-buf holds the reference from here on; the id is done. */
  ioctl(nvmap_fd_, NVMAP_IOC_FREE, handle_id);

  /* Mapped for the buffer's whole life, as for the library-born path: the
   * pool paints the slot through this mapping, the dump reads through it,
   * and a buffer that cannot be read back cannot answer the question the
   * paint asks -- so a mapping failure refuses the buffer outright. The
   * mapping inherits the handle's write-combined policy. */
  void *pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      buffer_fd, 0);
  if (pixels == MAP_FAILED) {
    ALOGE("zone buffer: the dma-buf would not map: %s", strerror(errno));
    close(buffer_fd);
    zone_refusals_++;
    return nullptr;
  }

  auto buffer = std::unique_ptr<VendorBuffer>(new VendorBuffer());
  buffer->fd = drm_hwcomposer::MakeSharedFd(buffer_fd);
  buffer->width = width;
  buffer->height = height;
  buffer->pitch = pitch;
  buffer->size = size;
  buffer->pixels = static_cast<uint32_t *>(pixels);
  buffer->mapped = size;

  /* The descriptor builder wants a memory handle in this word, and a memory
   * handle of this generation is the descriptor's own number: the kernel
   * resolves it through the process's file table, so our own descriptor
   * serves as well as one the library handed out, and creates no second
   * reference of its own. */
  buffer->surface.assign(kSurfaceWords, 0);
  surface_init_rm_pitch_(buffer->surface.data(), width, height, 0,
                         kFormatBlobRgba, kColorTagRgb, pitch,
                         reinterpret_cast<void *>(
                             static_cast<uintptr_t>(buffer_fd)),
                         0);
  buffer->surface[kSurfaceWordSize] = size;

  return buffer;
}

}  // namespace hwc
}  // namespace android
