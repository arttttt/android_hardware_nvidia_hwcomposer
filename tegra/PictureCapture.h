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

#ifndef TEGRA_PICTURE_CAPTURE_H
#define TEGRA_PICTURE_CAPTURE_H

#include <cstdint>
#include <string>

#include <cutils/native_handle.h>

namespace android {
namespace hwc {

/* Pictures of what this composer drew, and what it was told to draw, taken
 * when somebody asks and at no other time.
 *
 * Every composition this process makes for itself -- the image engine's
 * merges, the GPU's client range -- lands in a buffer this process can map,
 * and every question about a defect that lives one frame long is a question
 * about those bytes. Which engine drew it makes no difference to the
 * question: a picture is a picture. So any source hands its buffer here,
 * with the words that say what the frame was made of, and gets back a file
 * pair under /data/misc:
 *
 *   hwc-cap-NNNN-<source>.ppm   every fourth pixel along both axes
 *   hwc-cap-NNNN-<source>.txt   what the source said about the frame
 *
 * NNNN is one count across every source, so the files read in the order the
 * frames were made whichever engine made them.
 *
 * Asked for through the dump, never through the frame: the frame asks only
 * Wanted(), which is one load. The ask is two properties, read when
 * `dumpsys SurfaceFlinger` runs --
 *
 *   vendor.hwc.test.capture       how many frames each source gives; 0 stops
 *   vendor.hwc.test.capture.from  which sources, comma separated, of
 *                                 merge, merge-turned, gpu, frame; all but
 *                                 merge-turned when unset
 *
 * -- and a dump that finds the same two values as the last one asks nothing,
 * so a run is not restarted by every dump that happens to come along. A new
 * number starts a new run. */
class PictureCapture {
 public:
  enum class Source : uint8_t {
    /* Every group the image engine drew. */
    kMerge,
    /* Only the groups that carried a turn: a rotation's frames, which a
     * still scene drawing merge after merge would otherwise use up. */
    kMergeTurned,
    /* The GPU executor's client range. */
    kGpu,
    /* No picture: the windows each frame put on the panel. */
    kFrame,
    kCount,
  };

  /* Reads the ask and says, in a line for the dump, where the run stands. */
  static std::string Arm();

  /* Whether the run takes words only -- no wait, no lock, no file of
   * pixels -- so that it leaves the frame's pace as it found it.
   * vendor.hwc.test.capture.words=1, read with the rest of the ask. */
  static bool WordsOnly();

  /* Whether `source` still owes frames. All the frame ever pays. */
  static bool Wanted(Source source);

  /* `pixels` laid out in rows of `pitch` bytes, four bytes a pixel in the
   * allocator's RGBA order. `done` is waited on before a byte is read: until
   * it comes due the picture is the previous frame's. Borrowed; -1 for
   * none. */
  static void Write(Source source, const void *pixels, uint32_t width,
                    uint32_t height, uint32_t pitch, int done,
                    const std::string &text);

  /* The same for an allocator's buffer, in whatever arrangement it was
   * made: memory arranged in blocks is read back into rows here, which
   * locking it does not do. */
  static void WriteBuffer(Source source, buffer_handle_t buffer, uint32_t width,
                          uint32_t height, int done, const std::string &text);

  /* The words alone, for a frame with nothing to picture. */
  static void WriteText(Source source, const std::string &text);
};

}  // namespace hwc
}  // namespace android

#endif
