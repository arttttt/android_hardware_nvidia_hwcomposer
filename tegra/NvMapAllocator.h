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

#ifndef TEGRA_NVMAP_ALLOCATOR_H
#define TEGRA_NVMAP_ALLOCATOR_H

#include <cstdint>
#include <memory>

namespace android {
namespace hwc {

class ScratchBuffer;

/* The composer's own carveout zone, reached straight through /dev/nvmap.
 *
 * The kernel reserves 64M of physically contiguous memory for this
 * composer and names it heap bit 26; nothing else asks for it, so nothing
 * else can fragment it. This class is how a scratch gets born there: a
 * handle is created, the memory allocated straight from the zone, and a
 * dma-buf taken out for the engine to pin. The surface descriptor the
 * engine reads is built by the device's own libnvrm, not laid out here --
 * that structure has already been caught outgrowing its last published
 * header, so the library is the only one that knows its shape.
 *
 * The address a zone buffer carries is a physical one, made legible by
 * the linear mapping the kernel hangs over the whole zone in the
 * address spaces of the host1x domain -- the engine's and the
 * display's. Outside that domain the number is not an address at all,
 * so these dma-bufs are for this composer's own consumers only and are
 * never exported further; the pools that hand them out live and die
 * inside it.
 *
 * Resolved once, lazily, like the gralloc wrapper beside it, and with
 * the same permanence: if the zone or any of the three library calls it
 * needs cannot be had at first ask, every later ask fails the same way
 * and the pools fall back to gralloc -- a path that is gone for the
 * boot, never retried, so a broken zone cannot turn into a slow one.
 */
class NvMapAllocator {
 public:
  /* The allocator, or nullptr if the zone could not be reached. */
  static NvMapAllocator *GetInstance();

  /* One carveout buffer of `width` by `height` thirty-two bit pixels,
   * laid out in rows -- the arrangement the merge engine writes and the
   * fourth window reads. Null, having said what failed, when the zone
   * would not give it: the caller falls back to gralloc and says so in
   * the dump. */
  std::unique_ptr<ScratchBuffer> Allocate(uint32_t width, uint32_t height);

  /* How many times the zone has refused -- the dump's heartbeat for
   * whether the fallback ever actually runs. */
  uint64_t refusals() const { return refusals_; }

  /* What the library actually wrote into the surface's colour-format
   * word, and whether it agreed with the constant this code asked for.
   * Read back once per buffer, for the dump alone: a mismatch is the
   * earliest, quietest sign that the device's libnvrm and this code
   * have drifted apart about what thirty-two bit colour means. */
  uint32_t surface_format() const { return surface_format_; }
  uint32_t expected_format() const { return kFormatA8B8G8R8; }
  bool format_mismatch() const { return format_mismatch_; }

  /* Frees a memory handle this allocator made. Called by the scratch
   * buffer's destructor; a no-op for a null handle, which lets buffers
   * be released whether or not the allocator ever resolved. */
  static void ReleaseMemHandle(void *mem_handle);

  ~NvMapAllocator();

  NvMapAllocator(const NvMapAllocator &) = delete;
  NvMapAllocator &operator=(const NvMapAllocator &) = delete;

 private:
  NvMapAllocator();

  /* Thirty-two bit colour as this device's library spells it: the value
   * standing beside the letters A8B8G8R8 in the library's own name-keyed
   * format table, read out of the binary. Little-endian bytes R, G, B, A
   * in memory -- the framework's RGBA_8888 -- with the swizzle code in
   * the second byte and the bits per pixel packed into the low one: the
   * library's own stride arithmetic multiplies the width by exactly that
   * low byte. The last headers published with source, a generation
   * older, build a different constant with the pixel count in the high
   * byte; spelled that way the word reads here as twenty-six bits per
   * pixel, and nothing built on it will be taken for colour. */
  static constexpr uint32_t kFormatA8B8G8R8 = 0x00532120;

  /* The colour space, travelling separately from the format in the word
   * after it. The library's table lists the sRGB and 709 spellings of
   * the same format against the numbers two and three; plain linear
   * colour is one, which is what every caller that says nothing passes. */
  static constexpr uint32_t kColorSpaceLinearRgba = 1;

  bool usable_ = false;
  int nvmap_fd_ = -1;
  void *nvrm_library_ = nullptr;

  /* libnvrm. The mem handle wraps the dma-buf for the engine, and the
   * surface setup is the library's own, memsetting and filling the
   * descriptor of whatever shape this device's build really is. */
  int (*mem_handle_from_fd_)(int, void **) = nullptr;
  void (*mem_handle_free_)(void *) = nullptr;
  void (*surface_init_rm_pitch_)(void *, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, void *,
                                 uint32_t) = nullptr;

  uint64_t refusals_ = 0;
  uint32_t surface_format_ = 0;
  bool format_mismatch_ = false;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_NVMAP_ALLOCATOR_H
