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

#pragma once

#include <cutils/native_handle.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "display/AtomicStateManager.h"
#include "display/DrmMode.h"
#include "tegra/DcHead.h"
#include "tegra/CursorUnit.h"
#include "tegra/RefreshGovernor.h"
#include "tegra/ScratchPool.h"
#include "tegra/TurnPool.h"
#include "tegra/VicSession.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

/* One commit, described in the terms this hardware takes.
 *
 * Built from the arguments and then handed back to be carried out, so that
 * deciding what to do and doing it stay two steps -- the first can be asked
 * "would this work" without the second happening.
 *
 * A commit is not always a frame. Upstream puts the frame, the timing and
 * whether the display is lit into one atomic request, because on a DRM
 * display all three are properties of the same objects. Here they are three
 * different devices, so the request carries them separately and the order
 * they are applied in is decided when it is executed.
 */
class TegraAtomicRequest : public AtomicRequest {
 public:
  /* What is to be drawn by the image compositor rather than shown straight,
   * and which window slot the result goes to.
   *
   * Empty on every frame that needs no merge, which is most of them. */
  struct Merge {
    std::vector<hwc::VicSession::Layer> layers;

    /* Whose pixels each layer's are, in step with `layers`. The engine has
     * no use for this; it is what lets the drawn result be recognised when
     * the same group comes back the next frame. Nought where the platform
     * could not name the buffer, and a group with a nameless member is drawn
     * fresh every time. */
    std::vector<uint64_t> source_ids;

    /* How each member is turned, in step with `layers`: the framework's
     * bits, hflip | vflip << 1 | rotate90 << 2, nought for most. A turned
     * member is drawn turned into an intermediate by its own engine pass
     * before the group composes, and a turn that changed is a different
     * picture -- so this is part of what the drawn result is recognised
     * by. */
    std::vector<uint8_t> transforms;

    /* The group's own frame of reference. The layers' rectangles are held
     * relative to this corner, and the engine draws them from the buffer's
     * origin -- where the group sits on the panel is the window's business
     * alone. Pixels and position stay independent axes, which is what the
     * stock composer's scratch path did and what lets the window scan a
     * group's worth of memory rather than a panel's. */
    /* Uninitialised like their neighbours below, and it has to be: a
     * default here trips the compiler on the enclosing class's `= {}`
     * default argument, and value-initialisation zeroes them anyway. */
    int32_t origin_x;
    int32_t origin_y;
    uint32_t width;
    uint32_t height;

    size_t slot;
    int32_t window;
    uint32_t depth;
  };

  /* The one small thing the controller's own cursor is to show, if
   * anything. Absent on every frame without a pointer, which is most of
   * them. Uninitialised for the same reason as the Merge fields above --
   * value-initialisation zeroes them where the request is built. */
  struct Cursor {
    bool present;
    buffer_handle_t handle;
    uint64_t id;
    uint32_t width;
    uint32_t height;
    uint32_t stride_px;
    bool premultiplied;
    int32_t x;
    int32_t y;

    /* Which move of the pointer this position belongs to -- the manager's
     * count at the moment the plan was drawn up. A move landing between
     * plan and execution advances the count and leaves this behind, and a
     * position overtaken like that must not be re-stated over the fresher
     * one. */
    uint64_t seq;

    /* When the sprite's pixels are done being drawn -- the framework
     * paints its pointer on the graphics core, and reading it before
     * this comes due reads a half-painted sprite. */
    SharedFd acquire;
  };

  /* The frame's own record: what the plan decided about each layer.
   *
   * Carried by the request rather than asked of the plan again, because the
   * request is also built for frames that are only weighed -- and the frame
   * ring this feeds must hear only of frames that actually went up. */
  struct FrameNote {
    /* LayerToPlaneJoiningPlan::Steering, as a number: the plan's header is
     * not this one's business, and the ring prints the word. */
    int steering = 0;

    struct Row {
      /* The allocator's name for the buffer; nought where it has none. */
      uint64_t id = 0;
      /* Where the layer was seated: 'w' a window, 'm' the merge, 'c' the
       * cursor unit. */
      char seat = 'w';
      float alpha = 1.F;
      /* Source against destination, one per axis: 1 where the layer is
       * shown unresized. */
      float scale_w = 1.F;
      float scale_h = 1.F;
      /* Where on the panel the layer lands. */
      int32_t x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<Row> layers;
  };

  TegraAtomicRequest(std::vector<hwc::DcHead::Window> windows,
                     std::vector<buffer_handle_t> handles,
                     std::vector<uint64_t> handle_ids,
                     bool has_composition,
                     std::optional<PowerMode> power_mode,
                     Merge merge = {},
                     std::shared_ptr<const HalColorTransformMatrix>
                         color_matrix = nullptr,
                     Cursor cursor = {},
                     FrameNote note = {})
      : windows_(std::move(windows)),
        handles_(std::move(handles)),
        handle_ids_(std::move(handle_ids)),
        has_composition_(has_composition),
        power_mode_(power_mode),
        merge_(std::move(merge)),
        color_matrix_(std::move(color_matrix)),
        cursor_(cursor),
        note_(std::move(note)) {
  }

  const Merge &GetMerge() const {
    return merge_;
  }

  const Cursor &GetCursor() const {
    return cursor_;
  }

  /* The colour transform this frame is to be shown under, or null where the
   * caller said nothing. Identity means "none": the display level already
   * folds the client's and the render intent's matrices into one, and
   * substitutes the identity wherever the GPU is taking care of it. */
  const std::shared_ptr<const HalColorTransformMatrix> &GetColorMatrix()
      const {
    return color_matrix_;
  }

  const std::vector<hwc::DcHead::Window> &GetWindows() const {
    return windows_;
  }

  /* The buffer behind each window, as the allocator knows it, in step with
   * GetWindows(); null where a window shows nothing.
   *
   * Carried because a window is described in the controller's terms and the
   * allocator answers to none of them: preparing a buffer for the display
   * takes its own handle. Kept beside the windows rather than inside one,
   * because it is not something the controller is told -- it is what has to
   * happen before the controller is told anything.
   */
  const std::vector<buffer_handle_t> &GetHandles() const {
    return handles_;
  }

  /* The allocator's unique name for each buffer, in step with GetHandles();
   * nought where a window shows nothing or the platform could not name it.
   * Carried because a handle is an address and an address can be reused:
   * anything remembering a buffer across frames must remember the name. */
  const std::vector<uint64_t> &GetHandleIds() const {
    return handle_ids_;
  }

  /* Whether anything is to be shown. A commit that only changes the power
   * state must not post a frame: every window of the head goes into a flip,
   * so flipping without a composition would blank the display as a side
   * effect of turning it on. */
  bool HasComposition() const {
    return has_composition_;
  }

  const std::optional<PowerMode> &GetPowerMode() const {
    return power_mode_;
  }

  const FrameNote &GetFrameNote() const {
    return note_;
  }

 private:
  const std::vector<hwc::DcHead::Window> windows_;
  const std::vector<buffer_handle_t> handles_;
  const std::vector<uint64_t> handle_ids_;
  const bool has_composition_;
  const std::optional<PowerMode> power_mode_;
  const Merge merge_;
  const std::shared_ptr<const HalColorTransformMatrix> color_matrix_;
  const Cursor cursor_;
  const FrameNote note_;
};

/* Turns plans into frames on this controller.
 *
 * Every window of the head goes into every frame, whether or not a layer
 * claimed it: a window keeps what it was last given until told otherwise, so
 * one left out of a frame stays on screen over the top of it.
 */
class TegraAtomicStateManager : public AtomicStateManager {
 public:
  /* All five belong to the pipeline and outlive this. `vic` and `scratch`
   * are null together on a device that was not asked for the image
   * compositor, which is every device by default; nothing then reaches the
   * merge. `cursor` is null on a head whose cursor another descriptor
   * holds, and the cursor seat is then simply never offered. */
  TegraAtomicStateManager(hwc::DcHead &head,
                          const std::vector<DrmMode> &modes,
                          hwc::VicSession *vic, hwc::ScratchPool *scratch,
                          hwc::CursorUnit *cursor,
                          hwc::RefreshGovernor *governor)
      : head_(head), modes_(modes), vic_(vic), scratch_(scratch),
        cursor_(cursor), governor_(governor) {
    count_fences_ = CountFencesFromProperty();
    throttle_to_one_frame_ = ThrottleFromProperty();
    report_engine_reads_ = EngineReadsFromProperty();
    merge_cache_ = MergeCacheFromProperty();
    cmu_ctm_ = CmuFromProperty();
    calibrated_home_ = Properties::CalibratedColorMode();
  }

  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs &args) override;

  bool IsActive() const override {
    return active_;
  }

  void WaitLastFrame() override;

  void SetActive(bool active) {
    active_ = active;
  }

  /* Would the controller take this frame? Nothing is shown and nothing
   * changes. */
  bool Test(const AtomicRequest &request);

  /* Shows it. The fence handed back signals when this frame is on the panel;
   * see the note in the implementation on why that is not the fence this
   * flip returned. */
  int Execute(const AtomicRequest &request, AtomicCommitResult *out_result);

  void NoteFrameUnjudged() override {
    ForgetMerge();
  }

  /* Moves the hardware cursor, if this head has one and it is showing.
   * Arrives from the framework between frames, at the pointer's own rate;
   * touches nothing a frame owns, which is the whole point of the unit. */
  void MoveCursor(int32_t x, int32_t y) override {
    /* The pointer is life too, and it moves without frames by design --
     * a panel left slow under it would drag the cursor at thirty. */
    if (governor_ != nullptr)
      governor_->NoteActivity();

    if (cursor_ != nullptr) {
      /* Counted before the unit hears of it: every plan carries the count
       * it was drawn under, and a mismatch at execution says a move like
       * this one has overtaken the plan's position. */
      ++cursor_move_seq_;
      cursor_->Move(x, y);
    }
  }

  void NoteVsyncEnabled(bool enabled) override {
    if (governor_ != nullptr)
      governor_->NoteVsyncEnabled(enabled);
  }

  bool VsyncTimestampTrustworthy() override {
    return governor_ == nullptr || governor_->VsyncTimestampTrustworthy();
  }

 private:
  /* Lights the panel or puts it out, and remembers which. Not the head's
   * job: the controller posts frames and has no say over whether the display
   * is lit, so this goes to the framebuffer device on the same hardware. */
  int SetPowered(bool powered);

  std::string DumpState() override;

  hwc::DcHead &head_;

  /* The timings this panel runs, to check a requested one against. A fixed
   * panel has one, so the only mode that ever arrives here is the one already
   * in use -- but a request for another is a mistake worth refusing rather
   * than accepting and not carrying out. */
  const std::vector<DrmMode> &modes_;

  /* The engine that draws what will not fit a window, and somewhere for it to
   * write. Null together where the device was not asked for them. */
  hwc::VicSession *const vic_ = nullptr;

  /* Where turned copies land -- see TurnPool for why it is its own pool
   * and not the show pool's shape. As many turned members as the engine
   * takes sources in a pass; the memory behind them is lazy, cut to size,
   * and given back when nothing has turned for a while, so the cap prices
   * a scene that actually happens, not a reservation. No fences between
   * writes: the engine's channel serialises our passes, so the group that
   * read an intermediate has run before the next turn rewrites it -- held
   * by the driver's construction (serialize=true, one channel a session),
   * and by the stock blit's own reliance on the same. */
  static constexpr size_t kMaxRotatedMembers = hwc::VicSession::kMaxLayers;
  std::unique_ptr<hwc::TurnPool> rotate_pool_;

  /* Whether the frame being built has turned anything yet -- settled into
   * the pool's idle accounting at the top of the next Execute. */
  bool turned_in_frame_ = false;

  hwc::ScratchPool *const scratch_ = nullptr;

  /* The controller's cursor, or null where another descriptor holds it.
   * Whether the previous frame showed it, so a frame without a pointer
   * hides the sprite exactly once -- the unit draws independently of
   * frames, and a sprite nobody hides outlives its scene. */
  hwc::CursorUnit *const cursor_ = nullptr;
  bool cursor_shown_ = false;

  /* How many times the pointer has moved past the frames. A plan copies
   * this when drawn up; execution compares, and a plan overtaken by a
   * move keeps its hands off the position. Plain on purpose: every entry
   * into the device -- moves from the binder, plans and frames from the
   * main thread -- comes through the composer's one lock. */
  uint64_t cursor_move_seq_ = 0;

  /* Slows the panel when nobody draws, or null where the kernel offers
   * no such door. Owned by the pipeline, told of life from here: frames,
   * pointer moves, and the framework's own vsync confession. */
  hwc::RefreshGovernor *const governor_ = nullptr;

  /* Frames actually committed, whatever they carried. The counter the
   * cursor's whole promise is judged by: a moving pointer on a still
   * desktop must leave this exactly where it was. */
  uint64_t frames_executed_ = 0;

  bool active_ = true;

  /* What each window was last given, and so what has already been flattened.
   *
   * A buffer stays flat until something draws into it again, and a window
   * handed the same buffer as last time is showing pixels that were flattened
   * then. Keyed by window rather than by buffer because that is the question
   * being asked -- what is on this window now against what was on it before.
   */
  /* Keyed by the allocator's unique name for the buffer, not the handle
   * pointer: an address freed and reallocated can come back naming a
   * different buffer, and a stale match here would show compressed memory
   * as pixels. Nought names nothing and never matches. */
  std::map<int32_t, uint64_t> last_flattened_;

  /* Whether to keep only one frame in the air.
   *
   * A switch rather than a decision, because the decision has not been earned.
   * Upstream and every vendor bound the flip queue and we did not, which is a
   * good reason to add the bound and no reason at all to believe what it does
   * here -- the first measurement that said it helped turned out to be my own
   * metric misreading a broken-up animation as a fast one.
   *
   * So it can be turned off from the outside, and the two arrangements compared
   * on the panel rather than argued about.
   */
  bool throttle_to_one_frame_ = true;

  /* Whether to say which buffers the engine read rather than the display.
   *
   * On by default, because it is the vendor's documented contract rather than
   * an idea of ours, and off is the old behaviour of telling every layer to
   * wait for the flip. Kept switchable only so the two can be measured against
   * each other on the panel without building twice.
   */
  bool report_engine_reads_ = true;

  /* Asked of the system once, at construction. Not in Execute: what a frame
   * costs is the one thing being measured here, and a measurement that adds
   * to it is worth nothing. */
  static bool CountFencesFromProperty();
  static bool ThrottleFromProperty();
  static bool EngineReadsFromProperty();

  /* Had the fence handed out already come due when it was handed out?
   *
   * Counted rather than reasoned about, because reasoning has failed twice.
   * The kernel's own trace says a flip's fence comes due 10.5 ms after that
   * flip, and flips through the slow transition are 19.2 ms apart -- so the
   * fence handed over, belonging to the flip before, should have been due
   * some eight milliseconds before anyone received it, and nobody should
   * wait. The client waits anyway, fourteen milliseconds a frame. One of
   * those two measurements is wrong about something, and only this side of
   * the boundary can say which.
   *
   * Read and reset by the dump, so a caller can bracket one transition
   * between two of them -- the same way the composition statistics are read.
   */
  struct FenceCounters {
    uint64_t frames = 0;
    uint64_t already_due = 0;
    uint64_t not_yet_due = 0;
    uint64_t without_fence = 0;
    uint64_t could_not_ask = 0;

    /* The same question asked a frame later, which is what tells a fence that
     * is genuinely slow from a question that cannot be answered. If nothing is
     * ever due at hand-over and everything is due a frame later, the fence
     * simply needs longer than a flip interval and the client is right to
     * wait. If nothing is ever due at either moment, it is the asking that is
     * broken, and every number above it means nothing. */
    uint64_t due_a_frame_later = 0;
    uint64_t still_not_due = 0;
  };

  /* What the engine did over one dumpsys interval, so a dump around a
   * transition describes that transition. The engine's own accepted/refused
   * tallies (VicSession) run for its whole life instead -- whether it has
   * ever turned down a real set of layers is a question about the engine, not
   * about the last second, and it is what decides whether a fallback below
   * the merge is worth building at all. */
  struct MergeCounters {
    uint64_t frames = 0;
    uint64_t layers = 0;
    int64_t engine_ns = 0;
    int64_t engine_ns_max = 0;

    /* Frames on which the window was shown the buffer it was already
     * showing, because the group had not changed. The engine never woke. */
    uint64_t reused = 0;

    /* Why a frame was drawn rather than shown again, one count per drawn
     * frame. The breakdown is what decides the next step: an identity miss
     * is a content update no cleverness avoids, while a geometry miss is an
     * animation living inside the group -- the one case a smarter choice of
     * group could take out of the engine's hands. */
    uint64_t first_sight = 0;
    uint64_t changed_shape = 0; /* member count, window or stacking depth */
    uint64_t changed_identity = 0;
    uint64_t changed_geometry = 0; /* layout inside the group */
    uint64_t changed_size = 0;     /* the group resized -- honest redraw */
    uint64_t changed_blend = 0;
    uint64_t changed_transform = 0;
    uint64_t nameless = 0;

    /* The turning passes: how many ran, what they cost the engine, and how
     * many groups were refused because they asked for more turns than the
     * intermediates can hold at once. */
    uint64_t rotated = 0;
    int64_t rotate_ns = 0;
    int64_t rotate_ns_max = 0;
    uint64_t rotate_refused = 0;
  };
  FenceCounters fences_;
  MergeCounters merges_;

  /* The last thing the engine drew, kept to be shown again.
   *
   * A merged frame is a pure function of who its layers are and where they
   * go; while neither changes, the buffer already written is the frame, and
   * showing it again costs nothing. Compared against the previous frame
   * only, which is what makes it sound: pixels cannot change under an
   * identity, because drawing again means queueing, and queueing puts a
   * different buffer on the layer for at least a frame in between. That
   * argument holds only over an unbroken run of judged frames, which is
   * what ForgetMerge below protects.
   *
   * On a reuse the scratch pool is deliberately left alone -- no Next(), no
   * Presented() -- so it goes on believing, truly, that the same slot is on
   * screen.
   */
  struct MergedSource {
    uint64_t id;
    float source_left, source_top, source_right, source_bottom;
    int32_t display_left, display_top, display_right, display_bottom;
    bool premultiplied;
    float alpha;
    uint8_t transform;
  };
  std::vector<MergedSource> last_merge_sources_;
  int32_t last_merge_window_ = -1; /* -1: nothing remembered yet */
  uint32_t last_merge_depth_ = 0;
  uint32_t last_merge_width_ = 0;
  uint32_t last_merge_height_ = 0;
  hwc::DcHead::Window last_merge_described_{};
  SharedFd last_merge_fence_;

  /* Does this group name the frame already on the window? Counts the reason
   * whenever the answer is no. */
  bool RecognisesMerge(const TegraAtomicRequest::Merge &merge);
  void RememberMerge(const TegraAtomicRequest::Merge &merge,
                     const hwc::DcHead::Window &described,
                     const SharedFd &drawn);

  /* Nothing is remembered across a frame the group sat out.
   *
   * An identity is not a version. A buffer released to its owner can be
   * drawn into again and come back under the same identity with different
   * pixels; what rules that out is seeing the layer every frame, because
   * redrawing means queueing and a ring of two or more puts a different
   * buffer in front for at least the frame in between. A frame this path
   * did not judge -- the group dissolved, the engine refused, the panel was
   * only being powered -- is a frame nobody watched, so whatever was
   * remembered before it cannot be trusted after it. */
  void ForgetMerge() {
    last_merge_window_ = -1;
  }

  bool merge_cache_ = true;
  static bool MergeCacheFromProperty();

  /* Puts the frame's colour transform into the head's colour pipeline, or
   * puts the pipeline back when the transform is the identity. Called once
   * per executed frame; does nothing when the matrix repeats. */
  void ProgramColorMatrix(const HalColorTransformMatrix &matrix);

  /* What the framework's colour maths asked for, and what became of it.
   * Read-and-reset by the dump, like the merge counters beside it. */
  struct CmuCounters {
    /* Matrices written into the pipeline, and returns to the boot state.
     * Counted per change, not per frame: a night mode ramping counts once
     * per step, one holding steady counts nothing. */
    uint64_t applied = 0;
    uint64_t restored = 0;

    /* Transforms carrying an offset the inversion family did not claim --
     * nothing sends one today, and folding a bare offset into the regamma
     * would lift its floor, so the guard keeps them on the frame's own
     * path. */
    uint64_t skipped_offset = 0;

    /* Transforms honoured in a different shape than given: cross-channel
     * matrices applied in the pipeline's linear domain, inversions run as
     * the per-channel flip, offsets that differ between channels averaged.
     * Diagonal matrices are exact and are not counted here. */
    uint64_t approximated = 0;
  };
  CmuCounters cmu_;

  /* The matrix last handed in, to act only on changes: a night mode holding
   * steady repeats the same matrix every frame, and the steady state must
   * cost a comparison and nothing else. */
  HalColorTransformMatrix last_color_matrix_{};
  bool color_matrix_seen_ = false;

  /* Whether the pipeline currently differs from its boot state; false again
   * once restored. */
  bool csc_programmed_ = false;

  /* Whether this instance has written the pipeline at least once. Until it
   * has, "restore" cannot be skipped as a no-op: a predecessor's dying
   * state outlives it in the kernel, and only a write clears it. */
  bool boot_state_written_ = false;

  bool cmu_ctm_ = true;
  static bool CmuFromProperty();

  /* Whether the pipeline's resting state is the panel-to-sRGB correction
   * rather than the identity: the home the restore path writes, and the
   * inner factor composed under every framework matrix. */
  bool calibrated_home_ = false;

  /* Off unless asked for. The question costs one call into the kernel per
   * frame, and a frame is the thing being measured. */
  bool count_fences_ = false;

  /* The fence the previous flip returned. */
  SharedFd previous_post_fence_;

  /* When the last flip was posted. Diagnostic only, read by the trace. */
  int64_t last_flip_ns_ = 0;

  /* A copy of the fence given to the client, and the moment it was given.
   * Kept so that a later frame can ask how long it actually took to come due:
   * a fence handed out as this frame's and coming due two frames later is a
   * client standing still with nothing to blame. Diagnostic only. */
  SharedFd handed_out_fence_;
  int64_t handed_out_ns_ = 0;

  /* The last frames as they went up, one line each, for the dump.
   *
   * The flicker hunt's question is whether the seating breathes from frame
   * to frame -- which layer sits in the merge, which on a window, with what
   * alpha and at what scale -- and no answer to it survives the frame it
   * belongs to anywhere else: this composer's lines never reach the system
   * log on this platform, so the dump is the only channel. A ring, because
   * the defect is reproduced first and asked about after. */
  static constexpr size_t kFrameRing = 64;
  std::vector<std::string> frame_ring_;
  size_t frame_ring_pos_ = 0; /* where the next line goes, once full */
  void NoteFrame(const TegraAtomicRequest &tegra,
                 const std::vector<hwc::DcHead::Window> &windows,
                 bool merge_reused);
};

}  // namespace android::drm_hwcomposer
