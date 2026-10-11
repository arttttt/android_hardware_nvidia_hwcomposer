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

#ifndef TEGRA_PANEL_MIRROR_H
#define TEGRA_PANEL_MIRROR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tegra/VicSession.h"
#include "utils/fd.h"

namespace android {
namespace hwc {

/* What the panel showed, frame by frame, at the pace it showed it.
 *
 * Every other picture this composer can take is of one engine's buffer, and
 * a defect that every engine's buffer answers for correctly is not in any of
 * them: it is in what the controller made of the windows together. The
 * controller keeps no copy, so one is made the way it makes its own: each
 * window's buffer, cropped, placed and stacked as the flip described it, laid
 * down by the image engine into a quarter-size picture of our own.
 *
 * And at the pace the frames ran, because the defects this exists for are
 * the ones that vanish when the frames slow down -- a capture that waits on
 * the GPU and writes files every frame drew them all correctly. So the frame
 * pays one engine submit and nothing more: the engine waits for the buffers
 * itself, the picture is lifted out at the next frame when it is long done,
 * kept in memory, and written out only when the run is over.
 *
 * Asked for as PictureCapture's "panel" source. */
class PanelMirror {
 public:
  /* One window as the flip put it up, in the engine's terms: its buffer,
   * the part of it shown and where on the panel, blending and opacity, and
   * the fence the controller waited on. `depth` is the controller's: the
   * larger, the further back. */
  struct Window {
    uint32_t depth = 0;
    VicSession::Layer layer{};
  };

  PanelMirror(VicSession *vic, uint32_t panel_width, uint32_t panel_height);
  ~PanelMirror();

  PanelMirror(const PanelMirror &) = delete;
  PanelMirror &operator=(const PanelMirror &) = delete;

  /* Whether a picture is still in hand: the frame after a run ended still
   * owes it a visit, to lift the last picture and write the run out. */
  bool Busy() const { return pending_ || !kept_.empty(); }

  /* Called after each flip while a run is wanted, and once more after.
   * Lifts the previous picture, then lays down this frame's. */
  void Frame(std::vector<Window> windows, const std::string &words);

 private:
  void Lift();
  void WriteOut();

  VicSession *const vic_;
  const uint32_t width_;
  const uint32_t height_;
  std::unique_ptr<VendorBuffer> target_;

  struct Pending {
    int seq = -1;
    drm_hwcomposer::SharedFd done;
    std::string words;
    bool drawn = false;
  };
  std::unique_ptr<Pending> pending_;

  struct Kept {
    int seq = -1;
    std::vector<uint8_t> pixels; /* width_ * height_ * 4, rows dense */
    std::string words;
  };
  std::vector<Kept> kept_;
};

}  // namespace hwc
}  // namespace android

#endif
