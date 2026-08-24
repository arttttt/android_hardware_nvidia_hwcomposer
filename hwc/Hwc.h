/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "display/Device.h"
#include "display/PipelineToFrontendBinding.h"
#include "hwc/HwcDisplay.h"
#include "utils/properties.h"
#include "stats/DisplayRefreshRatesChangedAtomReporter.h"
#include "stats/Stats.h"

namespace android::drm_hwcomposer {

struct DisplayPipeline;

class Hwc : public PipelineToFrontendBindingInterface, public StatsProvider {
 public:
  Hwc();
  ~Hwc() override = default;

  // Enum for Display status: Connected, Disconnected, Link Training Failed
  enum DisplayStatus {
    kDisconnected,
    kConnected,
    kLinkTrainingFailed,
  };

  // Client Callback functions.:
  virtual void SendVsyncEventToClient(DisplayHandle display_handle,
                                      int64_t timestamp,
                                      uint32_t vsync_period) const = 0;
  virtual void SendVsyncPeriodTimingChangedEventToClient(
      DisplayHandle display_handle, int64_t timestamp) const = 0;
  virtual void SendRefreshEventToClient(DisplayHandle display_handle) = 0;
  virtual void SendHotplugEventToClient(DisplayHandle display_handle,
                                        enum DisplayStatus display_status) = 0;
  virtual void SendHdcpLevelsChangedEventToClient(
      DisplayHandle display_handle,
      std::optional<enum HdcpContentType> current_hdcp_level) = 0;

  // StatsProvider:
  auto PullCompositionStats()
      -> std::map<CompositionAttributes, CompositionStats> override;
  auto PullActiveDisplayCounts() -> ActiveDisplayCounts override;

  std::string DumpState();

  // Virtual Display functions.
  std::optional<DisplayHandle> CreateVirtualDisplay(uint32_t width,
                                                    uint32_t height);
  bool DestroyVirtualDisplay(DisplayHandle display_handle);
  uint32_t GetMaxVirtualDisplayCount();

  auto GetDisplay(DisplayHandle display_handle) {
    return displays_.count(display_handle) != 0
               ? displays_[display_handle].get()
               : nullptr;
  }

  /* Opens the display hardware and builds a display for everything on it.
   *
   * Upstream has a resource manager do this, watching the kernel and calling
   * back as cards and monitors appear. Nothing here appears or disappears, so
   * what is left of that is the part that was never about watching: build a
   * pipeline for each display the hardware has, hand it over, and say when
   * there are no more coming.
   *
   * Returns false if there is nothing to drive. A composer with no primary
   * display still answers the framework -- see the headless note below -- so
   * this failing is not the end of it.
   */
  bool Init();


  /* The one lock the framework's calls are serialised on.
   *
   * Upstream keeps it on the resource manager, because that is what a hotplug
   * arrives at and a hotplug must not land in the middle of a frame. There is
   * no hotplug here and no resource manager, but the lock is still what stops
   * the framework's several threads from reaching one display at once, so it
   * lives on the composer instead. */
  std::mutex &GetMainLock() {
    return main_lock_;
  }

  /* Whether a colour matrix the framework asks for may be quietly dropped
   * when the hardware cannot apply it, or must fall to the client. A setting,
   * read once. */
  CtmHandling GetCtmHandling() const {
    return ctm_handling_;
  }

  /* Whether anything that needs resizing goes to the client rather than to a
   * plane. Also a setting, and also not about resources -- it was on the
   * resource manager upstream only because that is where the settings were
   * read. */
  bool ForcedScalingWithGpu() const {
    return forced_scaling_with_gpu_;
  }

  /* The rest of what upstream's resource manager held: settings, read once
   * and kept. None of them is about resources; they sat there because that is
   * where the properties were read, and here that is the composer. */

  bool UseColorPipeline() const {
    return color_pipeline_enabled_;
  }

  bool PersistentHdrEnabled() const {
    return persistent_hdr_enabled_;
  }

  bool ExternalHdrEnabled() const {
    return external_hdr_enabled_;
  }

  int ForceColorMode() const {
    return force_color_mode_;
  }

  /* Displays named here are treated as built into the machine whatever they
   * say they are. */
  const std::set<std::string> &GetInternalDisplayNames();

  void ScheduleHotplugEvent(DisplayHandle display_handle,
                            enum DisplayStatus display_status) {
    hotplug_event_queue_.Add(display_handle, display_status);
  }

  void DeinitDisplays();

  // PipelineToFrontendBindingInterface
  bool BindDisplay(std::shared_ptr<DisplayPipeline> pipeline) override;
  bool UnbindDisplay(std::shared_ptr<DisplayPipeline> pipeline) override;
  void FinalizeDisplayBinding() override;
  void FlushHotplugEvents() override;

  // Notify Display Link Status
  void NotifyDisplayLinkStatus(
      std::shared_ptr<DisplayPipeline> pipeline) override;

  // Notify HDCP Termination from kernel Uevents
  void NotifyHdcpTermination(
      std::shared_ptr<DisplayPipeline> pipeline) override;

  // Should be done for all successful modesets (full and seamless).
  void LogRefreshRateChanges();

 protected:
  auto &Displays() {
    return displays_;
  }

 private:
  // An additional layer to isolate the critical region for invoking hotplug
  // event callbacks from the main mutex.
  class HotplugEventQueue {
   public:
    using Events = std::map<DisplayHandle, DisplayStatus>;

    void Add(DisplayHandle display_handle, DisplayStatus display_status);
    Events RetrieveAndFlush();

   private:
    Events events_;
    std::mutex mutex_;
  };

  std::unique_ptr<Device> device_;

  std::mutex main_lock_;
  const CtmHandling ctm_handling_ = Properties::GetCtmHandling();
  const bool forced_scaling_with_gpu_ = Properties::ScaleWithGpu();
  const bool color_pipeline_enabled_ = Properties::UseColorPipeline();
  const bool persistent_hdr_enabled_ = Properties::PersistentHdrEnabled();
  const bool external_hdr_enabled_ = Properties::ExternalHdrEnabled();
  const int force_color_mode_ = Properties::ForceColorMode();

  std::set<std::string> internal_display_names_;
  bool internal_display_names_read_ = false;
  std::map<DisplayHandle, std::unique_ptr<HwcDisplay>> displays_;
  std::map<std::shared_ptr<DisplayPipeline>, DisplayHandle> display_handles_;

  HotplugEventQueue hotplug_event_queue_;
  std::vector<DisplayHandle> displays_for_removal_list_;

  void RequestHdcpNegotiation(DisplayHandle display_handle);

  DisplayHandle last_display_handle_ = kPrimaryDisplay;
  StatsTracker dump_stats_tracker_;

  std::unique_ptr<DisplayRefreshRatesChangedAtomReporter>
      refresh_rates_reporter_;

  bool hdcp_on_hotplug_enabled_{};
};

}  // namespace android::drm_hwcomposer
