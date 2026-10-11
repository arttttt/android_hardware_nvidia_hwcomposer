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

#include "tegra/PanelMirror.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <sync/sync.h>

#include "tegra/PictureCapture.h"
#include "utils/log.h"

namespace android {
namespace hwc {

namespace {

/* A quarter along each axis, as every other picture here is. */
constexpr uint32_t kScale = 4;

}  // namespace

PanelMirror::PanelMirror(VicSession *vic, uint32_t panel_width,
                         uint32_t panel_height)
    : vic_(vic), width_(panel_width / kScale), height_(panel_height / kScale) {
}

PanelMirror::~PanelMirror() = default;

void PanelMirror::Lift() {
  if (!pending_)
    return;
  std::unique_ptr<Pending> p = std::move(pending_);
  Kept &k = kept_.emplace_back();
  k.seq = p->seq;
  k.words = std::move(p->words);
  if (!p->drawn) {
    k.words = "the engine would not draw this frame\n" + k.words;
    return;
  }
  /* A frame later the engine is long done; a wait that still runs out says
   * so in the words rather than holding the frame. */
  if (p->done && sync_wait(*p->done, 50) != 0)
    k.words = "ENGINE NOT DONE IN 50 MS\n" + k.words;
  k.pixels.resize(static_cast<size_t>(width_) * height_ * 4);
  const auto *src = reinterpret_cast<const uint8_t *>(target_->pixels);
  for (uint32_t y = 0; y < height_; ++y)
    memcpy(&k.pixels[static_cast<size_t>(y) * width_ * 4],
           src + static_cast<size_t>(y) * target_->pitch, width_ * 4);
}

void PanelMirror::WriteOut() {
  /* A window alone is asked for to see its buffer as the controller reads
   * it, and what blends through a buffer is its alpha: kept beside the
   * picture, one byte a pixel. */
  const bool alpha = PictureCapture::OnlyWindow() >= 0;
  for (const Kept &k : kept_) {
    if (alpha && !k.pixels.empty()) {
      char path[96];
      snprintf(path, sizeof(path), "/data/misc/hwc-cap-%04d-panel-alpha.pgm",
               k.seq);
      if (FILE *out = fopen(path, "wb")) {
        fprintf(out, "P5\n%u %u\n255\n", width_, height_);
        for (size_t i = 3; i < k.pixels.size(); i += 4)
          fputc(k.pixels[i], out);
        fclose(out);
      }
    }
    if (k.pixels.empty())
      PictureCapture::WriteTakenText(k.seq, PictureCapture::Source::kPanel,
                                     k.words);
    else
      PictureCapture::WriteTaken(k.seq, PictureCapture::Source::kPanel,
                                 k.pixels.data(), width_, height_, width_ * 4,
                                 k.words);
  }
  ALOGI("panel mirror: %zu frame(s) written", kept_.size());
  kept_.clear();
  kept_.shrink_to_fit();
}

void PanelMirror::Frame(std::vector<Window> windows, const std::string &words) {
  Lift();

  const int seq = PictureCapture::Take(PictureCapture::Source::kPanel);
  if (seq < 0) {
    WriteOut();
    return;
  }

  if (!target_)
    target_ = vic_->AllocateZoneTarget(width_, height_);

  auto p = std::make_unique<Pending>();
  p->seq = seq;
  p->words = words;

  /* Back to front, as the engine lays them: the controller's larger depth
   * is further back. Each window keeps its own crop and lands a quarter the
   * size, where it landed on the panel. */
  std::sort(windows.begin(), windows.end(),
            [](const Window &a, const Window &b) { return a.depth > b.depth; });
  std::vector<VicSession::Layer> layers;
  const int only = PictureCapture::OnlyWindow();
  for (const Window &w : windows) {
    if (only >= 0 && w.index != only)
      continue;
    VicSession::Layer l = w.layer;
    l.display_left /= static_cast<int32_t>(kScale);
    l.display_top /= static_cast<int32_t>(kScale);
    l.display_right /= static_cast<int32_t>(kScale);
    l.display_bottom /= static_cast<int32_t>(kScale);
    if (l.display_right > l.display_left && l.display_bottom > l.display_top)
      layers.push_back(l);
  }
  if (layers.size() > VicSession::kMaxLayers)
    layers.erase(layers.begin(),
                 layers.end() - static_cast<long>(VicSession::kMaxLayers));

  if (target_ && !layers.empty()) {
    p->done = vic_->Compose(*target_, layers, width_, height_);
    p->drawn = static_cast<bool>(p->done);
  }
  pending_ = std::move(p);
}

}  // namespace hwc
}  // namespace android
