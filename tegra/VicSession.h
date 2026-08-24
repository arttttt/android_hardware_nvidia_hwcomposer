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

#ifndef TEGRA_VIC_SESSION_H
#define TEGRA_VIC_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <cutils/native_handle.h>

#include "utils/fd.h"

namespace android {

namespace drm_hwcomposer {
class NvGralloc;
}  // namespace drm_hwcomposer

namespace hwc {

/* A merge buffer that is ours from birth to death.
 *
 * Cut from the composer's own carveout zone through nvmap: the memory is
 * the composer's, the dma-buf is the composer's, and the library is given
 * no ownership at all. The surface descriptor is still built by the
 * library's own call rather than assembled by hand, but the handle word it
 * is told about is our own descriptor -- the kernel resolves it through the
 * process's file table, so whose nvmap client it is does not matter. What
 * this side adds is the dma-buf for the window, a CPU mapping of the whole
 * buffer -- the slot is painted through it before its first use, and the
 * dump reads the result back through it -- and the words of the descriptor
 * as the library filled them.
 *
 * Releases itself: the mapping is unmapped and the descriptor closed, and
 * nothing more -- the fd is the only thing holding the zone's block by the
 * time the buffer dies. */
struct VendorBuffer {
  VendorBuffer() = default;
  ~VendorBuffer();

  VendorBuffer(const VendorBuffer &) = delete;
  VendorBuffer &operator=(const VendorBuffer &) = delete;

  /* Borrowed by the window for the duration of a frame; owned here. */
  int mem_fd() const { return fd ? *fd : -1; }

  drm_hwcomposer::SharedFd fd;

  /* The surface descriptor as the library built it, kSurfaceWords long. */
  std::vector<uint32_t> surface;

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0; /* bytes */
  uint32_t size = 0;  /* bytes, the descriptor's own size word */

  /* The whole buffer, mapped for the processor: the paint before the first
   * use and the dump's reads both go through here. */
  uint32_t *pixels = nullptr;
  size_t mapped = 0;
};

/* The image compositor this chip has, and the display controller does not.
 *
 * The controller can show a handful of buffers at once and no more -- three of
 * its windows are usable here, and the buffer anything composited lands in
 * takes one of them, so a frame carries two layers in hardware and everything
 * else has to be merged first. Today that merge is done by SurfaceFlinger on
 * the GPU, which is why a quarter of the pixels of an application opening go
 * through the graphics core.
 *
 * This is the other engine that can do it. VIC is a fixed-function block on
 * the host1x bus, made for exactly this: read several surfaces, scale them,
 * blend them, write one. It is idle on this device -- nothing has ever opened
 * it -- and using it means the graphics core is never woken for a merge.
 *
 * Reached through the libraries the board ships rather than built against
 * them, for the same reason as the allocator in bufferinfo/NvGralloc.h: the
 * vendor blob repository declares them by installation path rather than as
 * modules, so there is nothing for a module here to link to.
 */
class VicSession {
 public:
  /* The compositor, or nullptr if this device has none it can reach, having
   * logged what was missing. */
  static std::unique_ptr<VicSession> Create();

  ~VicSession();

  VicSession(const VicSession &) = delete;
  VicSession &operator=(const VicSession &) = delete;

  /* How many surfaces the engine takes in one pass.
   *
   * Fixed rather than asked, and deliberately. The number the hardware
   * reports lives inside the session structure, which would mean knowing that
   * structure's layout -- the one thing this class is built to avoid. Five is
   * what the interface reserves room for, and a set the engine will not take
   * is refused at configure or execute time anyway, which is the answer we
   * actually act on. */
  static constexpr size_t kMaxLayers = 5;

  /* One thing to draw into the merge. Rectangles are in the coordinates of
   * the buffer being drawn into, which for a merge is the display. */
  struct Layer {
    buffer_handle_t handle;

    float source_left;
    float source_top;
    float source_right;
    float source_bottom;

    int32_t display_left;
    int32_t display_top;
    int32_t display_right;
    int32_t display_bottom;

    /* Whether the colour in the buffer has already been multiplied by its own
     * alpha. The engine is told which, rather than being left to assume, so
     * that a layer arriving either way lands correctly. */
    bool premultiplied;

    /* Applied on top of whatever alpha the pixels carry. One means the layer
     * contributes at its own opacity. */
    float alpha;

    /* Borrowed. The engine is told to wait on it before reading, and does not
     * take ownership -- the caller closes it as it would have anyway. */
    int acquire_fence;

    /* Set when the source is a buffer this composer allocated itself,
     * cut from the zone. Then `handle` is null and the words the engine
     * reads the source by are the ones the library built when the buffer
     * was born, rather than the allocator's. No caller passes one today;
     * this seat is how a zone buffer would be read.
     *
     * Borrowed: the pool that lent the buffer outlives the frame. */
    const VendorBuffer *vendor = nullptr;
  };

  /* Draws `layers` into `target`, bottom of the list first.
   *
   * `target_ready` says when the target may be written to -- it is the fence
   * of the frame that replaced whatever `target` was last showing, and until
   * it comes due the display is still reading that buffer. Handed to the
   * engine rather than waited for: the engine is perfectly able to wait, it
   * does so without occupying a thread, and it is the only way every buffer
   * can stay in rotation. Waiting on it here instead, or refusing to offer a
   * buffer until it is due, costs exactly the buffers it protects -- which is
   * why the vendor's own composer passes it along the same way, as the
   * acquire fence of its scratch surface.
   *
   * Borrowed like the layers' own: read, not taken.
   *
   * Returns the fence to wait on before the result is read, or an empty one
   * if the engine would not take this set -- which is not a fault and is
   * counted rather than complained about: it is the signal that the merge has
   * to go somewhere else. Nothing has been written to `target` in that case.
   *
   * More than kMaxLayers layers is refused the same way.
   */
  /* `width` and `height` bound the region actually drawn, measured from
   * the buffer's origin; nought means the whole buffer. The caller that
   * shows only a corner of the target has no reason to pay for the rest of
   * it being written.
   *
   * The target is always a buffer of ours, cut from the zone: its
   * descriptor is the library's own and its bounds are known without
   * asking anyone. There is no entry point for composing into a buffer
   * the allocator made, and that is deliberate -- a second origin is a
   * second set of rules, and the last time this class had two, the one
   * nobody was watching wrote the fault we spent a month chasing in the
   * other. */
  drm_hwcomposer::SharedFd Compose(const VendorBuffer &target,
                                   const std::vector<Layer> &layers,
                                   uint32_t width = 0, uint32_t height = 0,
                                   int target_ready = -1,
                                   uint32_t transform = 0);

  /* The target descriptor as it stood the last time a frame was handed to
   * the engine with a vendor-born target: the memory handle in the word
   * the reloc is built from, and the descriptor's leading words. Read at
   * the boundary with the library -- after the target is configured,
   * before the job is submitted -- so the dump can say whether the handle
   * the buffer was born with is the handle the engine was told about.
   * Empty while no vendor-born target was ever composed. */
  const std::string &last_target_probe() const { return last_target_probe_; }

  /* Whether the composer's own carveout zone can be offered: the library's
   * import entry point and its descriptor builder resolved. The zone
   * itself is only knocked on when an allocation is actually asked for. */
  bool OffersZoneBuffers() const;

  /* A buffer cut from the composer's own carveout zone: the memory is
   * allocated straight from nvmap under the zone's heap bit, exported as
   * a dma-buf, and handed to the library's own import call -- through this
   * session's instance of the library, so the handle is born in the client
   * the engine calls its own. The descriptor is the library's own, built
   * by its pitch-layout call.
   *
   * Every buffer this composer allocates for itself comes from here: the
   * slots a merged frame lands in, and the intermediates a turned layer is
   * copied into. Nothing of ours is asked of gralloc any more -- one
   * origin means one set of rules to get right, and the era when turned
   * copies came from one place and merges from another is exactly how a
   * month went into looking for a fault in the target that lived in the
   * sources.
   *
   * The allocation is write-combined and the paint mapping follows it:
   * the zone's previous incarnation zeroed its slots through a cacheable
   * mapping and never flushed, and the stale lines that later evicted
   * over the engine's writes were the likely shape of that era's
   * corruption. Cacheable mappings of these buffers are not made.
   *
   * `pitch_grain` is what the row length is rounded up to. The default is
   * what the engine's parser demands of a target it writes in rows; a
   * caller whose reader wants dense rows instead -- the controller's
   * cursor unit scans them with no padding at all -- asks for four.
   *
   * Null, having said what failed, if the zone would not give one. */
  std::unique_ptr<VendorBuffer> AllocateZoneTarget(uint32_t width,
                                                   uint32_t height,
                                                   uint32_t pitch_grain = 256);

  /* Copies one layer into `target`, untransformed. The write is bounded to
   * `width` by `height` from the origin, which the caller sizes to the
   * crop. Born as the per-layer turning pass; the turning died with the
   * folded merge, and the one caller left is the cursor unit staging its
   * sprite -- a straightening copy, nothing turned.
   *
   * The copy is verbatim: blending is told to ignore alpha, so the pixels
   * arrive in the target exactly as they left the source, and the layer's
   * own alpha and premultiplication are honoured later, by whoever shows
   * the copy. This is how the stock blit copied too.
   *
   * Fences as in Compose: the layer's acquire is handed to the engine, the
   * returned fence says when the copy may be read. `target_ready` is
   * usually not needed at all -- the channel serialises our passes, so a
   * pass submitted after the group that read this buffer runs after it. */
  drm_hwcomposer::SharedFd CopyLayer(const VendorBuffer &target,
                                     const Layer &layer,
                                     uint32_t width, uint32_t height,
                                     int target_ready = -1);

  /* How many sets the engine has refused, and how many it has taken. What
   * decides whether a third way of merging is worth building at all. */
  uint64_t composed() const { return composed_; }
  uint64_t refused() const { return refused_; }

  /* How often the zone would not give a buffer.
   *
   * Read beside what the pools are holding, never alone: memory in use
   * says nothing on its own -- fifty megabytes with no refusal is a zone
   * doing its work, the same fifty with three refusals is a zone too
   * small for the scene, and only the second is a reason to act. The
   * bytes come from the pools, which own the buffers; this side only
   * knows what it would not give. */
  uint64_t zone_refusals() const { return zone_refusals_; }

 private:
  VicSession() = default;

  bool Open();
  bool Resolve();

  /* The body both Compose entry points share, once the target's surfaces
   * and real extent are known -- from the allocator for a gralloc handle,
   * from the buffer itself for a vendor-born one. `probe_words` is the
   * vendor-born target's descriptor, or null for a gralloc one: when set,
   * the words the job is submitted with are remembered for the dump. */
  drm_hwcomposer::SharedFd ComposeInto(drm_hwcomposer::NvGralloc *gralloc,
                                       const void *target_surfaces,
                                       uint32_t buffer_width,
                                       uint32_t buffer_height,
                                       const std::vector<Layer> &layers,
                                       uint32_t width, uint32_t height,
                                       int target_ready,
                                       const uint32_t *probe_words,
                                       uint32_t transform);

  /* Their settings structure, held as a plain run of bytes.
   *
   * Its fields are never touched here. The library has its own calls for
   * filling them -- one for the target, one per source, one for blending --
   * so what this class needs is somewhere to put the answer, not knowledge of
   * where each answer goes. That is not laziness but the only safe reading:
   * the structure belongs to the library and has no promise of stability, and
   * the allocator on this same device has already been caught having grown a
   * field since the last published version of its own (see NvGralloc.h).
   *
   * Sized well above what the published layout adds up to, and zeroed before
   * every use, so that a structure which has since grown is still written
   * inside memory that is ours. */
  static constexpr size_t kConfigBytes = 16384;

  void *nvrm_library_ = nullptr;
  void *graphics_library_ = nullptr;
  void *vic_library_ = nullptr;

  /* Opaque on purpose -- see kConfigBytes. */
  void *rm_device_ = nullptr;
  void *session_ = nullptr;
  std::vector<uint8_t> config_;

  uint64_t composed_ = 0;
  uint64_t zone_refusals_ = 0;
  uint64_t refused_ = 0;

  /* See last_target_probe(). */
  std::string last_target_probe_;

  /* libnvrm.
   *
   * The opener is the one without a device number. The interface has both,
   * and the one this device exports is the newer -- which on its own settles
   * that the compositor shipped alongside these libraries could not be
   * reused: it was built against the older name, which is gone. */
  int (*rm_open_)(void **) = nullptr;
  void (*rm_close_)(void *) = nullptr;

  /* libnvrm again -- the zone path. The library's own builder of a
   * pitch-layout descriptor, the one surviving piece of what the zone used
   * to lean on: the import that adopted a dma-buf as a handle of this
   * client is gone, because the handle word the descriptor carries is now
   * our own descriptor and the kernel resolves it through the process's
   * file table. */
  void (*surface_init_rm_pitch_)(void *, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, void *,
                                 uint32_t) = nullptr;
  int nvmap_fd_ = -1;

  /* Thirty-two bit colour as the engine spells it, and the tag gralloc's
   * own RGB output carries -- the pair proven against a gralloc-produced
   * descriptor, see docs/nvrm-format-table.txt for why the name and the
   * code read as different generations. The descriptor's size word sits at
   * [14], which the pitch-layout builder does not fill, and the memory
   * handle the engine's reloc is built from sits at [6]. */
  /* The name lies. In the library's format table
   * (docs/nvrm-format-table.txt) this code reads as R8G8B8A8, and A8B8G8R8
   * is 0x00d12120. The value stands because it is the code gralloc sends
   * and the window expects -- but the name would one day tempt someone to
   * "correct" the constant and turn the channels around. */
  static constexpr uint32_t kFormatBlobRgba = 0x00532120;
  static constexpr uint32_t kColorTagRgb = 1;
  static constexpr size_t kSurfaceWordMemHandle = 6;
  static constexpr size_t kSurfaceWordSize = 14;
  static constexpr size_t kSurfaceWords = 64;

  /* libnvrm_graphics -- turning the engine's own fences into descriptors the
   * rest of the system understands, and back. */
  int (*fence_from_fd_)(int32_t, void *, uint32_t *) = nullptr;
  int (*fence_to_fd_)(const char *, const void *, uint32_t, int32_t *) = nullptr;

  /* libnvddk_vic.
   *
   * The session maker takes three arguments here and only two in the last
   * source drop published: a channel to the engine was added between the
   * device and the answer. Passing zero for it is asking the library to open
   * its own, which is what we want and what its own callers do.
   *
   * Getting this wrong was not a failure to compile but a failure to work.
   * The second argument was the address of the answer, which is never zero,
   * so the library took it for a channel that was already open, never opened
   * one, and issued its commands against a descriptor read out of a stack
   * address -- which came out as zero, hence a stream of them against
   * standard input. Established by decompiling the shipped library. */
  int (*create_session_)(void *, int, void **) = nullptr;
  void (*free_session_)(void *) = nullptr;
  int (*configure_)(void *, void *) = nullptr;
  int (*execute_)(void *, void *, void *, uint32_t, void *) = nullptr;
  int (*configure_source_)(void *, void *, uint32_t, const void *, uint32_t,
                           const void *, const void *) = nullptr;
  int (*configure_target_)(void *, void *, const void *, uint32_t,
                           const void *) = nullptr;
  void (*configure_blending_)(void *, void *, uint32_t, int, float) = nullptr;
  int (*configure_clear_rects_)(void *, void *) = nullptr;

  /* Returns nothing -- the leaked header declares it void, and reading a
   * result out of a void call is reading a scratch register: it once made
   * every turn look refused. Any objection to the transform surfaces at
   * configure time instead. */
  void (*configure_transform_)(void *, void *, uint32_t) = nullptr;
};

}  // namespace hwc
}  // namespace android

#endif  // TEGRA_VIC_SESSION_H
