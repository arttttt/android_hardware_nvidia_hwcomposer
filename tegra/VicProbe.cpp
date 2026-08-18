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

#include "tegra/VicProbe.h"

#include <fcntl.h>
#include <sync/sync.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include <cutils/properties.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include "bufferinfo/NvGralloc.h"
#include "tegra/ScratchPool.h"
#include "tegra/VicSession.h"
#include "utils/log.h"

#undef  LOG_TAG
#define LOG_TAG "hwc-vicprobe"

namespace android {
namespace hwc {

namespace {

constexpr char kOutput[] = "/data/local/tmp/vic_probe.rgba";

/* Asked every time, not once.
 *
 * Everywhere else in this composer a property is read once and kept, because
 * a question asked sixty times a second is a cost of its own. Here the cost
 * is the point: the probe has to run when there is something on the screen,
 * and the composer starts before there is. Reading it every frame is what
 * lets the moment be chosen from outside -- bring the interface up, put
 * something on it, then ask. */
bool Wanted() {
  return property_get_bool("vendor.hwc.vic_probe", 0) != 0;
}

/* Two layers, and they have to be different ones.
 *
 * Merging a buffer with itself would draw the same picture twice and say
 * nothing about whether the second landed where it was told. */
std::vector<buffer_handle_t> &Collected() {
  static std::vector<buffer_handle_t> collected;
  return collected;
}

/* Copies `height` rows of `width` pixels, each `stride` pixels from the
 * last, into the probe's output file. Row by row rather than in one go
 * because a buffer is free to pad a row, and the padding is not part of
 * the picture. */
bool WriteRows(const void *pixels, uint32_t width, uint32_t height,
               uint32_t stride) {
  const int out = open(kOutput, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    ALOGE("cannot write %s: %s", kOutput, strerror(errno));
    return false;
  }

  bool ok = true;
  const auto *row = static_cast<const uint8_t *>(pixels);
  for (uint32_t y = 0; y < height && ok; y++) {
    ok = write(out, row, size_t{width} * 4) == static_cast<ssize_t>(width) * 4;
    row += size_t{stride} * 4;
  }
  if (!ok)
    ALOGE("short write to %s: %s", kOutput, strerror(errno));

  close(out);
  return ok;
}

/* Reads back what the engine wrote and puts it where it can be looked at.
 *
 * A carveout buffer is this composer's own, laid out in rows with no mapper
 * behind it, so it is read straight through its dma-buf; a gralloc one goes
 * through the mapper, which alone knows how to reach it. Both were asked for
 * as ones the processor reads often, so both are a straight row copy. */
bool WriteOut(const ScratchBuffer &target, uint32_t width, uint32_t height,
              uint32_t stride) {
  if (target.origin() == ScratchBuffer::Origin::kCarveout) {
    const size_t length = size_t{stride} * 4 * height;
    void *pixels = mmap(nullptr, length, PROT_READ, MAP_SHARED, target.fd(), 0);
    if (pixels == MAP_FAILED) {
      ALOGE("cannot map the merge back: %s", strerror(errno));
      return false;
    }
    const bool ok = WriteRows(pixels, width, height, stride);
    munmap(pixels, length);
    return ok;
  }

  void *pixels = nullptr;
  auto &mapper = GraphicBufferMapper::get();

  const status_t err = mapper.lock(target.handle(), GRALLOC_USAGE_SW_READ_OFTEN,
                                   Rect(static_cast<int32_t>(width),
                                        static_cast<int32_t>(height)),
                                   &pixels);
  if (err != NO_ERROR || pixels == nullptr) {
    ALOGE("cannot read the merge back: %d", err);
    return false;
  }

  const bool ok = WriteRows(pixels, width, height, stride);
  mapper.unlock(target.handle());
  return ok;
}

void Run() {
  auto &handles = Collected();

  auto session = VicSession::Create();
  if (!session) {
    ALOGE("no engine to probe with -- did the engine libraries open?");
    return;
  }

  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  drm_hwcomposer::NvGralloc::Surface first{};
  if (gralloc == nullptr || !gralloc->DescribeSurface(handles[0], &first)) {
    ALOGE("cannot describe the first layer");
    return;
  }

  auto pool = ScratchPool::Create(first.width, first.height, 1);
  if (!pool)
    return;

  /* Never shown, so nothing is waiting on it and the fence is empty. */
  ScratchBuffer *target = pool->Next(nullptr);
  if (target == nullptr)
    return;

  /* The lower one fills the buffer; the upper one lands in the middle at half
   * the size, blended. Both together are the question: is anything where it
   * was told to be, and does what is underneath show through. */
  const auto w = static_cast<int32_t>(pool->width());
  const auto h = static_cast<int32_t>(pool->height());

  std::vector<VicSession::Layer> layers;

  VicSession::Layer bottom{};
  bottom.handle = handles[0];
  bottom.source_right = static_cast<float>(first.width);
  bottom.source_bottom = static_cast<float>(first.height);
  bottom.display_right = w;
  bottom.display_bottom = h;
  bottom.premultiplied = true;
  bottom.alpha = 1.0F;
  bottom.acquire_fence = -1;
  layers.push_back(bottom);

  drm_hwcomposer::NvGralloc::Surface second{};
  if (!gralloc->DescribeSurface(handles[1], &second)) {
    ALOGE("cannot describe the second layer");
    return;
  }

  VicSession::Layer top{};
  top.handle = handles[1];
  top.source_right = static_cast<float>(second.width);
  top.source_bottom = static_cast<float>(second.height);
  top.display_left = w / 4;
  top.display_top = h / 4;
  top.display_right = w / 4 + w / 2;
  top.display_bottom = h / 4 + h / 2;
  top.premultiplied = true;
  top.alpha = 0.75F;
  top.acquire_fence = -1;
  layers.push_back(top);

  ALOGI("merging %ux%u over %ux%u into %ux%u", second.width, second.height,
        first.width, first.height, pool->width(), pool->height());

  auto fence = session->Compose(target->View(), layers);
  if (!fence) {
    ALOGE("the engine would not take it (%llu refused)",
          static_cast<unsigned long long>(session->refused()));
    return;
  }

  if (sync_wait(*fence, 1000) < 0) {
    ALOGE("the merge never finished");
    return;
  }

  if (WriteOut(*target, pool->width(), pool->height(), pool->stride()))
    ALOGI("merge written to %s, %ux%u RGBA", kOutput, pool->width(),
          pool->height());
}

}  // namespace

void VicProbe::Offer(buffer_handle_t handle) {
  static bool done = false;

  if (done || !Wanted() || handle == nullptr)
    return;

  auto &handles = Collected();
  for (auto *seen : handles)
    if (seen == handle)
      return;

  /* Big ones only.
   *
   * Taking whatever came first got two overlays that were almost entirely
   * transparent, and a merge of two transparent things is a black picture
   * that proves the engine ran and nothing else. What is wanted is a layer
   * with a picture in it -- the wallpaper, the launcher -- and on this
   * display those are the ones the size of the panel. */
  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  drm_hwcomposer::NvGralloc::Surface surface{};
  if (gralloc == nullptr || !gralloc->DescribeSurface(handle, &surface))
    return;
  if (surface.width < 1024 || surface.height < 1024)
    return;

  handles.push_back(handle);
  if (handles.size() < 2)
    return;

  /* Once, whatever happens. A probe that keeps trying on every frame would
   * bury the answer under its own repetition. */
  done = true;
  Run();
}

}  // namespace hwc
}  // namespace android
