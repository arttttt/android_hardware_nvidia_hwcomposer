/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "Hwc.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "backend/Backend.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "display/Connector.h"
#include "display/Device.h"
#include "display/DisplayPipeline.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "stats/DisplayRefreshRatesChangedAtomReporter.h"
#include "stats/Stats.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {
// Helper functions for implementing dumpsys support.
std::string DumpStats(const CompositionStats &stats) {
  if (stats.total_pixops == 0)
    return "No stats yet";

  // NOLINTNEXTLINE(readability-magic-numbers)
  auto ratio = 1.0 - (double(stats.gpu_pixops) / double(stats.total_pixops));

  std::stringstream ss;
  ss << " Total frames count: " << stats.total_frames << "\n"
     << " Failed cursor test commit frames: "
     << stats.failed_kms_cursor_validate << "\n"
     << " Failed to test commit frames: " << stats.failed_kms_validate << "\n"
     << " Failed to commit frames: " << stats.failed_kms_present << "\n"
     << ((stats.failed_kms_present > 0)
             ? " !!! Internal failure, FIX it please\n"
             : "")
     << " Flattened frames: " << stats.frames_flattened << "\n"
     << " Cursor plane frames: " << stats.cursor_plane_frames << "\n"
     << " Pixel operations (free units) : [TOTAL: " << stats.total_pixops
     << " / GPU: " << stats.gpu_pixops << "]\n"
     << " Composition efficiency: " << ratio;
  return ss.str();
}

std::string DumpDisplayStats(const HwcDisplay *display,
                             const CompositionStats &stats,
                             const CompositionStats &delta) {
  std::stringstream ss;
  ss << "- Display on: " << display->GetDisplayName() << "\n"
     << "Statistics since system boot:\n"
     << DumpStats(stats) << "\n\n"
     << "Statistics since last dumpsys request:\n"
     << DumpStats(delta) << "\n\n";
  return ss.str();
}
}  // namespace

Hwc::Hwc()
    : dump_stats_tracker_(this),
      refresh_rates_reporter_(DisplayRefreshRatesChangedAtomReporter::Create()),
      hdcp_on_hotplug_enabled_(Properties::EnableHdcpOnHotplug()) {
}

const std::set<std::string> &Hwc::GetInternalDisplayNames() {
  if (!internal_display_names_read_) {
    internal_display_names_read_ = true;

    /* One property, comma-separated. Upstream splits it with a helper this
     * platform's base library does not have yet. */
    const std::string names = Properties::InternalDisplayNames();
    for (size_t start = 0; start <= names.size();) {
      const size_t comma = names.find(',', start);
      const size_t end = comma == std::string::npos ? names.size() : comma;

      if (end > start)
        internal_display_names_.insert(names.substr(start, end - start));

      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
  }

  return internal_display_names_;
}

bool Hwc::Init() {
  /* Let go before taking hold again, and let go of all of it.
   *
   * Which windows a display head has is decided per open file: the driver
   * hands them to whoever asks first and refuses everyone after. Building the
   * new device before releasing the old one means asking a head that is still
   * owned, and it answers by giving nothing -- so the displays come up with
   * no planes to put anything on.
   *
   * Taking a display apart is not enough to release it. A pipeline is held
   * twice: by the display it drives and by the record of which display it was
   * given to. Only unbinding drops both, which is why this is written as an
   * unbind of everything rather than a deinitialisation. */
  std::vector<std::shared_ptr<DisplayPipeline>> bound;
  bound.reserve(display_handles_.size());
  for (const auto &[pipeline, _] : display_handles_)
    bound.push_back(pipeline);

  for (auto &pipeline : bound)
    UnbindDisplay(pipeline);

  /* Retires the displays that unbinding put up for removal. */
  FinalizeDisplayBinding();

  bound.clear();
  device_.reset();

  device_ = CreateDevice();
  if (!device_) {
    ALOGE("No display hardware to drive");
    return false;
  }

  /* Before any display is built, because building one reads buffers.
   *
   * Upstream does this in the resource manager for the same reason, and it is
   * easy to miss: nothing complains if it is skipped, every buffer simply
   * fails to be described and the screen stays black. */
  if (BufferInfoGetter::GetInstance() == nullptr) {
    auto getter = device_->GetBackend().CreateBufferInfoGetter();
    if (!getter) {
      ALOGE("Backend has no way to read a buffer");
      return false;
    }
    BufferInfoGetter::Init(std::move(getter));
  }

  /* What upstream's resource manager does once it has finished looking. The
   * looking is what is missing here, not this. */
  for (const auto &connector : device_->GetConnectors()) {
    auto pipeline = device_->GetBackend().CreatePipeline(*connector);
    if (!pipeline) {
      ALOGE("Failed to build a display for '%s'",
            connector->GetName().c_str());
      continue;
    }

    pipeline->device = device_.get();

    if (!BindDisplay(std::shared_ptr<DisplayPipeline>(std::move(pipeline))))
      ALOGE("Failed to attach '%s'", connector->GetName().c_str());
  }

  /* Called whether or not anything was attached: it is also what puts the
   * composer into headless mode, which is what keeps the framework alive when
   * there is no display at all. */
  FinalizeDisplayBinding();

  /* And then the events are handed over, which is the part that makes any of
   * it visible. Attaching a display only queues the news of it; until this
   * runs the framework has been told nothing, and a framework that registered
   * for hotplug and never heard about a primary display gives up. */
  FlushHotplugEvents();

  return true;
}

/* Must be called after every display attach/detach cycle */
void Hwc::FinalizeDisplayBinding() {
  if (displays_.count(kPrimaryDisplay) == 0) {
    /* Primary display MUST always exist */
    ALOGI("No pipelines available. Creating null-display for headless mode");
    displays_[kPrimaryDisplay] = std::make_unique<
        HwcDisplay>(kPrimaryDisplay, /* is_virtual */ false, this);
    /* Initializes null-display */
    displays_[kPrimaryDisplay]->SetPipeline({});
  }

  if (displays_[kPrimaryDisplay]->IsInHeadlessMode() &&
      !display_handles_.empty()) {
    /* Reattach first secondary display to take place of the primary */
    auto pipe = display_handles_.begin()->first;
    ALOGI("Primary display was disconnected, reattaching '%s' as new primary",
          pipe->connector->Get()->GetName().c_str());
    UnbindDisplay(pipe);
    BindDisplay(pipe);
  }

  for (auto handle : displays_for_removal_list_) {
    displays_.erase(handle);
  }
  displays_for_removal_list_.clear();
}

void Hwc::FlushHotplugEvents() {
  auto events = hotplug_event_queue_.RetrieveAndFlush();
  for (const auto &[handle, status] : events) {
    SendHotplugEventToClient(handle, status);
  }
}

bool Hwc::BindDisplay(std::shared_ptr<DisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) != 0) {
    ALOGE("%s, pipeline is already used by another display, FIXME!!!: %p",
          __func__, pipeline.get());
    return false;
  }

  uint32_t disp_handle = kPrimaryDisplay;

  if (displays_.count(kPrimaryDisplay) != 0 &&
      !displays_[kPrimaryDisplay]->IsInHeadlessMode()) {
    disp_handle = ++last_display_handle_;
  }

  if (displays_.count(disp_handle) == 0) {
    auto disp = std::make_unique<HwcDisplay>(disp_handle,
                                             /* is_virtual */ false, this);
    displays_[disp_handle] = std::move(disp);
  }

  ALOGI("Attaching pipeline '%s' to the display #%d%s",
        pipeline->connector->Get()->GetName().c_str(), (int)disp_handle,
        disp_handle == kPrimaryDisplay ? " (Primary)" : "");

  displays_[disp_handle]->SetPipeline(pipeline);
  display_handles_[pipeline] = disp_handle;

  RequestHdcpNegotiation(disp_handle);

  return true;
}

bool Hwc::UnbindDisplay(std::shared_ptr<DisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) == 0) {
    ALOGE("%s, can't find the display, pipeline: %p", __func__, pipeline.get());
    return false;
  }
  auto handle = display_handles_[pipeline];
  display_handles_.erase(pipeline);

  ALOGI("Detaching the pipeline '%s' from the display #%i%s",
        pipeline->connector->Get()->GetName().c_str(), (int)handle,
        handle == kPrimaryDisplay ? " (Primary)" : "");

  if (displays_.count(handle) == 0) {
    ALOGE("%s, can't find the display, handle: %" PRIu64, __func__, handle);
    return false;
  }
  displays_[handle]->SetPipeline({});

  /* Defer destruction of the display until after the hotplug event is sent. */
  if (handle != kPrimaryDisplay) {
    displays_for_removal_list_.emplace_back(handle);
  }
  return true;
}

void Hwc::NotifyDisplayLinkStatus(
    std::shared_ptr<DisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) == 0) {
    ALOGE("%s, can't find the display, pipeline: %p", __func__, pipeline.get());
    return;
  }
  ScheduleHotplugEvent(display_handles_[pipeline],
                       DisplayStatus::kLinkTrainingFailed);
}

void Hwc::NotifyHdcpTermination(
    std::shared_ptr<DisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) == 0) {
    ALOGE("%s, can't find the display, pipeline: %p", __func__, pipeline.get());
    return;
  }

  auto handle = display_handles_[pipeline];
  auto *display = GetDisplay(handle);
  if (display == nullptr) {
    ALOGE("%s, display is null for handle: %" PRIu64, __func__, handle);
    return;
  }

  // Trigger HwcDisplay to terminate HDCP negotiation only if it was previously
  // enabled.
  if (!display->StopHdcp()) {
    ALOGI("%s, StopHdcp() failed for display: %" PRIu64, __func__, handle);
  }
}

void Hwc::RequestHdcpNegotiation(DisplayHandle display_handle) {
  auto *display = GetDisplay(display_handle);
  if (display == nullptr) {
    ALOGE("%s, display is null for handle: %" PRIu64, __func__, display_handle);
    return;
  }

  const auto &pipeline = display->GetPipe();
  if (!hdcp_on_hotplug_enabled_) {
    return;
  }

  if (pipeline.connector && pipeline.connector->Get() != nullptr &&
      (pipeline.connector->Get()->IsInternal() ||
       pipeline.connector->Get()->IsMst())) {
    ALOGI(
        "%s, skipping default HDCP enabling for internal or MST display "
        "handle: "
        "%" PRIu64,
        __func__, display_handle);
    return;
  }

  if (!display->StartHdcp()) {
    ALOGI(
        "%s, StartHdcp() requested by default not supported for display: "
        "%" PRIu64,
        __func__, display_handle);
  }
}

std::optional<DisplayHandle> Hwc::CreateVirtualDisplay(uint32_t width,
                                                          uint32_t height) {
  ALOGI("Creating virtual display %dx%d", width, height);

  /* Nothing to write a virtual display into. Upstream builds one out of a
   * writeback connector -- a display the controller feeds into memory rather
   * than out to a panel -- and this controller has no such thing. */
  std::shared_ptr<DisplayPipeline> virtual_pipeline;
  if (!virtual_pipeline)
    return std::nullopt;

  DisplayHandle new_display_handle = ++last_display_handle_;
  auto disp = std::make_unique<HwcDisplay>(new_display_handle,
                                           /* is_virtual */ true, this);

  disp->SetVirtualDisplayResolution(width, height);
  disp->SetPipeline(virtual_pipeline);
  displays_[new_display_handle] = std::move(disp);
  return new_display_handle;
}

bool Hwc::DestroyVirtualDisplay(DisplayHandle display) {
  ALOGI("Destroying virtual display %" PRIu64, display);

  if (displays_.count(display) == 0) {
    ALOGE("Trying to destroy non-existent display %" PRIu64, display);
    return false;
  }

  if (displays_[display]->GetDisplayType() !=
      HwcDisplay::DisplayType::kVirtual) {
    ALOGE("Trying to destroy non-virtual display %" PRIu64, display);
    return false;
  }

  displays_[display]->SetPipeline({});
  displays_.erase(display);
  return true;
}

auto Hwc::PullCompositionStats()
    -> std::map<CompositionAttributes, CompositionStats> {
  std::map<CompositionAttributes, CompositionStats> stats;
  for (const auto &[display_handle, display] : displays_) {
    stats.insert(display->comp_stats().begin(), display->comp_stats().end());
  }
  return stats;
}

auto Hwc::PullActiveDisplayCounts() -> ActiveDisplayCounts {
  ActiveDisplayCounts counts;
  for (const auto &[_, display] : displays_) {
    if (!display->GetDisplayEnabled()) {
      continue;
    }

    const HwcDisplay::DisplayType display_type = display->GetDisplayType();
    if (display_type == HwcDisplay::DisplayType::kVirtual) {
      counts.num_virtual_displays++;
    } else {
      counts.num_active_physical_displays++;
      if (display_type == HwcDisplay::DisplayType::kExternal) {
        counts.num_active_external_displays++;
      }
    }
  }
  return counts;
}

std::string Hwc::DumpState() {
  std::stringstream output;

  output << "-- drm_hwcomposer --\n\n";

  std::map<DisplayHandle, std::pair<CompositionStats, CompositionStats>>
      total_stats;
  const auto callback = [&total_stats](const CompositionAttributes &attributes,
                                       const CompositionStats &cumulative,
                                       const CompositionStats &delta) {
    auto it = total_stats.find(attributes.display_handle);
    if (it == total_stats.end()) {
      total_stats.emplace(attributes.display_handle,
                          std::make_pair(CompositionStats{},
                                         CompositionStats{}));
      it = total_stats.find(attributes.display_handle);
    }
    auto &[total_cumulative, total_delta] = it->second;
    total_cumulative += cumulative;
    total_delta += delta;
  };
  dump_stats_tracker_.ReportCompositionStats(callback);

  for (const auto &[display_handle, display_stats] : total_stats) {
    const auto *display = GetDisplay(display_handle);
    ALOGE_IF(display == nullptr, "Display %" PRIu64 " not found",
             display_handle);
    if (display != nullptr) {
      output << DumpDisplayStats(display, display_stats.first,
                                 display_stats.second);
    }
  }
  return output.str();
}

uint32_t Hwc::GetMaxVirtualDisplayCount() {
  /* Virtual display is an experimental feature.
   * Unless explicitly set to true, return 0 for no support.
   */
  if (!Properties::EnableVirtualDisplay()) {
    return 0;
  }

  /* See CreateVirtualDisplay: no writeback, so none can be made. Kept as an
   * arithmetic rather than a plain zero, because the property above is what
   * decides whether any of this is asked for. */
  uint32_t writeback_count = 0;
  writeback_count = std::min(writeback_count, 1U);
  /* Currently, only 1 virtual display is supported. Other cases need testing */
  ALOGI("Max virtual display count: %d", writeback_count);
  return writeback_count;
}

void Hwc::DeinitDisplays() {
  for (auto &pair : Displays()) {
    pair.second->SetPipeline(nullptr);
  }
}

void Hwc::LogRefreshRateChanges() {
  std::vector<int32_t> refresh_rates;
  refresh_rates.reserve(displays_.size());
  for (const auto &[_, display] : displays_) {
    if (const HwcDisplayConfig *config = display->GetCurrentConfig(); config) {
      refresh_rates.push_back(
          static_cast<int32_t>(lround(config->mode.GetVRefresh())));
    }
  }

  if (refresh_rates_reporter_)
    refresh_rates_reporter_->UpdateRefreshRates(refresh_rates);
}

void Hwc::HotplugEventQueue::Add(DisplayHandle display_handle,
                                    DisplayStatus display_status) {
  std::scoped_lock lock(mutex_);
  events_[display_handle] = display_status;
}

Hwc::HotplugEventQueue::Events Hwc::HotplugEventQueue::RetrieveAndFlush() {
  Events events;
  {
    std::scoped_lock lock(mutex_);
    std::swap(events, events_);
  }
  return events;
}

}  // namespace android::drm_hwcomposer
