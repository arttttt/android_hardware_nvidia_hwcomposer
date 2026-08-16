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

#include <cstring>


#include "bufferinfo/NvGralloc.h"
#include "utils/log.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-vic"

namespace android {
namespace hwc {

namespace {

/* Success, in this vendor's numbering. */
constexpr int kNvSuccess = 0;

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

  /* The libraries are left open. Everything above was made from them and is
   * only now gone, and this object is made once per display and released when
   * the composer is going away anyway. */
}

drm_hwcomposer::SharedFd VicSession::Compose(
    buffer_handle_t target, const std::vector<Layer> &layers,
    uint32_t width, uint32_t height, int target_ready) {
  if (layers.empty() || layers.size() > kMaxLayers) {
    refused_++;
    return {};
  }

  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  if (gralloc == nullptr) {
    refused_++;
    return {};
  }

  const void *target_surfaces = nullptr;
  size_t target_count = 0;
  if (!gralloc->GetRawSurfaces(target, &target_surfaces, &target_count) ||
      target_count != 1) {
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
   * The buffer's own size still comes from the allocator rather than from
   * the layers: the bound is clamped to the buffer, not trusted over it. */
  drm_hwcomposer::NvGralloc::Surface target_surface{};
  if (!gralloc->DescribeSurface(target, &target_surface)) {
    refused_++;
    return {};
  }

  if (width == 0 || width > target_surface.width)
    width = target_surface.width;
  if (height == 0 || height > target_surface.height)
    height = target_surface.height;

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

    const void *surfaces = nullptr;
    size_t count = 0;
    if (!gralloc->GetRawSurfaces(layer.handle, &surfaces, &count) ||
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
    buffer_handle_t target, const Layer &layer, uint32_t transform,
    uint32_t width, uint32_t height, int target_ready) {
  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  if (gralloc == nullptr || width == 0 || height == 0) {
    refused_++;
    return {};
  }

  const void *target_surfaces = nullptr;
  size_t target_count = 0;
  if (!gralloc->GetRawSurfaces(target, &target_surfaces, &target_count) ||
      target_count != 1) {
    refused_++;
    return {};
  }

  drm_hwcomposer::NvGralloc::Surface target_surface{};
  if (!gralloc->DescribeSurface(target, &target_surface) ||
      width > target_surface.width || height > target_surface.height) {
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

  const void *surfaces = nullptr;
  size_t count = 0;
  if (!gralloc->GetRawSurfaces(layer.handle, &surfaces, &count) ||
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

  if (configure_transform_(session_, config, transform) != kNvSuccess) {
    refused_++;
    return {};
  }

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
  if (fence_to_fd_("hwc-vic", &done, 1, &fd) != kNvSuccess || fd < 0) {
    ALOGE("the turned copy has no fence; dropping it");
    refused_++;
    return {};
  }

  composed_++;
  return drm_hwcomposer::MakeSharedFd(fd);
}

}  // namespace hwc
}  // namespace android
