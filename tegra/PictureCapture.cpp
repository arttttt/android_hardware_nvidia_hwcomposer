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

#include "tegra/PictureCapture.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <sync/sync.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include "bufferinfo/NvGralloc.h"
#include "utils/Time.h"
#include "utils/log.h"

namespace android {
namespace hwc {

namespace {

constexpr size_t kSources = static_cast<size_t>(PictureCapture::Source::kCount);
constexpr const char *kNames[kSources] = {"merge", "merge-turned", "gpu",
                                          "frame"};

/* The whole of what the frame side touches: how many each source still owes,
 * and where the numbering stands. */
std::array<std::atomic<int>, kSources> left{};
std::atomic<int> next_seq{0};

/* The last ask, so that a dump repeating it starts nothing. The dump's side
 * only. */
std::mutex arm_lock;
std::string armed_with;

/* A source's turn from what it still owes, or -1 when it owes nothing. Two
 * engines on two threads can both be asked in one frame, so the count is
 * taken rather than read. */
int Take(PictureCapture::Source source) {
  auto &owed = left[static_cast<size_t>(source)];
  int now = owed.load(std::memory_order_relaxed);
  while (now > 0)
    if (owed.compare_exchange_weak(now, now - 1, std::memory_order_relaxed))
      return next_seq.fetch_add(1, std::memory_order_relaxed);
  return -1;
}

std::string PathFor(int seq, PictureCapture::Source source, const char *ext) {
  char path[96];
  snprintf(path, sizeof(path), "/data/misc/hwc-cap-%04d-%s.%s", seq,
           kNames[static_cast<size_t>(source)], ext);
  return path;
}

/* The fence's verdict goes into the words: a picture read after a wait that
 * timed out is a picture of an unfinished frame, and should say so. */
std::string Wait(int done) {
  if (done < 0)
    return "no fence";
  return sync_wait(done, 500) == 0 ? "fence signalled"
                                   : "FENCE NOT SIGNALLED IN 500 MS";
}

void WriteWords(int seq, PictureCapture::Source source, const std::string &head,
                const std::string &text) {
  const std::string path = PathFor(seq, source, "txt");
  FILE *out = fopen(path.c_str(), "w");
  if (out == nullptr) {
    ALOGE("no capture: %s: %s", path.c_str(), strerror(errno));
    return;
  }
  fprintf(out, "#%d %s at %" PRId64 " ns, %s\n%s", seq,
          kNames[static_cast<size_t>(source)], drm_hwcomposer::GetTimeMonotonicNs(),
          head.c_str(), text.c_str());
  fclose(out);
}

/* Every fourth pixel along both axes, through `at`, which answers the four
 * bytes of the pixel at a column and a row. */
template <typename At>
void WritePicture(int seq, PictureCapture::Source source, uint32_t width,
                  uint32_t height, At at) {
  const std::string path = PathFor(seq, source, "ppm");
  FILE *out = fopen(path.c_str(), "wb");
  if (out == nullptr) {
    ALOGE("no capture: %s: %s", path.c_str(), strerror(errno));
    return;
  }
  constexpr uint32_t kStep = 4;
  const uint32_t w = width / kStep;
  const uint32_t h = height / kStep;
  fprintf(out, "P6\n%u %u\n255\n", w, h);
  std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t word = at(x * kStep, y * kStep);
      row[x * 3 + 0] = static_cast<uint8_t>(word & 0xff);
      row[x * 3 + 1] = static_cast<uint8_t>((word >> 8) & 0xff);
      row[x * 3 + 2] = static_cast<uint8_t>((word >> 16) & 0xff);
    }
    fwrite(row.data(), 1, row.size(), out);
  }
  fclose(out);
}

/* Where a byte of memory arranged in blocks lives. A block is one group of
 * bytes wide -- 64 bytes by 8 rows -- and 2^`log2` groups tall; blocks run
 * across the surface first. Inside a group the bytes go in pairs of rows of
 * sixteen, which is the controller's own arrangement (TRM, "block linear",
 * 16Bx2). */
size_t BlocklinearOffset(uint32_t x_bytes, uint32_t y, uint32_t pitch,
                         uint32_t log2) {
  const uint32_t rows = 8u << log2;
  const uint32_t across = (pitch + 63) / 64;
  const size_t block = static_cast<size_t>(y / rows) * across + x_bytes / 64;
  const uint32_t group = (y % rows) / 8;
  const uint32_t in_group = ((x_bytes & 0x3f) >> 5) << 8 |
                            ((y & 7) >> 1) << 6 | ((x_bytes & 0x1f) >> 4) << 5 |
                            (y & 1) << 4 | (x_bytes & 0xf);
  return block * (512u << log2) + group * 512u + in_group;
}

}  // namespace

std::string PictureCapture::Arm() {
  const int frames = property_get_int32("vendor.hwc.test.capture", 0);
  char from[PROPERTY_VALUE_MAX];
  property_get("vendor.hwc.test.capture.from", from, "merge,gpu,frame");

  std::lock_guard<std::mutex> guard(arm_lock);
  const std::string ask = std::to_string(frames) + "/" + from;
  if (ask != armed_with) {
    armed_with = ask;
    const std::string wanted = std::string(",") + from + ",";
    for (size_t i = 0; i < kSources; ++i) {
      const bool named =
          wanted.find(std::string(",") + kNames[i] + ",") != std::string::npos;
      left[i].store(frames > 0 && named ? frames : 0,
                    std::memory_order_relaxed);
    }
    next_seq.store(0, std::memory_order_relaxed);
  }

  std::string line = "Picture capture (" + ask + "): still owed";
  for (size_t i = 0; i < kSources; ++i)
    line += std::string(" ") + kNames[i] + " " +
            std::to_string(left[i].load(std::memory_order_relaxed));
  line += ", next #" + std::to_string(next_seq.load(std::memory_order_relaxed));
  return line;
}

bool PictureCapture::Wanted(Source source) {
  return left[static_cast<size_t>(source)].load(std::memory_order_relaxed) > 0;
}

void PictureCapture::Write(Source source, const void *pixels, uint32_t width,
                           uint32_t height, uint32_t pitch, int done,
                           const std::string &text) {
  const int seq = Take(source);
  if (seq < 0)
    return;
  const std::string waited = Wait(done);
  if (pixels == nullptr || pitch == 0) {
    WriteWords(seq, source, waited + ", no pixels", text);
    return;
  }
  const auto *bytes = static_cast<const uint8_t *>(pixels);
  WritePicture(seq, source, width, height, [&](uint32_t x, uint32_t y) {
    uint32_t word;
    memcpy(&word, bytes + static_cast<size_t>(y) * pitch + x * 4, 4);
    return word;
  });
  char head[96];
  snprintf(head, sizeof(head), "%s, %ux%u rows of %u", waited.c_str(), width,
           height, pitch);
  WriteWords(seq, source, head, text);
}

void PictureCapture::WriteBuffer(Source source, buffer_handle_t buffer,
                                 uint32_t width, uint32_t height, int done,
                                 const std::string &text) {
  const int seq = Take(source);
  if (seq < 0)
    return;
  const std::string waited = Wait(done);

  auto *gralloc = drm_hwcomposer::NvGralloc::GetInstance();
  drm_hwcomposer::NvGralloc::Surface s{};
  if (buffer == nullptr || gralloc == nullptr ||
      !gralloc->DescribeSurface(buffer, &s)) {
    WriteWords(seq, source, waited + ", the buffer would not describe itself",
               text);
    return;
  }

  char head[160];
  snprintf(head, sizeof(head),
           "%s, %ux%u layout %u pitch %u offset %u kind %u block 2^%u",
           waited.c_str(), s.width, s.height, s.layout, s.pitch, s.offset,
           s.kind, s.block_height_log2);

  if (s.layout == drm_hwcomposer::NvGralloc::kLayoutTiled) {
    WriteWords(seq, source, std::string(head) + ", tiled: not read", text);
    return;
  }

  /* A lock of block-arranged memory hands back the blocks as they lie, with
   * the caches made right; the arrangement is undone below. The mapper
   * locks only what was imported through it; a buffer this process
   * allocated itself through the allocator's own device -- the GPU
   * executor's -- is locked through the allocator's module instead. */
  auto &mapper = GraphicBufferMapper::get();
  const gralloc_module_t *module = nullptr;
  void *mapped = nullptr;
  if (mapper.lock(buffer, GRALLOC_USAGE_SW_READ_OFTEN,
                  Rect(static_cast<int32_t>(width),
                       static_cast<int32_t>(height)),
                  &mapped) != NO_ERROR ||
      mapped == nullptr) {
    mapped = nullptr;
    const hw_module_t *hw = nullptr;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &hw) == 0) {
      module = reinterpret_cast<const gralloc_module_t *>(hw);
      if (module->lock(module, buffer, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0,
                       static_cast<int>(width), static_cast<int>(height),
                       &mapped) != 0)
        mapped = nullptr;
    }
  }
  if (mapped == nullptr) {
    WriteWords(seq, source, std::string(head) + ", would not lock", text);
    return;
  }

  const auto *base = static_cast<const uint8_t *>(mapped);
  const uint32_t pitch = s.pitch;
  const uint32_t log2 = s.block_height_log2;
  if (s.layout == drm_hwcomposer::NvGralloc::kLayoutBlocklinear) {
    WritePicture(seq, source, width, height, [&](uint32_t x, uint32_t y) {
      uint32_t word;
      memcpy(&word, base + BlocklinearOffset(x * 4, y, pitch, log2), 4);
      return word;
    });
  } else {
    WritePicture(seq, source, width, height, [&](uint32_t x, uint32_t y) {
      uint32_t word;
      memcpy(&word, base + static_cast<size_t>(y) * pitch + x * 4, 4);
      return word;
    });
  }
  if (module != nullptr)
    module->unlock(module, buffer);
  else
    mapper.unlock(buffer);
  WriteWords(seq, source,
             std::string(head) + (module != nullptr ? ", module lock" : ""),
             text);
}

void PictureCapture::WriteText(Source source, const std::string &text) {
  const int seq = Take(source);
  if (seq < 0)
    return;
  WriteWords(seq, source, "words only", text);
}

}  // namespace hwc
}  // namespace android
