/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ui/ColorSpace.h>
#include <ui/GraphicTypes.h>

#include "compositor/CompositionPlanner.h"
#include "compositor/DisplayInfo.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/PlanInvalidation.h"
#include "display/DisplayPipeline.h"
#include "drm/drm_mode.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/BacklightController.h"
#include "utils/EdidWrapper.h"
#include "utils/fd.h"

/* Upstream declares the high-dynamic-range list through a package this
 * platform predates and aliases it into ui:: itself. Here <ui/GraphicTypes.h>
 * above already provides ui::Hdr, so there is nothing to alias. */

namespace android::drm_hwcomposer {

template <typename T>
class CommitStatusOr;
class DisplayConfigurationResultReporter;
class DisplayHotplugConnectModeDetectedAtomReporter;
class Hwc;
class FlatteningController;
class HdcpController;
class VSyncWorker;

struct AtomicCommitArgs;
struct AtomicCommitResult;
struct CommitStatus;
struct CompositionAttributes;
struct CompositionStats;
struct DisplayPipeline;

using EdidWrapperUnique = std::unique_ptr<EdidWrapper>;
using ColorGamut = ::android::ColorSpace;

class FrontendDisplayBase {
 public:
  virtual ~FrontendDisplayBase() = default;
};

inline constexpr uint32_t kPrimaryDisplay = 0;
inline constexpr float kBrightnessUnset = -1;

inline constexpr ui::RenderIntent
    kVendorBoostedRenderIntent = static_cast<ui::RenderIntent>(256);

// NOLINTNEXTLINE
class HwcDisplay : public ICompositorDisplay {
 public:
  enum class Error {
    kNone,
    kBadParameter,
    kUnsupported,
  };

  enum ConfigError {
    kNone,
    kBadConfig,
    kSeamlessNotAllowed,
    kSeamlessNotPossible,
    kConfigFailed
  };

  enum DisplayType { kInternal, kExternal, kVirtual };

  using PowerMode = android::drm_hwcomposer::PowerMode;

  HwcDisplay(DisplayHandle handle, bool is_virtual, Hwc *hwc);
  HwcDisplay(const HwcDisplay &) = delete;
  ~HwcDisplay() override;


  void SetColorTransformMatrix(
      const HalColorTransformMatrix &color_transform_matrix);

  bool CursorPlaneNeedsColorPipeline(
      const HwcLayer &cursor_layer) const override;

  /* SetPipeline should be carefully used only by HwcTwo hotplug handlers */
  void SetPipeline(std::shared_ptr<DisplayPipeline> pipeline);

  CommitStatus TestComposition(
      CompositionPlanner::ValidatedComposition &composition) const override;

  auto GetReusablePlan() const
      -> const std::optional<CompositionPlanner::ValidatedComposition>
          & override {
    return reusable_plan_;
  }

  uint32_t TakePlanInvalidators() const override {
    uint32_t taken = plan_invalidators_;
    plan_invalidators_ = 0;
    for (const auto &[id, layer] : layers_) {
      taken |= layer.TakePlanInvalidators();
      /* Liveness is re-judged exactly where the plan's other inputs are
       * gathered, so a crossing and the replan it demands are one
       * event. A scene that goes fully quiet raises this at the same
       * moment flattening starts wanting it -- and loses to it, since
       * the flatten branch answers before these bits are asked for. */
      if (layer.RefreshLiveness()) {
        taken |= kLayerActivity;
      }
    }
    return taken;
  }

  std::vector<const HwcLayer *> GetOrderLayersByZPos() const override;

  /* The group selector's half of the dump: how plans were laid out and
   * which layers count as drawing right now -- the oscillation of that
   * answer is the thing to watch in the field. */
  auto DumpGroupSelector() const -> std::string;

  /* The colour bridge's half of the dump: what the framework asked of
   * the colour surface and what it last set. A colour switch that never
   * shows on the panel broke in one of two places -- above this surface
   * or below it -- and these counters say which. */
  auto DumpColorBridge() const -> std::string;


  auto GetDisplayName() const -> std::string;

  auto GetDisplayConfigs() const -> std::vector<HwcDisplayConfig>;

  // Get the config representing the mode that has been committed to KMS.
  auto GetCurrentConfig() const -> const HwcDisplayConfig *;

  // Get the config that was last requested through SetActiveConfig and similar
  // functions. This may differ from the GetCurrentConfig if the config change
  // is queued up to take effect in the future.
  auto GetLastRequestedConfig() const -> const HwcDisplayConfig *;

  // Get the config that will be active during the next commit. If a config
  // change has been staged, it will be returned iff the scheduled time has
  // arrived. Otherwise the current config will be returned.
  const HwcDisplayConfig *GetNextConfig() const;

  // Set a config synchronously. If the requested config fails to be committed,
  // this will return with an error. Otherwise, the config will have been
  // committed to the kernel on successful return.
  ConfigError SetConfig(ConfigId config);

  // Queues a configuration change to take effect in the future. All queued
  // configurations are seamless.
  auto QueueConfig(ConfigId config, int64_t desired_time,
                   QueuedConfigTiming *out_timing) -> ConfigError;

  // Get the HwcDisplayConfig, or nullptr if none.
  auto GetConfig(ConfigId config_id) const -> const HwcDisplayConfig *;

  auto GetDisplayBoundsMm() const -> std::pair<int32_t, int32_t>;

  // To be called after SetDisplayProperties. Returns an empty vector if the
  // requested layers have been validated, otherwise the vector describes
  // the requested composition type changes.
  using ChangedLayer = std::pair<ILayerId, CompositionType>;

  struct ValidateResult {
    // Layers whose composition type was changed my the HWC.
    std::vector<ChangedLayer> changed_layers;
    // Request the client to write transparent pixels where these layers would
    // be.
    std::vector<ILayerId> punch_out_layers;
  };
  auto ValidateStagedComposition() -> ValidateResult;

  // Mark previously validated properties as ready to present.
  auto AcceptValidatedComposition() -> void;

  using ReleaseFence = std::pair<ILayerId, SharedFd>;
  // Present previously staged properties, and return fences to indicate when
  // the new content has been presented, and when the previous buffers have
  // been released. If |desired_present_time| is set, ensure that the
  // composition is presented at the closest vsync to that requested time.
  // Otherwise, present immediately.
  auto PresentStagedComposition(std::optional<int64_t> desired_present_time,
                                SharedFd &out_present_fence,
                                std::vector<ReleaseFence> &out_release_fences)
      -> bool;

  // Get the edid bytes for this display. Return an empty vector on error.
  auto GetRawEdid() const -> std::vector<uint8_t>;

  // Get the port id that this display is plugged into.
  auto GetPort() const -> uint8_t;

  auto HasBacklight() const -> bool {
    return backlight_controller_ != nullptr;
  }

  auto SetBrightness(float brightness) -> bool;

  auto SetContentType(ContentType content_type) {
    content_type_ = content_type;
  }

  // Physical displays are either internal or external.
  auto GetDisplayType() const -> DisplayType;

  // Enable or disable vsync callbacks.
  void SetVsyncCallbacksEnabled(bool enabled);

  bool GetDisplayEnabled() const;

  auto GetFrontendPrivateData() -> std::shared_ptr<FrontendDisplayBase> {
    return frontend_private_data_;
  }

  auto SetFrontendPrivateData(std::shared_ptr<FrontendDisplayBase> data) {
    frontend_private_data_ = std::move(data);
  }

  auto CreateLayer(ILayerId new_layer_id) -> bool;
  auto DestroyLayer(ILayerId layer_id) -> bool;

  auto GetColorModes() const -> std::vector<ColorMode>;
  auto GetRenderIntents(ColorMode color_mode) const
      -> std::vector<ui::RenderIntent>;
  void SetColorMode(ColorMode color_mode, ui::RenderIntent render_intent);

  void GetHdrCapabilities(std::vector<ui::Hdr> *types, float *max_luminance,
                          float *max_average_luminance,
                          float *min_luminance) const;

  auto IsHdcpPropertyPresent() -> bool;
  auto StartHdcp() -> bool;
  auto StopHdcp() -> bool;


  HwcLayer *get_layer(ILayerId layer) {
    auto it = layers_.find(layer);
    if (it == layers_.end())
      return nullptr;
    return &it->second;
  }


  const auto &GetPipe() const {
    return *pipeline_;
  }

  auto &GetPipe() {
    return *pipeline_;
  }

  size_t GetNumAvailablePlanes() const override;
  size_t GetNumDirectPlanes() const override;
  std::shared_ptr<BindingOwner<Plane>> GetCursorPlane() const override;

  // Whether the GPU should be responsible for the client CTM. (GPU is never
  // responsible for render intent CTM).
  bool CtmByGpu() const override;

  bool ForcedScalingWithGpu() const override;

  bool UseColorPipeline() const override {
    return use_color_pipeline_;
  };

  const std::map<CompositionAttributes, CompositionStats> &comp_stats() const {
    return comp_stats_;
  }

  /* Headless mode required to keep SurfaceFlinger alive when all display are
   * disconnected, Without headless mode Android will continuously crash.
   * Only single internal (primary) display is required to be in HEADLESS mode
   * to prevent the crash. See:
   * https://source.android.com/devices/graphics/hotplug#handling-common-scenarios
   */
  bool IsInHeadlessMode() const {
    return !pipeline_;
  }

  void Deinit();


  auto GetClientLayer() -> HwcLayer & {
    return client_layer_;
  }

  const HwcLayer &GetClientLayer() const override {
    return client_layer_;
  }

  auto &GetWritebackLayer() {
    return writeback_layer_;
  }

  void SetVirtualDisplayResolution(uint16_t width, uint16_t height) {
    virtual_disp_width_ = width;
    virtual_disp_height_ = height;
  }


  std::pair<uint32_t, uint32_t> GetSize() const override;

  // Enable or disable the display.
  HwcDisplay::Error SetPowerMode(PowerMode mode);

 private:
  bool IsDozeSupported() const;
  bool IsDozeSuspendSupported() const;
  bool IsSuspendSupported() const;

  void InitUseColorPipeline();
  void InitWcgSupported();
  void InitHdrSupported();
  void InitForcedColorMode();

  // Before CreateFrameUpdateCommit() can be called, it must be ensured that
  // the composition's internal states are up to date and ready to create an
  // AtomicCommitArgs.
  void PrepareCompositionForCommit(
      CompositionPlanner::ValidatedComposition &composition) const;

  // Create AtomicCommitArgs to commit at the next vsync. Returns nullopt if
  // such AtomicCommitArgs cannot be created due to lack of drm resources or
  // invalid HwcDisplay or HwcLayer state.
  // The caller must do a test commit on the returned args to ensure that the
  // hardware can perform the commit.
  // PrepareCompositionForCommit() must be called before this function to
  // ensure that the composition's internal states are up to date.
  std::optional<AtomicCommitArgs> CreateFrameUpdateCommit(
      const CompositionPlanner::ValidatedComposition &composition) const;

  // Creates a LayerToPlaneJoiningPlan for the given composition type map.
  std::unique_ptr<LayerToPlaneJoiningPlan> CreateLayerToPlaneJoiningPlan(
      const CompositionPlanner::CompositionTypeMap &composition_types) const;

  CommitStatus CommitStagedComposition(SharedFd &out_present_fence);

  // Update HwcDisplay state tracking to reflect what was committed in |a_args|.
  // This should be called after a successful commit.
  void ApplyCommitChanges(const AtomicCommitArgs &a_args,
                          const AtomicCommitResult &result);

  AtomicCommitArgs CreateModesetCommit(
      const HwcDisplayConfig *config,
      const std::optional<LayerData> &modeset_layer);

  CommitStatusOr<AtomicCommitResult> ExecuteAtomicCommit(
      AtomicCommitArgs &a_args) const;

  // Sleep the current thread until |present_time| is closest to the next
  // expected vsync time.
  void WaitForPresentTime(int64_t present_time, uint32_t vsync_period_ns);

  uint32_t GetCurrentVsyncPeriodNs() const;

  // Returns a client's layer if one was already provided and its size matches
  // the new config, otherwise allocates a new one.
  std::optional<LayerData> GetModesetLayerData(
      const HwcDisplayConfig *new_config);

  // Seamless-tests all configs against the active config for future seamless
  // transitions and update the config groups.
  void SetConfigGroupsForActiveConfig();

  void UpdateColorTransformMatrix();

  bool Init();

  void SetHdrHeadroom();
  void SetHdrOutputMetadata(const ColorGamut &color_gamut,
                            TransferFunction transfer_function);
  void SetOutputType(OutputType hdr_output_type);

  auto GetEdid() const -> const EdidWrapperUnique & {
    return edid_wrapper_;
  }

  void LogModesOnHotplug();
  void LogConfigResult(const AtomicCommitArgs &args, bool success,
                       int64_t duration_ns) const;

  HwcDisplayConfigsGenerator configs_generator_;
  HwcDisplayConfigs configs_;
  ConfigId active_config_id_ = 0;

  Hwc *const hwc_;

  EdidWrapperUnique edid_wrapper_ = std::make_unique<EdidWrapper>();

  int64_t staged_mode_change_time_{};
  std::optional<ConfigId> staged_mode_config_id_{};

  std::shared_ptr<DisplayPipeline> pipeline_;

  std::unique_ptr<FlatteningController> flatcon_;
  std::unique_ptr<HdcpController> hdcpcon_;

  std::unique_ptr<VSyncWorker> vsync_worker_;
  bool vsync_event_en_{};

  const DisplayHandle handle_;
  bool is_virtual_;

  std::map<ILayerId, HwcLayer> layers_;
  HwcLayer client_layer_;
  std::unique_ptr<HwcLayer> writeback_layer_;
  uint16_t virtual_disp_width_{};
  uint16_t virtual_disp_height_{};
  std::shared_ptr<const HalColorTransformMatrix>
      color_matrix_ = GetIdentityCtmPtr();
  std::shared_ptr<const HalColorTransformMatrix>
      client_color_matrix_ = GetIdentityCtmPtr();
  // ASSERTION: render_intent_matrix_ must never have offset.
  std::shared_ptr<const HalColorTransformMatrix>
      render_intent_matrix_ = GetIdentityCtmPtr();
  /* Colour-bridge telemetry; the query counter is mutable because the
   * query itself is const. */
  mutable uint32_t render_intent_queries_ = 0;
  uint32_t color_mode_sets_ = 0;
  int32_t last_color_mode_ = -1;
  int32_t last_render_intent_ = -1;
  bool client_ctm_has_offset_ = false;
  bool client_ctm_has_negative_ = false;
  ContentType content_type_ = ContentType::kNoData;
  HwcColorspace colorspace_{};
  TransferFunction transfer_func_{};
  int32_t min_bpc_{};
  std::shared_ptr<hdr_output_metadata> hdr_metadata_;
  float brightness_ = kBrightnessUnset;
  float hdr_headroom_{};

  bool has_wcg_support_ = false;
  bool has_hdr_support_ = false;
  bool use_color_pipeline_ = false;
  std::optional<ColorMode> forced_color_mode_;

  // Most recent result of ValidateStagedComposition. Must be kept alive until
  // the composition is committed.
  std::optional<CompositionPlanner::ValidatedComposition>
      validated_composition_ = std::nullopt;


  uint32_t frame_no_ = 0;
  std::map<CompositionAttributes, CompositionStats> comp_stats_{};

  std::shared_ptr<FrontendDisplayBase> frontend_private_data_;

  std::unique_ptr<DisplayHotplugConnectModeDetectedAtomReporter>
      display_mode_reporter_;
  std::unique_ptr<DisplayConfigurationResultReporter> config_result_reporter_;

  std::unique_ptr<BacklightController> backlight_controller_;

  /* The composition the display last actually showed, minus its plan
   * reference -- holding that would keep the planes reserved, and the
   * commit rebuilds the joining from the types anyway. What a clean-flags
   * frame is handed. Empty until a frame has been shown, and emptied by
   * every path that makes the shown frame unrepresentative. */
  std::optional<CompositionPlanner::ValidatedComposition> reusable_plan_;

  /* How the joining plans were laid out, one counter per steering
   * outcome, over the lifetime and since the last dump -- the same two
   * windows every neighbouring section answers for. Mutable for the
   * same reason the invalidator bits are: counted from a const path. */
  static constexpr size_t kSteeringOutcomes = 7;
  mutable uint64_t steering_outcomes_[kSteeringOutcomes] = {};
  mutable uint64_t steering_interval_[kSteeringOutcomes] = {};

  /* Whether the staged composition was handed back from the kept plan
   * rather than planned anew -- in which case writing it back at present
   * would be a copy onto itself. */
  bool staged_composition_reused_ = false;

  /* Display-level PlanInvalidator bits; the layers carry their own. Starts
   * all-dirty so the first frame of a life is planned in full, and every
   * failure path re-raises it. Mutable for the same reason the layer's is:
   * consumed through a const interface. */
  mutable uint32_t plan_invalidators_ = 0xFFFFFFFF;

  void MarkPlanInvalid(uint32_t invalidator_bits) {
    plan_invalidators_ |= invalidator_bits;
  }

  /* Which buffers a frame handed to an engine instead of to the display, and
   * when that engine finished reading them.
   *
   * Two frames' worth, because a release fence is not about the buffer a layer
   * has now -- it is about the one the layer has just given up, which was read
   * while the frame before this one was being built. So the frame just
   * committed fills the first of these, and the answers handed out this time
   * come from the second.
   */
  struct EngineReads {
    SharedFd fence;
    std::vector<buffer_handle_t> buffers;
  };

  /* The fence saying when `prior` stopped being read, or nothing if the
   * display was the one reading it and the frame's own fence is the answer. */
  auto EngineFenceFor(buffer_handle_t prior) const -> std::optional<SharedFd>;

  EngineReads engine_this_frame_;
  EngineReads engine_last_frame_;

  /* When the buffers this frame replaced come free -- always the flip just
   * posted, never the one before it. Separate from the fence handed to the
   * client as the present fence, which is allowed to name an older frame so
   * that the client does not treat a fence cutting it fine as a frame it
   * missed. See AtomicCommitResult. */
  SharedFd release_fence_;
};

}  // namespace android::drm_hwcomposer
