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

/* Measures the one number the composer had to guess: how far the image
 * compositor's own verifier lets a single source be resized.
 *
 * The limit lives in the engine's firmware. Its userspace never checks, the
 * stock composer never checked, and the capability that would answer is not
 * exported -- so the composer's guard runs on an empirical bound (a ratio
 * over four always passed, one over sixteen always refused) and this stand
 * asks the verifier directly for the boundary in between.
 *
 * A probe is the shortest road to a verdict: reset the config, describe a
 * target, describe one source with the geometry under test, and call the
 * configure step whose return value is the verifier's answer. No execute, no
 * fences, no panel -- the judgement is made in userspace before anything is
 * submitted, which the field already proved: two hundred and forty-eight
 * refusals left the kernel log empty.
 *
 * Run it from a shell on the device; it opens its own session and lives
 * beside the composer's. The output ends in the number the guard wants.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>
#include <hardware/gralloc.h>

using android::GraphicBufferAllocator;
using android::status_t;

namespace {

constexpr int kNvSuccess = 0;
constexpr uint32_t kBuffer = 2048;
constexpr size_t kConfigBytes = 16384;
constexpr float kStep = 0.25F;

struct NvRect {
  int32_t left, top, right, bottom;
};
struct NvRectF32 {
  float left, top, right, bottom;
};

int (*rm_open)(void **);
void (*rm_close)(void *);
int (*create_session)(void *, int, void **);
void (*free_session)(void *);
int (*configure)(void *, void *);
int (*configure_source)(void *, void *, uint32_t, const void *, uint32_t,
                        const void *, const void *);
int (*configure_target)(void *, void *, const void *, uint32_t, const void *);
void (*configure_blending)(void *, void *, uint32_t, int, float);
int (*nvgr_is_valid)(buffer_handle_t);
void (*nvgr_get_surfaces)(buffer_handle_t, const void **, size_t *);

void *rm_device;
void *session;
std::vector<uint8_t> config;

const void *src_surfaces;
uint32_t src_count;
const void *dst_surfaces;
uint32_t dst_count;

/* The one escape hatch the spec allows: if even identity is refused on the
 * minimal triple, a blending description is added and everything retried
 * once -- it removes "uninitialised blend" as a variable without touching
 * the geometry under test. */
bool with_blending = false;

template <typename Fn>
bool Resolve(void *library, const char *name, Fn *slot) {
  *slot = reinterpret_cast<Fn>(dlsym(library, name));
  if (*slot == nullptr)
    printf("BROKEN: no %s\n", name);
  return *slot != nullptr;
}

enum Verdict { kPass, kRefused, kBroken };

/* One question to the verifier. Any refusal from the target or source
 * stages is a broken instrument, not a data point: the geometry is judged
 * by the configure step alone, and mixing the classes would make the
 * search measure the wrong thing. */
Verdict Probe(const NvRectF32 &src, const NvRect &dst, const char *tag) {
  memset(config.data(), 0, config.size());

  const NvRect full = {0, 0, static_cast<int32_t>(kBuffer),
                       static_cast<int32_t>(kBuffer)};
  int t = configure_target(session, config.data(), dst_surfaces, dst_count,
                           &full);
  int s = t == kNvSuccess
              ? configure_source(session, config.data(), 0, src_surfaces,
                                 src_count, &src, &dst)
              : -1;
  if (s == kNvSuccess && with_blending)
    configure_blending(session, config.data(), 0, 1 /* premultiplied */,
                       1.0F);
  int c = s == kNvSuccess ? configure(session, config.data()) : -1;

  const Verdict verdict = t != kNvSuccess || s != kNvSuccess
                              ? kBroken
                              : (c == kNvSuccess ? kPass : kRefused);
  printf("%-28s src %.1fx%.1f -> dst %dx%d : t=%d s=%d c=%d : %s\n", tag,
         src.right - src.left, src.bottom - src.top, dst.right - dst.left,
         dst.bottom - dst.top, t, s, c,
         verdict == kPass ? "PASS" : verdict == kRefused ? "REFUSED"
                                                         : "BROKEN");
  return verdict;
}

/* The four directions, each built so the ratio is exact where it can be:
 * upscales shrink the float source (any ratio is representable), downscales
 * shrink the integer destination (the achieved ratio is reported by the
 * caller reading the rectangles back). The idle axis stays at one-to-one. */
Verdict ProbeRatio(int direction, float ratio, uint32_t idle, float *achieved,
                   const char *tag) {
  NvRectF32 src = {0, 0, 0, 0};
  NvRect dst = {0, 0, 0, 0};
  const auto full = static_cast<float>(kBuffer);
  const auto idle_f = static_cast<float>(idle);

  switch (direction) {
    case 0: /* up, horizontal */
      src = {0, 0, full / ratio, idle_f};
      dst = {0, 0, static_cast<int32_t>(kBuffer),
             static_cast<int32_t>(idle)};
      *achieved = ratio;
      break;
    case 1: /* down, horizontal */
      src = {0, 0, full, idle_f};
      dst = {0, 0,
             std::max<int32_t>(1, static_cast<int32_t>(
                                      std::lround(full / ratio))),
             static_cast<int32_t>(idle)};
      *achieved = full / static_cast<float>(dst.right);
      break;
    case 2: /* up, vertical */
      src = {0, 0, idle_f, full / ratio};
      dst = {0, 0, static_cast<int32_t>(idle),
             static_cast<int32_t>(kBuffer)};
      *achieved = ratio;
      break;
    default: /* down, vertical */
      src = {0, 0, idle_f, full};
      dst = {0, 0, static_cast<int32_t>(idle),
             std::max<int32_t>(1, static_cast<int32_t>(
                                      std::lround(full / ratio)))};
      *achieved = full / static_cast<float>(dst.bottom);
      break;
  }
  return Probe(src, dst, tag);
}

const char *const kDirection[] = {"up-H", "down-H", "up-V", "down-V"};

}  // namespace

int main() {
  void *nvrm = dlopen("libnvrm.so", RTLD_NOW);
  void *vic = dlopen("libnvddk_vic.so", RTLD_NOW);
  void *nvgr = dlopen("libnvgr.so", RTLD_NOW);
  if (nvrm == nullptr || vic == nullptr || nvgr == nullptr) {
    printf("BROKEN: dlopen: %s\n", dlerror());
    return 1;
  }

  if (!Resolve(nvrm, "NvRmOpenNew", &rm_open) ||
      !Resolve(nvrm, "NvRmClose", &rm_close) ||
      !Resolve(vic, "NvDdkVicCreateSession", &create_session) ||
      !Resolve(vic, "NvDdkVicFreeSession", &free_session) ||
      !Resolve(vic, "NvDdkVicConfigure", &configure) ||
      !Resolve(vic, "NvDdkVicConfigureSourceSurface", &configure_source) ||
      !Resolve(vic, "NvDdkVicConfigureTargetSurface", &configure_target) ||
      !Resolve(vic, "NvDdkVicConfigureBlending", &configure_blending) ||
      !Resolve(nvgr, "nvgr_is_valid", &nvgr_is_valid) ||
      !Resolve(nvgr, "nvgr_get_surfaces", &nvgr_get_surfaces))
    return 1;

  if (rm_open(&rm_device) != kNvSuccess || rm_device == nullptr) {
    printf("BROKEN: NvRmOpenNew failed\n");
    return 1;
  }
  if (create_session(rm_device, 0, &session) != kNvSuccess ||
      session == nullptr) {
    printf("BROKEN: NvDdkVicCreateSession failed\n");
    return 1;
  }
  config.resize(kConfigBytes);

  /* Real buffers, described by the allocator's own hand: the source stage
   * reads their format and layout before any geometry is judged, and a
   * descriptor built by hand fails there -- measuring nothing. */
  constexpr uint64_t kUsage = GRALLOC_USAGE_HW_COMPOSER |
                              GRALLOC_USAGE_SW_READ_OFTEN;
  auto &allocator = GraphicBufferAllocator::get();
  buffer_handle_t src_handle = nullptr;
  buffer_handle_t dst_handle = nullptr;
  uint32_t stride = 0;
  if (allocator.allocate(kBuffer, kBuffer, android::PIXEL_FORMAT_RGBA_8888,
                         1, kUsage, &src_handle, &stride, 0,
                         "vicscaletest") != android::NO_ERROR ||
      allocator.allocate(kBuffer, kBuffer, android::PIXEL_FORMAT_RGBA_8888,
                         1, kUsage, &dst_handle, &stride, 0,
                         "vicscaletest") != android::NO_ERROR) {
    printf("BROKEN: cannot allocate probe buffers\n");
    return 1;
  }

  size_t count = 0;
  nvgr_get_surfaces(src_handle, &src_surfaces, &count);
  src_count = static_cast<uint32_t>(count);
  nvgr_get_surfaces(dst_handle, &dst_surfaces, &count);
  dst_count = static_cast<uint32_t>(count);
  if (src_surfaces == nullptr || dst_surfaces == nullptr || src_count == 0 ||
      dst_count == 0) {
    printf("BROKEN: buffers have no surfaces\n");
    return 1;
  }

  /* Phase 0: controls. Identity must pass -- with the blending fallback as
   * the one retry -- or the instrument itself is wrong and nothing below
   * means anything. The field geometries are replayed verbatim: the ratio
   * the popup always survived, and the navbar's two-axis fold that always
   * refused. A dual fold that passes here is an instrument that is not
   * measuring what the field measured. */
  printf("== controls ==\n");
  float achieved = 0;
  if (ProbeRatio(0, 1.0F, 256, &achieved, "identity") != kPass) {
    with_blending = true;
    printf("(retrying with a blending description)\n");
    if (ProbeRatio(0, 1.0F, 256, &achieved, "identity+blend") != kPass) {
      printf("BROKEN: identity refused; instrument wrong\n");
      return 1;
    }
  }
  for (int d = 1; d < 4; d++)
    ProbeRatio(d, 1.0F, 256, &achieved, "identity");
  ProbeRatio(0, 4.25F, 256, &achieved, "field 4.25 up-H");
  ProbeRatio(0, 16.0F, 256, &achieved, "exact 16 up-H");
  ProbeRatio(2, 16.0F, 256, &achieved, "exact 16 up-V");
  ProbeRatio(1, 32.0F, 256, &achieved, "single 32 down-H");
  ProbeRatio(3, 32.0F, 256, &achieved, "single 32 down-V");
  {
    const NvRectF32 navbar_src = {0, 0, 48, 1536};
    const NvRect navbar_dst = {0, 0, 1536, 48};
    Probe(navbar_src, navbar_dst, "field navbar dual 32");
  }

  /* Phases 1 and 2: per direction, grow the bracket by doubling from the
   * ratio the field proved until the first refusal, then bisect to a
   * quarter step. No refusal up to the representable ceiling is itself an
   * answer: that direction is unbounded as far as this stand can see. */
  printf("== search ==\n");
  float weakest = 0;
  bool bounded = false;
  for (int d = 0; d < 4; d++) {
    float lo = 4.25F;
    float hi = lo;
    bool refused = false;
    while (hi < 2048.0F) {
      hi *= 2.0F;
      if (ProbeRatio(d, hi, 256, &achieved, kDirection[d]) == kRefused) {
        refused = true;
        break;
      }
      lo = achieved;
    }
    if (!refused) {
      printf("%s: no refusal up to 2048:1\n", kDirection[d]);
      continue;
    }
    float bad = hi;
    while (bad - lo > kStep) {
      const float mid = (lo + bad) / 2.0F;
      if (ProbeRatio(d, mid, 256, &achieved, kDirection[d]) == kPass)
        lo = achieved > lo ? achieved : mid;
      else
        bad = mid;
    }
    printf("%s: boundary pass %.2f / refuse %.2f\n", kDirection[d], lo, bad);

    /* Phase 3 for this direction: both sides of the boundary again -- the
     * verifier is expected to be deterministic -- and the passing side on
     * a wider idle axis, because a boundary that moves with the idle
     * extent is a limit on absolute size, not on the ratio, and the guard
     * formula would have to change shape. */
    ProbeRatio(d, lo, 256, &achieved, "confirm pass");
    ProbeRatio(d, bad, 256, &achieved, "confirm refuse");
    ProbeRatio(d, lo, 512, &achieved, "confirm pass idle 512");

    if (!bounded || lo < weakest)
      weakest = lo;
    bounded = true;
  }

  /* Phase 4, free on the same session: the smallest things the verifier
   * will take, and whether a fractional crop edge is judged at all. */
  printf("== minimums ==\n");
  for (int32_t n : {1, 2, 4, 8, 16, 64}) {
    const NvRectF32 s = {0, 0, static_cast<float>(n),
                         static_cast<float>(n)};
    const NvRect dd = {0, 0, n, n};
    Probe(s, dd, "square 1:1");
  }
  for (int32_t n : {1, 2, 4}) {
    const NvRectF32 s = {0, 0, static_cast<float>(n), 256};
    const NvRect dd = {0, 0, n, 256};
    Probe(s, dd, "sliver 1:1");
  }
  {
    const NvRectF32 s = {0, 0, 481.5F, 256};
    const NvRect dd = {0, 0, 2048, 256};
    Probe(s, dd, "sub-pixel crop up-H");
  }

  if (bounded)
    printf("measured, not guessed: kEngineScaleReach candidate: %.2f\n",
           std::floor(weakest / kStep) * kStep);
  else
    printf("no axis boundary found up to 2048:1 -- the dual control is the "
           "only bound this stand saw\n");

  free_session(session);
  rm_close(rm_device);
  allocator.free(src_handle);
  allocator.free(dst_handle);
  return 0;
}
