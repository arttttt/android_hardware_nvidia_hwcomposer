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

/* Stand: the GPU executor as the composer will use it.
 *
 * Loads hwcvk.<platform>.so by path through dlopen and the interface in
 * vk/HwcVk.h -- exactly the composer's way in -- and composes one scene
 * into a panel-sized target:
 *
 *   0. a panel-sized buffer in four coloured quadrants, rounded corners;
 *   1. a solid blue at half alpha over the red quadrant;
 *   2. a small buffer, white on its left half and black on its right,
 *      turned a quarter clockwise -- its white half must land on top.
 *
 * Then the target is read the moment the returned fence comes due and
 * checked, and the call is timed fence to fence over twenty runs.
 *
 *   m hwcvk_stand hwcvk.tegra
 *   adb push .../vendor/lib/hw/hwcvk.tegra.so /data/local/tmp/
 *   adb shell /data/local/tmp/hwcvk_stand /data/local/tmp/hwcvk.tegra.so
 */

#define LOG_TAG "hwcvk_stand"

#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBuffer.h>

#include "vk/HwcVk.h"

using namespace android;
namespace hv = android::hwc::vk;

namespace {

constexpr uint32_t kW = 1536;
constexpr uint32_t kH = 2048;

constexpr uint32_t kRed = 0xff0000ff;
constexpr uint32_t kGreen = 0xff00ff00;
constexpr uint32_t kBlue = 0xffff0000;
constexpr uint32_t kWhite = 0xffffffff;
constexpr uint32_t kBlack = 0xff000000;

int failures = 0;

class StderrHost : public hv::Host {
 public:
  void Log(int prio, const char *msg) override {
    fprintf(stderr, "  [lib %d] %s\n", prio, msg);
  }
};

double NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

bool WaitFd(int fd) {
  pollfd p = {fd, POLLIN, 0};
  return poll(&p, 1, 2000) == 1;
}

sp<GraphicBuffer> Make(uint32_t w, uint32_t h, uint64_t usage, const char *name) {
  sp<GraphicBuffer> gb = new GraphicBuffer(w, h, PIXEL_FORMAT_RGBA_8888, 1, usage, name);
  return gb->initCheck() == NO_ERROR ? gb : nullptr;
}

template <typename F>
bool Fill(const sp<GraphicBuffer> &gb, F colour) {
  void *p = nullptr;
  if (gb->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &p) != NO_ERROR || !p)
    return false;
  auto *px = static_cast<uint32_t *>(p);
  for (uint32_t y = 0; y < gb->getHeight(); y++)
    for (uint32_t x = 0; x < gb->getWidth(); x++)
      px[y * gb->getStride() + x] = colour(x, y);
  gb->unlock();
  return true;
}

hv::Layer LayerOf(const sp<GraphicBuffer> &gb) {
  hv::Layer l;
  l.buffer = gb->handle;
  l.width = gb->getWidth();
  l.height = gb->getHeight();
  l.stride = gb->getStride();
  l.format = gb->getPixelFormat();
  l.usage = gb->getUsage();
  l.src_right = static_cast<float>(gb->getWidth());
  l.src_bottom = static_cast<float>(gb->getHeight());
  return l;
}

bool Close(uint32_t got, uint32_t want) {
  for (int s = 0; s < 32; s += 8) {
    const int a = (got >> s) & 0xff, b = (want >> s) & 0xff;
    if (abs(a - b) > 3)
      return false;
  }
  return true;
}

void Check(const uint32_t *px, uint32_t stride, uint32_t x, uint32_t y, uint32_t want,
           const char *what) {
  const uint32_t got = px[y * stride + x];
  const bool ok = Close(got, want);
  fprintf(stderr, "%s %-34s at %4u,%4u: 0x%08x (want 0x%08x)\n", ok ? "ok  " : "BAD ", what, x,
          y, got, want);
  failures += ok ? 0 : 1;
}

/* One call, fence to fence, the target taken as free. */
double Once(hv::Compositor *c, hv::Target t, const hv::Layer *layers, size_t n) {
  t.ready_fence = -1;
  const double t0 = NowMs();
  const int fd = c->Compose(t, layers, n);
  if (fd < 0)
    return -1;
  WaitFd(fd);
  close(fd);
  return NowMs() - t0;
}

}  // namespace

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/vendor/lib/hw/hwcvk.tegra.so";
  void *dso = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!dso) {
    fprintf(stderr, "FAIL: dlopen %s: %s\n", path, dlerror());
    return 1;
  }
  auto version = reinterpret_cast<uint32_t (*)()>(dlsym(dso, "hwc_vk_api_version"));
  auto create = reinterpret_cast<hv::Compositor *(*)(hv::Host *)>(dlsym(dso, "hwc_vk_create"));
  auto destroy = reinterpret_cast<void (*)(hv::Compositor *)>(dlsym(dso, "hwc_vk_destroy"));
  if (!version || !create || !destroy || version() != hv::apiVersion) {
    fprintf(stderr, "FAIL: entry points or api version (%u)\n", version ? version() : 0);
    return 1;
  }
  StderrHost host;
  hv::Compositor *c = create(&host);
  if (!c) {
    fprintf(stderr, "FAIL: the executor did not come up\n");
    return 1;
  }

  const uint64_t client = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
                          GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_WRITE_OFTEN;
  const uint64_t scanout = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER |
                           GRALLOC_USAGE_SW_READ_OFTEN;
  sp<GraphicBuffer> panel = Make(kW, kH, client, "stand panel");
  sp<GraphicBuffer> small = Make(64, 32, client, "stand small");
  sp<GraphicBuffer> target = Make(kW, kH, scanout, "stand target");
  if (!panel || !small || !target) {
    fprintf(stderr, "FAIL: gralloc\n");
    return 1;
  }
  Fill(panel, [](uint32_t x, uint32_t y) {
    const bool right = x >= kW / 2, bottom = y >= kH / 2;
    return bottom ? (right ? kWhite : kBlue) : (right ? kGreen : kRed);
  });
  Fill(small, [](uint32_t x, uint32_t) { return x < 32 ? kWhite : kBlack; });

  hv::Layer layers[3];
  layers[0] = LayerOf(panel);
  layers[0].dst_right = kW;
  layers[0].dst_bottom = kH;
  layers[0].corner_right = kW;
  layers[0].corner_bottom = kH;
  layers[0].corner_radius = 128;

  layers[1].dst_left = 100;
  layers[1].dst_top = 100;
  layers[1].dst_right = 300;
  layers[1].dst_bottom = 300;
  layers[1].color[2] = 1.f;
  layers[1].color[3] = 0.5f;

  layers[2] = LayerOf(small);
  layers[2].dst_left = 700;
  layers[2].dst_top = 900;
  layers[2].dst_right = 732;
  layers[2].dst_bottom = 964;
  layers[2].transform = 4;

  hv::Target t;
  t.buffer = target->handle;
  t.width = kW;
  t.height = kH;
  t.stride = target->getStride();
  t.format = target->getPixelFormat();
  t.usage = target->getUsage();

  /* "warm": one small composition before anything else, to see what of
   * the first call's cost is the pipeline's first use rather than the
   * buffers' import. */
  if (argc > 2 && !strcmp(argv[2], "warm")) {
    sp<GraphicBuffer> tiny = Make(16, 16, scanout, "stand tiny");
    hv::Layer solid;
    solid.dst_right = 16;
    solid.dst_bottom = 16;
    solid.color[3] = 1.f;
    hv::Target tt;
    tt.buffer = tiny->handle;
    tt.width = tt.height = 16;
    tt.stride = tiny->getStride();
    tt.format = tiny->getPixelFormat();
    tt.usage = tiny->getUsage();
    fprintf(stderr, "warm-up, 16x16 solid: %.2f ms\n", Once(c, tt, &solid, 1));
  }

  constexpr int kRuns = 20;
  double total = 0, worst = 0;
  int ready = -1;
  for (int run = 0; run < kRuns; run++) {
    t.ready_fence = ready;
    const double t0 = NowMs();
    const int fd = c->Compose(t, layers, 3);
    if (ready >= 0)
      close(ready);
    ready = -1;
    if (fd < 0) {
      fprintf(stderr, "FAIL: Compose refused on run %d\n", run);
      return 1;
    }
    if (!WaitFd(fd)) {
      fprintf(stderr, "FAIL: the fence never came due\n");
      return 1;
    }
    const double ms = NowMs() - t0;
    total += ms;
    if (ms > worst)
      worst = ms;
    if (run == 0)
      fprintf(stderr, "first call: %.2f ms\n", ms);
    if (run == kRuns - 1) {
      void *p = nullptr;
      if (target->lock(GRALLOC_USAGE_SW_READ_OFTEN, &p) != NO_ERROR || !p) {
        fprintf(stderr, "FAIL: lock target\n");
        return 1;
      }
      const auto *px = static_cast<const uint32_t *>(p);
      const uint32_t s = target->getStride();
      Check(px, s, 0, 0, 0, "corner cut");
      Check(px, s, kW - 1, kH - 1, 0, "corner cut");
      Check(px, s, 1152, 512, kGreen, "green quadrant");
      Check(px, s, 384, 1536, kBlue, "blue quadrant");
      Check(px, s, 50, 200, kRed, "red, beside the solid");
      Check(px, s, 200, 200, 0xff800080, "half blue over red");
      Check(px, s, 716, 910, kWhite, "turned: white half on top");
      Check(px, s, 716, 955, kBlack, "turned: black half below");
      target->unlock();
    }
    ready = fd; /* the target is free once this frame's drawing is done */
  }
  if (ready >= 0)
    close(ready);
  fprintf(stderr, "three layers into %ux%u: mean %.2f ms, worst %.2f ms over %d\n", kW, kH,
          total / kRuns, worst, kRuns);

  /* After a pause long enough for the GPU to power down, and with a
   * buffer the executor has never seen. */
  usleep(1500 * 1000);
  fprintf(stderr, "after 1.5 s idle: %.2f ms\n", Once(c, t, layers, 3));
  fprintf(stderr, "next, warm again: %.2f ms\n", Once(c, t, layers, 3));
  sp<GraphicBuffer> fresh = Make(kW, kH, client, "stand fresh");
  hv::Layer fl = LayerOf(fresh);
  fl.dst_right = kW;
  fl.dst_bottom = kH;
  fprintf(stderr, "a buffer never seen: %.2f ms\n", Once(c, t, &fl, 1));
  fprintf(stderr, "same buffer again: %.2f ms\n", Once(c, t, &fl, 1));

  /* A buffer never seen, but prepared when it "arrived": the import is
   * paid on the executor's thread before the call that draws it. */
  sp<GraphicBuffer> early = Make(kW, kH, client, "stand early");
  hv::Layer el = LayerOf(early);
  el.dst_right = kW;
  el.dst_bottom = kH;
  c->Prepare(el);
  usleep(20 * 1000);
  fprintf(stderr, "a buffer never drawn, prepared 20 ms ahead: %.2f ms\n", Once(c, t, &el, 1));

  /* The executor's own warm-up after a pause, with the composition coming
   * that long after it: how much lead the governor would need to give. */
  for (int lead_ms : {0, 5, 20, 40}) {
    usleep(1500 * 1000);
    const bool sent = c->Warm();
    usleep(lead_ms * 1000);
    fprintf(stderr, "after 1.5 s idle, warmed %2d ms ahead%s: %.2f ms\n", lead_ms,
            sent ? "" : " (not sent)", Once(c, t, layers, 3));
  }
  destroy(c);
  fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
