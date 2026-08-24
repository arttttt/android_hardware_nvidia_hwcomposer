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

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
// #define LOG_NDEBUG 0 // Uncomment to see HWC2 API calls in logcat

#define LOG_TAG "drmhwc"

#include <cinttypes>
#include <memory>
#include <optional>

#include <cutils/native_handle.h>
#include <hardware/hwcomposer2.h>
#include <system/graphics-base-v1.1.h>
#include <ui/GraphicTypes.h>

#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/DisplayInfo.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcLayer.h"
#include "display/FbImporter.h"
#include "hwc2_device/DrmHwcTwo.h"
#include "utils/FrameworkTraits.h"
#include "utils/GraphicsCompat.h"
#include "utils/Time.h"
#include "utils/Logging.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

static int32_t ConfigErrorToHWC2(HwcDisplay::ConfigError result) {
  switch (result) {
    case HwcDisplay::ConfigError::kBadConfig:
      return static_cast<int32_t>(HWC2::Error::BadConfig);
    case HwcDisplay::ConfigError::kSeamlessNotAllowed:
      return static_cast<int32_t>(HWC2::Error::SeamlessNotAllowed);
    case HwcDisplay::ConfigError::kSeamlessNotPossible:
      return static_cast<int32_t>(HWC2::Error::SeamlessNotPossible);
    case HwcDisplay::ConfigError::kConfigFailed:
      return static_cast<int32_t>(HWC2::Error::BadConfig);
    case HwcDisplay::ConfigError::kNone:
      return static_cast<int32_t>(HWC2::Error::None);
  }
}

/* Converts long __PRETTY_FUNCTION__ result, e.g.:
 * "int32_t android::LayerHook(hwc2_device_t *, hwc2_display_t, hwc2_layer_t,"
 * "Args...) [HookType = HWC2::Error (android::HwcLayer::*)(const native_handle"
 * "*,int), func = &android::HwcLayer::SetLayerBuffer, Args = <const
 * "native_handle, int>"
 * to the short "android::HwcLayer::SetLayerBuffer" for better logs readability
 */
static std::string GetFuncName(const char *pretty_function) {
  const std::string str(pretty_function);
  const char *start = "func = &";
  auto p1 = str.find(start);
  p1 += strlen(start);
  auto p2 = str.find(',', p1);
  return str.substr(p1, p2 - p1);
}

class Hwc2DeviceDisplay : public FrontendDisplayBase {
 public:
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  std::vector<HwcDisplay::ChangedLayer> changed_layers;

  /* Layers the client is asked to leave transparent for, so that what this
   * composer scans out from a window of its own is not covered by whatever
   * the client target's buffer happened to be carrying. Held between the
   * validate that decided them and the asking that collects them, exactly
   * as the changed types above are. */
  std::vector<ILayerId> punch_out_layers;

  int64_t next_layer_id = 1;
};

/* Says how long a call took, if it took long enough to matter.
 *
 * The client wakes on a blank, draws, asks for the frame to be validated and
 * then shown, and the whole of that has to fit into one refresh. Whatever is
 * spent in here comes out of the client's share of it -- so when frames come
 * out at half the rate, the first thing worth establishing is whether this is
 * where the time went. From outside, a slow composer and a slow client look
 * exactly the same.
 *
 * Only the calls that did not fit are reported. A frame that fits leaves
 * nothing to look into, and a line for each of those would be a cost of its
 * own on a device this size.
 */
class SayIfSlow {
 public:
  explicit SayIfSlow(const char *what)
      : what_(what), entered_ns_(GetTimeMonotonicNs()) {
  }

  ~SayIfSlow() {
    /* A quarter of a refresh. Below that a call is a rounding error against
     * the frame it belongs to; above it, it is a share worth naming. */
    constexpr int64_t kWorthSaying = 4000000;

    const int64_t spent = GetTimeMonotonicNs() - entered_ns_;
    if (spent > kWorthSaying) {
      HWC_LOGD("%s took %" PRId64 "us", what_, spent / 1000);
    }
  }

  SayIfSlow(const SayIfSlow &) = delete;
  SayIfSlow &operator=(const SayIfSlow &) = delete;

 private:
  const char *const what_;
  const int64_t entered_ns_;
};

static auto GetHwc2DeviceDisplay(HwcDisplay &display)
    -> std::shared_ptr<Hwc2DeviceDisplay> {
  auto frontend_private_data = display.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<Hwc2DeviceDisplay>();
    display.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<Hwc2DeviceDisplay>(frontend_private_data);
}

class Hwc2DeviceLayer : public FrontendLayerBase {
 public:
  /* One reading of the buffer per frame, handed over with that frame's
   * fence. Recognising a buffer already seen is not done here any more:
   * the getter remembers shapes by the buffer's own identity, and the
   * framebuffer importer recognises the same identity below -- both of
   * which replaced a swapchain-guessing machine that could never conclude
   * its guess and so described every buffer of every frame from scratch. */
  auto HandleNextBuffer(buffer_handle_t buffer_handle, int32_t fence_fd,
                        FbImporter &importer)
      -> std::optional<HwcLayer::LayerProperties> {
    if (invalid_) {
      return std::nullopt;
    }

    auto bo_info = BufferInfoGetter::GetInstance()->GetBoInfo(buffer_handle);
    if (!bo_info) {
      invalid_ = true;
      return std::nullopt;
    }

    HwcLayer::LayerProperties lp;
    lp.buffer = HwcLayer::Buffer{
        .bi = bo_info.value(),
        .fb = importer.GetOrCreateFbId(&bo_info.value()),
        .fence = MakeSharedFd(fence_fd),
    };

    return lp;
  }

 private:
  bool invalid_{}; /* Layer is invalid and should be skipped */
};

static auto GetHwc2DeviceLayer(HwcLayer &layer)
    -> std::shared_ptr<Hwc2DeviceLayer> {
  auto frontend_private_data = layer.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<Hwc2DeviceLayer>();
    layer.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<Hwc2DeviceLayer>(frontend_private_data);
}

struct Drmhwc2Device : hwc2_device {
  DrmHwcTwo drmhwctwo;
};

static DrmHwcTwo *ToDrmHwcTwo(hwc2_device_t *dev) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast):
  return &static_cast<Drmhwc2Device *>(dev)->drmhwctwo;
}

template <typename PFN, typename T>
static hwc2_function_pointer_t ToHook(T function) {
  // NOLINTNEXTLINE(modernize-type-traits): ToHook is going to be removed
  static_assert(std::is_same<PFN, T>::value, "Incompatible fn pointer");
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast):
  return reinterpret_cast<hwc2_function_pointer_t>(function);
}


/* Registering a callback is the one entry point whose locking depends on the
 * release this is built for, so it does not go through the ordinary hook.
 *
 * Where the framework takes the news of a display away and deals with it
 * afterwards -- every release after Android 9 -- the lock is held across the
 * whole call, which is what upstream does and what the ordinary hook would
 * have done.
 *
 * Where it does not, the lock cannot reach that far. Android 9 handles the
 * display appearing while still inside this call, on this same thread, and
 * asks the composer what kind of display it is before returning -- and that
 * question wants this lock. Nor can the news simply be deferred: the same
 * release checks on return that a primary display was announced during the
 * call, and gives up if none was. So there the lock covers the registration
 * and is let go before the composer speaks, which is exactly what the queue
 * those events sit in is for, and what this composer itself did when Android
 * 9 was the current release.
 *
 * Both are compiled every time. Which one runs is decided by a constant; see
 * utils/FrameworkTraits.h.
 */
static int32_t RegisterCallbackHook(hwc2_device_t *dev, int32_t descriptor,
                                    hwc2_callback_data_t data,
                                    hwc2_function_pointer_t function) {
  ALOGV("Device hook: %s", GetFuncName(__PRETTY_FUNCTION__).c_str());

  auto *hwc = ToDrmHwcTwo(dev);
  int32_t result = 0;

  if (kFrameworkHotplugIsReentrant) {
    {
      const std::unique_lock lock(hwc->GetMainLock());
      result = static_cast<int32_t>(
          hwc->RegisterCallback(descriptor, data, function));
    }

    hwc->FlushHotplugEvents();
  } else {
    const std::unique_lock lock(hwc->GetMainLock());
    result = static_cast<int32_t>(
        hwc->RegisterCallback(descriptor, data, function));
    hwc->FlushHotplugEvents();
  }

  return result;
}

static int HookDevClose(hw_device_t *dev) {
  // NOLINTNEXTLINE (cppcoreguidelines-pro-type-reinterpret-cast): Safe
  auto *hwc2_dev = reinterpret_cast<hwc2_device_t *>(dev);
  const std::unique_ptr<DrmHwcTwo> ctx(ToDrmHwcTwo(hwc2_dev));
  return 0;
}

static void HookDevGetCapabilities(hwc2_device_t * /*dev*/, uint32_t *out_count,
                                   int32_t *out_capabilities) {
  /* The one claim this device makes: its display controller applies the
   * client's colour transform itself, so the client must not bake it into
   * what it composes -- claimed and honoured behind the same switch, read
   * once by each side, so they cannot disagree. The client reads this list
   * once at its start; every restart of the composer service restarts the
   * client through init's onrestart rule -- the same rule that already
   * guards against a client outliving its composer -- so a change of the
   * switch reaches both, whatever killed the service. This build serves
   * one board; were its DRM pipeline ever used, the claim would need the
   * backend's word too, because a display without a CTM property would be
   * left with nobody applying anything. */
  if (!Properties::CmuColorPipeline()) {
    *out_count = 0;
    return;
  }

  if (out_capabilities == nullptr) {
    *out_count = 1;
    return;
  }
  if (*out_count >= 1) {
    out_capabilities[0] = HWC2_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
    *out_count = 1;
  }
}

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

#define LOCK_COMPOSER(dev)       \
  auto *ihwc = ToDrmHwcTwo(dev); \
  const std::unique_lock lock(ihwc->GetMainLock());

#define GET_DISPLAY(display_handle)                  \
  auto *idisplay = ihwc->GetDisplay(display_handle); \
  if (!idisplay)                                     \
    return static_cast<int32_t>(HWC2::Error::BadDisplay);

#define GET_LAYER(layer_id)                     \
  auto *ilayer = idisplay->get_layer(layer_id); \
  if (!ilayer)                                  \
    return static_cast<int32_t>(HWC2::Error::BadLayer);

// NOLINTEND(cppcoreguidelines-macro-usage)

/* What upstream calls a colour space here is the matrix a buffer's colour was
 * encoded with, and the type it goes into was renamed to say so after this
 * file was written. The mapping is unchanged. */
static BufferColorEncoding Hwc2ToColorSpace(int32_t dataspace) {
  switch (dataspace & HAL_DATASPACE_STANDARD_MASK) {
    case HAL_DATASPACE_STANDARD_BT709:
      return BufferColorEncoding::kItuRec709;
    case HAL_DATASPACE_STANDARD_BT601_625:
    case HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED:
    case HAL_DATASPACE_STANDARD_BT601_525:
    case HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED:
      return BufferColorEncoding::kItuRec601;
    case HAL_DATASPACE_STANDARD_BT2020:
    case HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE:
      return BufferColorEncoding::kItuRec2020;
    default:
      return BufferColorEncoding::kUndefined;
  }
}

static BufferSampleRange Hwc2ToSampleRange(int32_t dataspace) {
  switch (dataspace & HAL_DATASPACE_RANGE_MASK) {
    case HAL_DATASPACE_RANGE_FULL:
      return BufferSampleRange::kFullRange;
    case HAL_DATASPACE_RANGE_LIMITED:
      return BufferSampleRange::kLimitedRange;
    default:
      return BufferSampleRange::kUndefined;
  }
}

/* Device functions */
static int32_t Dump(hwc2_device_t *device, uint32_t *out_size,
                    char *out_buffer) {
  if (out_size == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  /* Locked like every other device function, and this one long was not.
   * The refresh below walks counters the commit thread is incrementing and
   * resets them as it reads; unlocked, two dumps also raced each other over
   * the kept string. A dump is rare and a frame is not, so the frame paying
   * an occasional wait here is the cheap direction. */
  LOCK_COMPOSER(device);

  if (out_buffer != nullptr) {
    const std::string &last_dump = ihwc->GetLastStateDump();
    auto copied_bytes = last_dump.copy(out_buffer, *out_size);
    *out_size = copied_bytes;
    return 0;
  }

  const std::string &new_dump = ihwc->RefreshStateDump();
  *out_size = static_cast<uint32_t>(new_dump.size());
  return 0;
}

static int32_t CreateVirtualDisplay(hwc2_device_t *device, uint32_t width,
                                    uint32_t height, int32_t * /*format*/,
                                    hwc2_display_t *out_display_handle) {
  ALOGV("CreateVirtualDisplay");
  LOCK_COMPOSER(device);
  auto display_handle = ihwc->CreateVirtualDisplay(width, height);
  if (!display_handle) {
    return static_cast<int32_t>(HWC2::Error::Unsupported);
  }

  *out_display_handle = display_handle.value();
  return 0;
}

static int32_t DestroyVirtualDisplay(hwc2_device_t *device,
                                     hwc2_display_t display) {
  ALOGV("DestroyVirtualDisplay");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  if (!ihwc->DestroyVirtualDisplay(static_cast<DisplayHandle>(display))) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }
  return 0;
}

static int32_t GetMaxVirtualDisplayCount(hwc2_device_t *device) {
  ALOGV("GetMaxVirtualDisplayCount");
  LOCK_COMPOSER(device);
  return static_cast<int32_t>(ihwc->GetMaxVirtualDisplayCount());
}

/* Display functions */
static int32_t CreateLayer(hwc2_device_t *device, hwc2_display_t display,
                           hwc2_layer_t *out_layer) {
  ALOGV("CreateLayer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  if (!idisplay->CreateLayer(hwc2display->next_layer_id)) {
    return static_cast<int32_t>(HWC2::Error::BadDisplay);
  }

  *out_layer = (hwc2_layer_t)hwc2display->next_layer_id;
  hwc2display->next_layer_id++;

  return 0;
}

static int32_t DestroyLayer(hwc2_device_t *device, hwc2_display_t display,
                            hwc2_layer_t layer) {
  ALOGV("DestroyLayer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (!idisplay->DestroyLayer((ILayerId)layer)) {
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  return 0;
}

static int32_t GetActiveConfig(hwc2_device_t *device, hwc2_display_t display,
                               hwc2_config_t *config) {
  ALOGV("GetActiveConfig");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  // If a config has been queued, it is considered the "active" config.
  const HwcDisplayConfig *hwc_config = idisplay->GetLastRequestedConfig();
  if (hwc_config == nullptr)
    return static_cast<int32_t>(HWC2::Error::BadConfig);

  *config = hwc_config->id;
  return 0;
}

/* Which layers the client must leave transparent for.
 *
 * A layer this composer took for the hardware and put below the client
 * target is scanned out from a window of its own, with the client target
 * lying over it. The client clears its target under such a layer only when
 * asked here, by name; unasked, the target keeps whatever its buffer was
 * carrying from an earlier frame and covers the layer with it. After a
 * rotation that is the picture in the old orientation, laid over the new
 * one -- which is what this stub cost until it stopped being a stub.
 *
 * Nothing is asked of the display as a whole, only of layers. */
static int32_t GetDisplayRequests(hwc2_device_t *device,
                                  hwc2_display_t display,
                                  int32_t *out_display_requests,
                                  uint32_t *out_num_elements,
                                  hwc2_layer_t *out_layers,
                                  int32_t *out_layer_requests) {
  ALOGV("GetDisplayRequests");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  if (out_display_requests != nullptr)
    *out_display_requests = 0;

  /* Asked twice, the first time only to learn how much room to make. The
   * count is what is answered then, and the answer must survive until the
   * second asking, so nothing is cleared here. */
  if (out_layers == nullptr || out_layer_requests == nullptr) {
    *out_num_elements = hwc2display->punch_out_layers.size();
    return static_cast<int32_t>(HWC2::Error::None);
  }

  if (*out_num_elements < hwc2display->punch_out_layers.size()) {
    ALOGW("Overflow num_elements %d/%zu", *out_num_elements,
          hwc2display->punch_out_layers.size());
    return static_cast<int32_t>(HWC2::Error::NoResources);
  }

  for (size_t i = 0; i < hwc2display->punch_out_layers.size(); ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_layers[i] = hwc2display->punch_out_layers[i];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_layer_requests[i] = HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET;
  }

  *out_num_elements = hwc2display->punch_out_layers.size();
  hwc2display->punch_out_layers.clear();

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t GetDisplayType(hwc2_device_t *device, hwc2_display_t display,
                              int32_t *out_type) {
  ALOGV("GetDisplayType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  switch (idisplay->GetDisplayType()) {
    case HwcDisplay::DisplayType::kVirtual:
      *out_type = static_cast<int32_t>(HWC2::DisplayType::Virtual);
      break;
    case HwcDisplay::DisplayType::kInternal:
    case HwcDisplay::DisplayType::kExternal:
      *out_type = static_cast<int32_t>(HWC2::DisplayType::Physical);
      break;
  }
  return 0;
}

static int32_t GetDozeSupport(hwc2_device_t * /*device*/,
                              hwc2_display_t /*display*/,
                              int32_t *out_support) {
  ALOGV("GetDozeSupport");
  *out_support = 0;  // Doze support is not available
  return 0;
}

static int32_t GetClientTargetSupport(hwc2_device_t * /*device*/,
                                      hwc2_display_t /*display*/,
                                      uint32_t /*width*/, uint32_t /*height*/,
                                      int32_t /*format*/, int32_t dataspace) {
  ALOGV("GetClientTargetSupport");

  if (dataspace != HAL_DATASPACE_UNKNOWN)
    return static_cast<int32_t>(HWC2::Error::Unsupported);

  return 0;
}

static int32_t SetClientTarget(hwc2_device_t *device, hwc2_display_t display,
                               buffer_handle_t target, int32_t acquire_fence,
                               int32_t dataspace, hwc_region_t /*damage*/) {
  ALOGV("SetClientTarget");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto &client_layer = idisplay->GetClientLayer();
  auto h2l = GetHwc2DeviceLayer(client_layer);

  if (target == nullptr) {
    return 0;
  }

  auto lp = h2l->HandleNextBuffer(target, acquire_fence,
                                  *idisplay->GetPipe().importer);
  if (!lp) {
    ALOGE("Failed to process client target");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  lp->color_encoding = Hwc2ToColorSpace(dataspace);
  lp->sample_range = Hwc2ToSampleRange(dataspace);

  /* Panel-sized by construction, and said out loud: with the rects left
   * unsaid every geometry gate in plane eligibility silently skipped the
   * client target, and the missing-rect convention means three different
   * things at three sites. */
  const auto client_size = idisplay->GetSize();
  lp->display_frame = DstRectInfo{
      IRect{0, 0, static_cast<int32_t>(client_size.first),
            static_cast<int32_t>(client_size.second)}};
  lp->source_crop = SrcRectInfo{
      FRect{0.F, 0.F, static_cast<float>(client_size.first),
            static_cast<float>(client_size.second)}};

  idisplay->GetClientLayer().SetLayerProperties(lp.value());

  return 0;
}

static int32_t GetColorModes(hwc2_device_t *device, hwc2_display_t display,
                             uint32_t *num_modes, int32_t *out_modes) {
  ALOGV("GetColorModes");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  const std::vector<ColorMode> modes = idisplay->GetColorModes();
  if (modes.empty())
    return static_cast<int32_t>(HWC2::Error::BadConfig);

  /* Asked twice: once with nowhere to put the answer, only to learn how much
   * room to make, and again with the room made. Writing on the first asking
   * is a write through nothing. */
  if (out_modes != nullptr) {
    for (uint32_t i = 0; i < modes.size(); ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
      out_modes[i] = static_cast<int32_t>(modes[i]);
    }
  }

  *num_modes = modes.size();
  return 0;
}

static int32_t GetDisplayAttribute(hwc2_device_t *device,
                                   hwc2_display_t display, hwc2_config_t config,
                                   int32_t attribute, int32_t *value) {
  ALOGV("GetDisplayAttribute");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  const auto *hwc_config = idisplay->GetConfig(static_cast<ConfigId>(config));

  if (hwc_config == nullptr) {
    ALOGE("Could not find mode #%d", config);
    return static_cast<int32_t>(HWC2::Error::BadConfig);
  }

  int mm_width = -1;
  int mm_height = -1;
  std::tie(mm_width, mm_height) = idisplay->GetDisplayBoundsMm();
  std::optional<std::pair<float, float>> dpi_inches = {};

  if (mm_width > 0) {
    static const float kMmPerInch = 25.4;
    float dpi_x = float(hwc_config->mode.GetRawMode().hdisplay) * kMmPerInch /
                  float(mm_width);
    float dpi_y = mm_height <= 0
                      ? dpi_x
                      : float(hwc_config->mode.GetRawMode().vdisplay) *
                            kMmPerInch / float(mm_height);
    dpi_inches = std::make_pair(dpi_x, dpi_y);
  }

  static const int kLegacyDpiUnit = 1000;
  switch (static_cast<HWC2::Attribute>(attribute)) {
    case HWC2::Attribute::Width:
      *value = static_cast<int>(hwc_config->mode.GetRawMode().hdisplay);
      break;
    case HWC2::Attribute::Height:
      *value = static_cast<int>(hwc_config->mode.GetRawMode().vdisplay);
      break;
    case HWC2::Attribute::VsyncPeriod:
      // in nanoseconds
      *value = hwc_config->mode.GetVSyncPeriodNs();
      break;
    case HWC2::Attribute::DpiY:
      *value = dpi_inches
                   ? static_cast<int>(dpi_inches->second * kLegacyDpiUnit)
                   : -1;
      break;
    case HWC2::Attribute::DpiX:
      *value = dpi_inches ? static_cast<int>(dpi_inches->first * kLegacyDpiUnit)
                          : -1;
      break;
    case HWC2::Attribute::ConfigGroup:
      /* Dispite ConfigGroup is a part of HWC2.4 API, framework
       * able to request it even if service @2.1 is used */
      *value = int(hwc_config->group_id);
      break;
    default:
      *value = -1;
      return static_cast<int32_t>(HWC2::Error::BadConfig);
  }
  return 0;
}

static int32_t GetDisplayConfigs(hwc2_device_t *device, hwc2_display_t display,
                                 uint32_t *num_configs,
                                 hwc2_config_t *configs) {
  ALOGV("GetDisplayConfigs");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  uint32_t idx = 0;
  /* Upstream skipped configs marked disabled here. A config no longer
   * carries that mark -- the ones that cannot be used are left out when the
   * list is built, which is earlier and truer. */
  for (const auto &hwc_config : idisplay->GetDisplayConfigs()) {
    if (configs != nullptr) {
      if (idx >= *num_configs) {
        break;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
      configs[idx] = hwc_config.id;
    }

    idx++;
  }
  *num_configs = idx;
  return 0;
}

static int32_t GetDisplayName(hwc2_device_t *device, hwc2_display_t display,
                              uint32_t *size, char *name) {
  ALOGV("GetDisplayName");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  std::string name_str = idisplay->GetDisplayName();

  auto length = name_str.length();
  if (name == nullptr) {
    *size = length;
    return 0;
  }

  *size = std::min<uint32_t>(static_cast<uint32_t>(length - 1), *size);
  strncpy(name, name_str.c_str(), *size);
  return 0;
}

/* The wide and HDR families are modeset decisions on this display, and
 * every colour-mode door must agree on that: a mode the setters refuse
 * is a mode the intent list must not advertise either. Each door still
 * answers in its own spec vocabulary -- the getter has no Unsupported,
 * the setters do. */
static bool IsModesetOnlyColorMode(int32_t mode) {
  return mode == HAL_COLOR_MODE_DISPLAY_BT2020 ||
         mode == HAL_COLOR_MODE_ADOBE_RGB ||
         mode == HAL_COLOR_MODE_BT2020 ||
         mode == HAL_COLOR_MODE_BT2100_PQ ||
         mode == HAL_COLOR_MODE_BT2100_HLG;
}

static int32_t SetColorMode(hwc2_device_t *device, hwc2_display_t display, int32_t mode) {
  ALOGV("SetColorMode");
  if (mode < HAL_COLOR_MODE_NATIVE || mode > HAL_COLOR_MODE_DISPLAY_BT2020)
    return static_cast<int32_t>(HWC2::Error::BadParameter);

  // HDR color modes should be requested during modeset
  if (IsModesetOnlyColorMode(mode)) {
    return static_cast<int32_t>(HWC2::Error::Unsupported);
  }

  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  // Values for color modes match across HWC versions, so static cast is safe:
  // https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/graphics/composer/aidl/android/hardware/graphics/composer3/ColorMode.aidl
  // https://cs.android.com/android/platform/superproject/main/+/main:system/core/libsystem/include/system/graphics-base-v1.0.h;drc=7d940ae4afa450696afa25e07982f3a95e17e9b2;l=118
  // https://cs.android.com/android/platform/superproject/main/+/main:system/core/libsystem/include/system/graphics-base-v1.1.h;drc=7d940ae4afa450696afa25e07982f3a95e17e9b2;l=35
  /* A colour mode now arrives with the intent it is to be rendered under.
   * This entry point has no way to say one -- the framework only gained that
   * later -- so it asks for the plain one, which is what a display without
   * the choice was doing all along. */
  idisplay->SetColorMode(static_cast<ColorMode>(mode),
                         ui::RenderIntent::COLORIMETRIC);
  return 0;
}

static int32_t SetColorTransform(hwc2_device_t *device, hwc2_display_t display,
                                 const float *matrix, int32_t hint) {
  ALOGV("SetColorTransform");
  if (hint < HAL_COLOR_TRANSFORM_IDENTITY ||
      hint > HAL_COLOR_TRANSFORM_CORRECT_TRITANOPIA) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (hint != HAL_COLOR_TRANSFORM_ARBITRARY_MATRIX &&
      hint != HAL_COLOR_TRANSFORM_IDENTITY) {
    return static_cast<int32_t>(HWC2::Error::Unsupported);
  }

  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (matrix == nullptr) {
    if (hint == HAL_COLOR_TRANSFORM_IDENTITY) {
      idisplay->SetColorTransformMatrix(kIdentityMatrix);
      return 0;
    }

    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  std::array<float, kColorMatrixSize> aidl_matrix = kIdentityMatrix;
  memcpy(aidl_matrix.data(), matrix, aidl_matrix.size() * sizeof(float));
  idisplay->SetColorTransformMatrix(aidl_matrix);

  return 0;
}

static int32_t SetOutputBuffer(hwc2_device_t *device, hwc2_display_t display,
                               buffer_handle_t buffer, int32_t release_fence) {
  ALOGV("SetOutputBuffer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto &writeback_layer = idisplay->GetWritebackLayer();
  if (!writeback_layer) {
    ALOGE("Writeback layer is not available");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  auto h2l = GetHwc2DeviceLayer(*writeback_layer);

  auto lp = h2l->HandleNextBuffer(buffer, release_fence,
                                  *idisplay->GetPipe().importer);
  if (!lp) {
    ALOGE("Failed to process output buffer");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  writeback_layer->SetLayerProperties(lp.value());

  return 0;
}

static int32_t AcceptDisplayChanges(hwc2_device_t *device,
                                    hwc2_display_t display) {
  ALOGV("AcceptDisplayChanges");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  idisplay->AcceptValidatedComposition();

  return 0;
}

static int32_t GetHdrCapabilities(hwc2_device_t *device, hwc2_display_t display,
                                  uint32_t *num_types, int32_t *types,
                                  float *max_luminance,
                                  float *max_average_luminance,
                                  float *min_luminance) {
  ALOGV("GetHdrCapabilities");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  std::vector<ui::Hdr> temp_types;
  idisplay->GetHdrCapabilities(&temp_types, max_luminance,
                               max_average_luminance, min_luminance);
  uint32_t i = 0;
  for (auto &t : temp_types) {
    switch (t) {
      case ui::Hdr::HDR10:
        /* Counted either way; written only where there is somewhere to write
         * -- the counting asking, again, see GetColorModes. */
        if (types != nullptr) {
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
          types[i] = HAL_HDR_HDR10;
        }
        i++;
        break;
      case ui::Hdr::HLG:
        if (types != nullptr) {
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
          types[i] = HAL_HDR_HLG;
        }
        i++;
        break;
      default:
        // Ignore any other HDR types
        break;
    }
  }

  *num_types = i;

  return 0;
}

static int32_t GetReleaseFences(hwc2_device_t *device, hwc2_display_t display,
                                uint32_t *out_num_elements,
                                hwc2_layer_t *out_layers, int32_t *out_fences) {
  ALOGV("GetReleaseFences");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  /* The counting asking, again -- and here nothing may be handed out on it
   * at all: the fences are given away, and giving them away twice would have
   * the client close each of them twice. */
  if (out_layers == nullptr || out_fences == nullptr) {
    *out_num_elements = hwc2display->release_fences.size();
    return static_cast<int32_t>(HWC2::Error::None);
  }

  if (*out_num_elements < hwc2display->release_fences.size()) {
    ALOGW("Overflow num_elements %d/%zu", *out_num_elements,
          hwc2display->release_fences.size());
    return static_cast<int32_t>(HWC2::Error::NoResources);
  }

  for (size_t i = 0; i < hwc2display->release_fences.size(); ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_layers[i] = hwc2display->release_fences[i].first;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_fences[i] = DupFd(hwc2display->release_fences[i].second);
  }

  *out_num_elements = hwc2display->release_fences.size();
  hwc2display->release_fences.clear();

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetPowerMode(hwc2_device_t *device, hwc2_display_t display,
                            int32_t mode) {
  ALOGV("SetPowerMode");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  switch (mode) {
    // Supported modes.
    case static_cast<int32_t>(HWC2::PowerMode::Off):
    case static_cast<int32_t>(HWC2::PowerMode::On):
      break;
    // Unsupported modes.
    case static_cast<int32_t>(HWC2::PowerMode::Doze):
    case static_cast<int32_t>(HWC2::PowerMode::DozeSuspend):
      return static_cast<int32_t>(HWC2::Error::Unsupported);
    // Bad parameter.
    default:
      ALOGE("Incorrect power mode value (%d)\n", mode);
      return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  /* Turning a display on and off is a power mode now rather than a boolean,
   * which is the same two states this switch has already narrowed the request
   * to -- and it says why it refused rather than only that it did. */
  const auto err = idisplay
                       ->SetPowerMode(mode == static_cast<int32_t>(
                                                  HWC2::PowerMode::On)
                                          ? PowerMode::kOn
                                          : PowerMode::kOff);
  switch (err) {
    case HwcDisplay::Error::kNone:
      break;
    case HwcDisplay::Error::kUnsupported:
      return static_cast<int32_t>(HWC2::Error::Unsupported);
    case HwcDisplay::Error::kBadParameter:
    default:
      return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetVsyncEnabled(hwc2_device_t *device, hwc2_display_t display,
                               int32_t enabled) {
  ALOGV("SetVsyncEnabled");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  idisplay->SetVsyncCallbacksEnabled(HWC2_VSYNC_ENABLE == enabled);
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t ValidateDisplay(hwc2_device_t *device, hwc2_display_t display,
                               uint32_t *out_num_types,
                               uint32_t *out_num_requests) {
  ALOGV("ValidateDisplay");

  /* The other half of what the composer costs the frame; see PresentDisplay.
   * Timed by an object rather than by a line before each return, because the
   * checks below leave through several of them. */
  const SayIfSlow timed("validate");

  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  /* Both halves of the answer are kept. The changed types are collected by
   * one asking, the holes to punch by another, and this entry point says how
   * many of each are waiting.
   *
   * The second half used to be dropped here. Nothing downstream noticed,
   * because the client asks for it separately and got an empty answer -- so
   * it cleared nothing under the layers this composer took, and its target
   * carried old pixels over them wherever they lay below it. */
  auto validated = idisplay->ValidateStagedComposition();
  hwc2display->changed_layers = std::move(validated.changed_layers);
  hwc2display->punch_out_layers = std::move(validated.punch_out_layers);

  *out_num_types = hwc2display->changed_layers.size();
  *out_num_requests = hwc2display->punch_out_layers.size();

  return 0;
}

static int32_t GetChangedCompositionTypes(hwc2_device_t *device,
                                          hwc2_display_t display,
                                          uint32_t *out_num_elements,
                                          hwc2_layer_t *out_layers,
                                          int32_t *out_types) {
  ALOGV("GetChangedCompositionTypes");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  /* Asked twice, the first time only to learn how much room to make -- see
   * the note in GetColorModes. The count is what is answered then, and the
   * answer must survive until the second asking, so nothing is cleared. */
  if (out_layers == nullptr || out_types == nullptr) {
    *out_num_elements = hwc2display->changed_layers.size();
    return static_cast<int32_t>(HWC2::Error::None);
  }

  if (*out_num_elements < hwc2display->changed_layers.size()) {
    ALOGW("Overflow num_elements %d/%zu", *out_num_elements,
          hwc2display->changed_layers.size());
    return static_cast<int32_t>(HWC2::Error::NoResources);
  }

  for (size_t i = 0; i < hwc2display->changed_layers.size(); ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_layers[i] = hwc2display->changed_layers[i].first;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_types[i] = static_cast<int32_t>(hwc2display->changed_layers[i].second);
  }

  *out_num_elements = hwc2display->changed_layers.size();
  hwc2display->changed_layers.clear();

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t PresentDisplay(hwc2_device_t *device, hwc2_display_t display,
                              int32_t *out_release_fence) {
  ALOGV("PresentDisplay");

  /* What the composer costs the frame it is part of.
   *
   * The client wakes on a blank, draws, asks this to be validated and then
   * shown, and the whole of that has to fit in one refresh. Everything spent
   * in here is taken out of the client's share of it, so when frames come out
   * at half the rate the first thing worth establishing is whether this is
   * where the time went -- and that cannot be read off the outside, because
   * from there a slow composer and a slow client look the same.
   *
   * Said only when it is worth saying. A frame that fits leaves nothing to
   * investigate, and a line per frame would itself be part of the cost.
   */
  const SayIfSlow timed("present");

  /* Taken separately from the work that follows, because waiting for it and
   * doing something are different answers to "where did the frame go". The
   * lock is the composer's one door: every call the client makes goes through
   * it, and one that is already inside holds up the rest. */
  const int64_t before_lock = GetTimeMonotonicNs();
  LOCK_COMPOSER(device);
  const int64_t waited = GetTimeMonotonicNs() - before_lock;
  if (waited > 1000000) {
    HWC_LOGD("present waited %" PRId64 "us for the composer lock",
             waited / 1000);
  }

  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  SharedFd out_fence;

  hwc2display->release_fences.clear();

  if (!idisplay->PresentStagedComposition(std::nullopt, out_fence,
                                          hwc2display->release_fences)) {
    ALOGE("Failed to present display");
    return static_cast<int32_t>(HWC2::Error::BadDisplay);
  }

  *out_release_fence = DupFd(out_fence);

  return 0;
}

static int32_t SetActiveConfig(hwc2_device_t *device, hwc2_display_t display,
                               hwc2_config_t config) {
  ALOGV("SetActiveConfig");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  QueuedConfigTiming out_timing{};
  const auto config_id = static_cast<ConfigId>(config);
  auto error = idisplay->QueueConfig(config_id,
                                     GetTimeMonotonicNs(),
                                     &out_timing);

  if (error == HwcDisplay::kSeamlessNotAllowed) {
    // Fallback to a full blocking modeset.
    error = idisplay->SetConfig(config_id);
  }

  return ConfigErrorToHWC2(error);
}

static int32_t GetDisplayBrightnessSupport(hwc2_device_t * /*device*/,
                                           hwc2_display_t /*display*/,
                                           bool *out_support) {
  ALOGV("GetDisplayBrightnessSupport");
  *out_support = false;  // Brightness support is not available
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetDisplayBrightness(hwc2_device_t * /*device*/,
                                    hwc2_display_t /*display*/,
                                    float /*brightness*/) {
  ALOGV("SetDisplayBrightness");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetRenderIntents(hwc2_device_t *device,
                                hwc2_display_t display, int32_t mode,
                                uint32_t *num_intents, int32_t *intents) {
  ALOGV("GetRenderIntents");

  if (mode < HAL_COLOR_MODE_NATIVE || mode > HAL_COLOR_MODE_DISPLAY_BT2020 ||
      IsModesetOnlyColorMode(mode))
    return static_cast<int32_t>(HWC2::Error::BadParameter);

  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  const auto render_intents = idisplay->GetRenderIntents(
      static_cast<ColorMode>(mode));

  /* Asked twice, like the colour modes above: once with nowhere to put
   * the answer, only to learn how much room to make, and again with the
   * room made. The old stub wrote on the first asking -- a write
   * through nothing that no 2.1 client ever made, and every 2.2 client
   * would have. */
  if (intents != nullptr) {
    for (uint32_t i = 0; i < render_intents.size(); ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
      intents[i] = static_cast<int32_t>(render_intents[i]);
    }
  }
  *num_intents = render_intents.size();

  return 0;
}

static int32_t SetColorModeWithRenderIntent(hwc2_device_t *device,
                                            hwc2_display_t display,
                                            int32_t mode, int32_t intent) {
  ALOGV("SetColorModeWithRenderIntent");
  /* The old guard held the mode against the intent's boundaries -- a
   * transposition that let nothing legal through by accident. The mode
   * answers to the mode's range and the intent to the intent's, plus
   * the one vendor value the framework passes through numerically. */
  if (mode < HAL_COLOR_MODE_NATIVE || mode > HAL_COLOR_MODE_DISPLAY_BT2020)
    return static_cast<int32_t>(HWC2::Error::BadParameter);

  const bool standard_intent = intent >= HAL_RENDER_INTENT_COLORIMETRIC &&
                               intent <= HAL_RENDER_INTENT_TONE_MAP_ENHANCE;
  if (!standard_intent &&
      intent != static_cast<int32_t>(kVendorBoostedRenderIntent))
    return static_cast<int32_t>(HWC2::Error::BadParameter);

  // HDR color modes should be requested during modeset
  if (IsModesetOnlyColorMode(mode)) {
    return static_cast<int32_t>(HWC2::Error::Unsupported);
  }

  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  idisplay->SetColorMode(static_cast<ColorMode>(mode),
                         static_cast<ui::RenderIntent>(intent));
  return 0;
}

static int32_t GetDataspaceSaturationMatrix(hwc2_device_t * /*device*/,
                                            int32_t dataspace,
                                            float *out_matrix) {
  ALOGV("GetDataspaceSaturationMatrix");
  if (dataspace != HAL_DATASPACE_SRGB_LINEAR)
    return static_cast<int32_t>(HWC2::Error::BadParameter);

  /* Identity, deliberately. This matrix exists for the legacy dataspace
   * saturation the client composition applies itself; our saturation is
   * a render intent, applied by the display's own colour machinery.
   * Anything but identity here and a saturated frame that fell to the
   * GPU would be saturated twice. */
  static constexpr float kIdentity[16] = {1, 0, 0, 0,  //
                                          0, 1, 0, 0,  //
                                          0, 0, 1, 0,  //
                                          0, 0, 0, 1};
  for (int i = 0; i < 16; i++) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_matrix[i] = kIdentity[i];
  }
  return 0;
}

/* The rest of the 2.2 surface names things this hardware does not do,
 * and says so the way upstream says it: an honest Unsupported, never a
 * pretend success. Per-frame metadata is refused at the root -- a key
 * list of nothing -- so the framework never sends any; readback has no
 * engine behind it on a controller whose only output is the panel. */

static int32_t GetPerFrameMetadataKeys(hwc2_device_t * /*device*/,
                                       hwc2_display_t /*display*/,
                                       uint32_t *num_keys,
                                       int32_t * /*keys*/) {
  ALOGV("GetPerFrameMetadataKeys");
  *num_keys = 0;
  return 0;
}

static int32_t SetLayerPerFrameMetadata(hwc2_device_t * /*device*/,
                                        hwc2_display_t /*display*/,
                                        hwc2_layer_t /*layer*/,
                                        uint32_t /*num_elements*/,
                                        const int32_t * /*keys*/,
                                        const float * /*metadata*/) {
  ALOGV("SetLayerPerFrameMetadata");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t SetLayerFloatColor(hwc2_device_t * /*device*/,
                                  hwc2_display_t /*display*/,
                                  hwc2_layer_t /*layer*/,
                                  hwc_float_color_t /*color*/) {
  ALOGV("SetLayerFloatColor");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t SetReadbackBuffer(hwc2_device_t * /*device*/,
                                 hwc2_display_t /*display*/,
                                 buffer_handle_t /*buffer*/,
                                 int32_t release_fence) {
  ALOGV("SetReadbackBuffer");
  /* The fence arrives owned by us whatever the answer is. */
  if (release_fence >= 0)
    close(release_fence);
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetReadbackBufferAttributes(hwc2_device_t * /*device*/,
                                           hwc2_display_t /*display*/,
                                           int32_t * /*format*/,
                                           int32_t * /*dataspace*/) {
  ALOGV("GetReadbackBufferAttributes");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetReadbackBufferFence(hwc2_device_t * /*device*/,
                                      hwc2_display_t /*display*/,
                                      int32_t *out_fence) {
  ALOGV("GetReadbackBufferFence");
  *out_fence = -1;
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetDisplayIdentificationData(hwc2_device_t *device,
                                            hwc2_display_t display,
                                            uint8_t *out_port,
                                            uint32_t *out_data_size,
                                            uint8_t *out_data) {
  ALOGV("GetDisplayIdentificationData");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto edid = idisplay->GetRawEdid();
  if (edid.empty()) {
    return static_cast<int32_t>(HWC2::Error::Unsupported);
  }

  *out_port = idisplay->GetPort();

  if (out_data != nullptr) {
    *out_data_size = std::min(*out_data_size,
                              static_cast<uint32_t>(edid.size()));
    memcpy(out_data, edid.data(), *out_data_size);
  } else {
    *out_data_size = edid.size();
  }

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t GetDisplayCapabilities(hwc2_device_t *device,
                                      hwc2_display_t display,
                                      uint32_t *out_num_capabilities,
                                      uint32_t *out_capabilities) {
  ALOGV("GetDisplayCapabilities");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (out_num_capabilities == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (ihwc->GetCtmHandling() == CtmHandling::kDrmOrIgnore) {
    if (out_capabilities != nullptr && *out_num_capabilities > 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
      out_capabilities[0] = HWC2_DISPLAY_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
    }
    *out_num_capabilities = 1;
  }

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t GetDisplayConnectionType(hwc2_device_t *device,
                                        hwc2_display_t display,
                                        int32_t *out_connection_type) {
  ALOGV("GetDisplayConnectionType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  switch (idisplay->GetDisplayType()) {
    case HwcDisplay::DisplayType::kVirtual:
      return static_cast<int32_t>(HWC2::Error::BadDisplay);
    case HwcDisplay::DisplayType::kInternal:
      *out_connection_type = static_cast<int32_t>(
          HWC2::DisplayConnectionType::Internal);
      break;
    case HwcDisplay::DisplayType::kExternal:
      *out_connection_type = static_cast<int32_t>(
          HWC2::DisplayConnectionType::External);
      break;
  }
  return 0;
}

static int32_t GetDisplayVsyncPeriod(hwc2_device_t *device,
                                     hwc2_display_t display,
                                     hwc2_vsync_period_t *out_vsync_period) {
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  const HwcDisplayConfig *config = idisplay->GetCurrentConfig();
  if (config == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadConfig);
  }

  *out_vsync_period = config->mode.GetVSyncPeriodNs();
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetActiveConfigWithConstraints(
    hwc2_device_t *device, hwc2_display_t display, hwc2_config_t config,
    hwc_vsync_period_change_constraints_t *vsync_period_change_constraints,
    hwc_vsync_period_change_timeline_t *out_timeline) {
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (vsync_period_change_constraints == nullptr || out_timeline == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (vsync_period_change_constraints->seamlessRequired != 0) {
    return static_cast<int32_t>(HWC2::Error::SeamlessNotAllowed);
  }

  const auto config_id = static_cast<ConfigId>(config);
  QueuedConfigTiming out_timing{};
  auto error = idisplay->QueueConfig(config_id,
                                     vsync_period_change_constraints
                                         ->desiredTimeNanos,
                                     &out_timing);

  if (error == HwcDisplay::kNone) {
    out_timeline->newVsyncAppliedTimeNanos = out_timing.new_vsync_time_ns;
    out_timeline->refreshTimeNanos = out_timing.refresh_time_ns;
    out_timeline->refreshRequired = 1U;
  } else if (error == HwcDisplay::kSeamlessNotAllowed) {
    error = idisplay->SetConfig(config_id);
    out_timeline
        ->newVsyncAppliedTimeNanos = GetTimeMonotonicNs();
    out_timeline->refreshRequired = 0U;
  }

  return ConfigErrorToHWC2(error);
}

static int32_t SetAutoLowLatencyMode(hwc2_device_t * /*device*/,
                                     hwc2_display_t /*display*/, bool /*on*/) {
  ALOGV("SetAutoLowLatencyMode");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetSupportedContentTypes(
    hwc2_device_t * /*device*/, hwc2_display_t /*display*/,
    uint32_t *out_num_supported_content_types,
    uint32_t * /*out_supported_content_types*/) {
  ALOGV("GetSupportedContentTypes");
  *out_num_supported_content_types = 0;
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetContentType(hwc2_device_t *device, hwc2_display_t display,
                              int32_t content_type) {
  ALOGV("SetContentType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (content_type < HWC2_CONTENT_TYPE_NONE ||
      content_type > HWC2_CONTENT_TYPE_GAME) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  idisplay->SetContentType(static_cast<ContentType>(content_type));

  return static_cast<int32_t>(HWC2::Error::None);
}

/* Layer functions */

static int32_t SetLayerBlendMode(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer,
                                 int32_t /*hwc2_blend_mode_t*/ mode) {
  ALOGV("SetLayerBlendMode");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  BufferBlendMode blend_mode{};
  switch (static_cast<HWC2::BlendMode>(mode)) {
    case HWC2::BlendMode::None:
      blend_mode = BufferBlendMode::kNone;
      break;
    case HWC2::BlendMode::Premultiplied:
      blend_mode = BufferBlendMode::kPreMult;
      break;
    case HWC2::BlendMode::Coverage:
      blend_mode = BufferBlendMode::kCoverage;
      break;
    default:
      ALOGE("Unknown blending mode b=%d", mode);
      blend_mode = BufferBlendMode::kUndefined;
      break;
  }

  HwcLayer::LayerProperties layer_properties;
  layer_properties.blend_mode = blend_mode;

  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerBuffer(hwc2_device_t *device, hwc2_display_t display,
                              hwc2_layer_t layer, buffer_handle_t buffer,
                              int32_t acquire_fence) {
  ALOGV("SetLayerBuffer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  /* A null handle is a fact of this protocol, not a caller's mistake: the
   * command carries a slot, and a null says "the buffer you cached there".
   * The service layer resolves the slot before the call lands here -- but a
   * cache it cannot resolve from, such as a composer freshly restarted under
   * a client that remembers its slots as seeded, hands the null through.
   * The client target's twin of this call has held the same guard all
   * along; this one lost it between the two eras of the entry point, and a
   * first boot found it: one null, one dereference, and the composer and
   * SurfaceFlinger took each other down in a loop. */
  if (buffer == nullptr) {
    return 0;
  }

  auto h2l = GetHwc2DeviceLayer(*ilayer);

  auto lp = h2l->HandleNextBuffer(buffer, acquire_fence,
                                  *idisplay->GetPipe().importer);
  if (!lp) {
    ALOGV("Failed to process layer buffer");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  ilayer->SetLayerProperties(lp.value());

  return 0;
}

static int32_t SetLayerDataspace(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer,
                                 int32_t /*android_dataspace_t*/ dataspace) {
  ALOGV("SetLayerDataspace");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.color_encoding = Hwc2ToColorSpace(dataspace);
  layer_properties.sample_range = Hwc2ToSampleRange(dataspace);
  ilayer->SetLayerProperties(layer_properties);
  return 0;
}

static int32_t SetCursorPosition(hwc2_device_t *device,
                                 hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t x, int32_t y) {
  ALOGV("SetCursorPosition");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);
  (void)ilayer;

  /* The framework only sends these for a layer whose validated type was
   * Cursor, so a call arriving here is a pointer the hardware cursor is
   * already showing -- the position goes straight to it, and no frame is
   * asked for. */
  idisplay->GetPipe().atomic_state_manager->MoveCursor(x, y);
  return 0;
}

static int32_t SetLayerColor(hwc2_device_t * /*device*/,
                             hwc2_display_t /*display*/, hwc2_layer_t /*layer*/,
                             hwc_color_t /*color*/) {
  ALOGV("SetLayerColor");
  return 0;
}

static int32_t SetLayerCompositionType(hwc2_device_t *device,
                                       hwc2_display_t display,
                                       hwc2_layer_t layer,
                                       int32_t /*hwc2_composition_t*/ type) {
  ALOGV("SetLayerCompositionType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  switch (static_cast<HWC2::Composition>(type)) {
    case HWC2::Composition::Client:
      layer_properties.composition_type = CompositionType::kClient;
      break;
    case HWC2::Composition::Device:
      layer_properties.composition_type = CompositionType::kDevice;
      break;
    case HWC2::Composition::SolidColor:
      layer_properties.composition_type = CompositionType::kSolidColor;
      break;
    case HWC2::Composition::Cursor:
      layer_properties.composition_type = CompositionType::kCursor;
      break;
    default:
      ALOGE("Unsupported composition type t=%d", type);
      break;
  }
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerDisplayFrame(hwc2_device_t *device,
                                    hwc2_display_t display, hwc2_layer_t layer,
                                    hwc_rect_t frame) {
  ALOGV("SetLayerDisplayFrame");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  /* The type is named rather than left to be worked out: this release's
   * standard library will not deduce it through an optional from a list of
   * named fields. */
  layer_properties.display_frame = DstRectInfo{
      .i_rect = IRect{.left = frame.left,
                      .top = frame.top,
                      .right = frame.right,
                      .bottom = frame.bottom}};
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerPlaneAlpha(hwc2_device_t *device, hwc2_display_t display,
                                  hwc2_layer_t layer, float alpha) {
  ALOGV("SetLayerPlaneAlpha");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.alpha = alpha;
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerSidebandStream(hwc2_device_t * /*device*/,
                                      hwc2_display_t /*display*/,
                                      hwc2_layer_t /*layer*/,
                                      const native_handle_t * /*stream*/) {
  ALOGV("SetLayerSidebandStream");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t SetLayerSourceCrop(hwc2_device_t *device, hwc2_display_t display,
                                  hwc2_layer_t layer, hwc_frect_t crop) {
  ALOGV("SetLayerSourceCrop");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  /* Named for the same reason as the frame above. */
  layer_properties.source_crop = SrcRectInfo{
      .f_rect = FRect{.left = crop.left,
                      .top = crop.top,
                      .right = crop.right,
                      .bottom = crop.bottom}};
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerSurfaceDamage(hwc2_device_t *device,
                                     hwc2_display_t display, hwc2_layer_t layer,
                                     hwc_region_t damage) {
  ALOGV("SetLayerSurfaceDamage");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties{.damage = DamageInfo{}};
  for (size_t i = 0; i < damage.numRects; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto rect = damage.rects[i];
    layer_properties.damage->dmg_rects.emplace_back(
        IRect{.left = rect.left,
              .top = rect.top,
              .right = rect.right,
              .bottom = rect.bottom});
  }
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerTransform(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer, int32_t transform) {
  ALOGV("SetLayerTransform");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.transform = {
      .hflip = (transform & HAL_TRANSFORM_FLIP_H) != 0,
      .vflip = (transform & HAL_TRANSFORM_FLIP_V) != 0,
      .rotate90 = (transform & HAL_TRANSFORM_ROT_90) != 0,
  };
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerVisibleRegion(hwc2_device_t * /*device*/,
                                     hwc2_display_t /*display*/,
                                     hwc2_layer_t /*layer*/,
                                     hwc_region_t /*visible*/) {
  ALOGV("SetLayerVisibleRegion");
  return 0;
}

static int32_t SetLayerZOrder(hwc2_device_t *device, hwc2_display_t display,
                              hwc2_layer_t layer, uint32_t z) {
  ALOGV("SetLayerZOrder");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.z_order = z;
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

/* Entry point for the HWC2 API */
// NOLINTBEGIN(cppcoreguidelines-pro-type-cstyle-cast)

static hwc2_function_pointer_t HookDevGetFunction(struct hwc2_device * /*dev*/,
                                                  int32_t descriptor) {
  auto func = static_cast<HWC2::FunctionDescriptor>(descriptor);
  switch (func) {
    // Device functions
    case HWC2::FunctionDescriptor::CreateVirtualDisplay:
      return (hwc2_function_pointer_t)CreateVirtualDisplay;
    case HWC2::FunctionDescriptor::DestroyVirtualDisplay:
      return (hwc2_function_pointer_t)DestroyVirtualDisplay;
    case HWC2::FunctionDescriptor::Dump:
      return (hwc2_function_pointer_t)Dump;
    case HWC2::FunctionDescriptor::GetMaxVirtualDisplayCount:
      return (hwc2_function_pointer_t)GetMaxVirtualDisplayCount;
    case HWC2::FunctionDescriptor::RegisterCallback:
      return ToHook<HWC2_PFN_REGISTER_CALLBACK>(RegisterCallbackHook);

    // Display functions
    case HWC2::FunctionDescriptor::AcceptDisplayChanges:
      return (hwc2_function_pointer_t)AcceptDisplayChanges;
    case HWC2::FunctionDescriptor::CreateLayer:
      return (hwc2_function_pointer_t)CreateLayer;
    case HWC2::FunctionDescriptor::DestroyLayer:
      return (hwc2_function_pointer_t)DestroyLayer;
    case HWC2::FunctionDescriptor::GetActiveConfig:
      return (hwc2_function_pointer_t)GetActiveConfig;
    case HWC2::FunctionDescriptor::GetChangedCompositionTypes:
      return (hwc2_function_pointer_t)GetChangedCompositionTypes;
    case HWC2::FunctionDescriptor::GetClientTargetSupport:
      return (hwc2_function_pointer_t)GetClientTargetSupport;
    case HWC2::FunctionDescriptor::GetColorModes:
      return (hwc2_function_pointer_t)GetColorModes;
    case HWC2::FunctionDescriptor::GetDisplayAttribute:
      return (hwc2_function_pointer_t)GetDisplayAttribute;
    case HWC2::FunctionDescriptor::GetDisplayConfigs:
      return (hwc2_function_pointer_t)GetDisplayConfigs;
    case HWC2::FunctionDescriptor::GetDisplayName:
      return (hwc2_function_pointer_t)GetDisplayName;
    case HWC2::FunctionDescriptor::GetDisplayRequests:
      return (hwc2_function_pointer_t)GetDisplayRequests;
    case HWC2::FunctionDescriptor::GetDisplayType:
      return (hwc2_function_pointer_t)GetDisplayType;
    case HWC2::FunctionDescriptor::GetDozeSupport:
      return (hwc2_function_pointer_t)GetDozeSupport;
    case HWC2::FunctionDescriptor::GetHdrCapabilities:
      return (hwc2_function_pointer_t)GetHdrCapabilities;
    case HWC2::FunctionDescriptor::GetReleaseFences:
      return (hwc2_function_pointer_t)GetReleaseFences;
    case HWC2::FunctionDescriptor::PresentDisplay:
      return (hwc2_function_pointer_t)PresentDisplay;
    case HWC2::FunctionDescriptor::SetActiveConfig:
      return (hwc2_function_pointer_t)SetActiveConfig;
    case HWC2::FunctionDescriptor::SetClientTarget:
      return (hwc2_function_pointer_t)SetClientTarget;
    case HWC2::FunctionDescriptor::SetColorMode:
      return (hwc2_function_pointer_t)SetColorMode;
    case HWC2::FunctionDescriptor::SetColorTransform:
      return (hwc2_function_pointer_t)SetColorTransform;
    case HWC2::FunctionDescriptor::SetOutputBuffer:
      return (hwc2_function_pointer_t)SetOutputBuffer;
    case HWC2::FunctionDescriptor::SetPowerMode:
      return (hwc2_function_pointer_t)SetPowerMode;
    case HWC2::FunctionDescriptor::SetVsyncEnabled:
      return (hwc2_function_pointer_t)SetVsyncEnabled;
    case HWC2::FunctionDescriptor::ValidateDisplay:
      return (hwc2_function_pointer_t)ValidateDisplay;
    case HWC2::FunctionDescriptor::GetRenderIntents:
      return (hwc2_function_pointer_t)GetRenderIntents;
    case HWC2::FunctionDescriptor::SetColorModeWithRenderIntent:
      return (hwc2_function_pointer_t)SetColorModeWithRenderIntent;
    case HWC2::FunctionDescriptor::GetDataspaceSaturationMatrix:
      return (hwc2_function_pointer_t)GetDataspaceSaturationMatrix;
    case HWC2::FunctionDescriptor::GetPerFrameMetadataKeys:
      return (hwc2_function_pointer_t)GetPerFrameMetadataKeys;
    case HWC2::FunctionDescriptor::SetLayerPerFrameMetadata:
      return (hwc2_function_pointer_t)SetLayerPerFrameMetadata;
    case HWC2::FunctionDescriptor::SetLayerFloatColor:
      return (hwc2_function_pointer_t)SetLayerFloatColor;
    case HWC2::FunctionDescriptor::SetReadbackBuffer:
      return (hwc2_function_pointer_t)SetReadbackBuffer;
    case HWC2::FunctionDescriptor::GetReadbackBufferAttributes:
      return (hwc2_function_pointer_t)GetReadbackBufferAttributes;
    case HWC2::FunctionDescriptor::GetReadbackBufferFence:
      return (hwc2_function_pointer_t)GetReadbackBufferFence;
    case HWC2::FunctionDescriptor::GetDisplayIdentificationData:
      return (hwc2_function_pointer_t)GetDisplayIdentificationData;
    case HWC2::FunctionDescriptor::GetDisplayCapabilities:
      return (hwc2_function_pointer_t)GetDisplayCapabilities;
    case HWC2::FunctionDescriptor::GetDisplayBrightnessSupport:
      return (hwc2_function_pointer_t)GetDisplayBrightnessSupport;
    case HWC2::FunctionDescriptor::SetDisplayBrightness:
      return (hwc2_function_pointer_t)SetDisplayBrightness;
    case HWC2::FunctionDescriptor::GetDisplayConnectionType:
      return (hwc2_function_pointer_t)GetDisplayConnectionType;
    case HWC2::FunctionDescriptor::GetDisplayVsyncPeriod:
      return (hwc2_function_pointer_t)GetDisplayVsyncPeriod;
    case HWC2::FunctionDescriptor::SetActiveConfigWithConstraints:
      return (hwc2_function_pointer_t)SetActiveConfigWithConstraints;
    case HWC2::FunctionDescriptor::SetAutoLowLatencyMode:
      return (hwc2_function_pointer_t)SetAutoLowLatencyMode;
    case HWC2::FunctionDescriptor::GetSupportedContentTypes:
      return (hwc2_function_pointer_t)GetSupportedContentTypes;
    case HWC2::FunctionDescriptor::SetContentType:
      return (hwc2_function_pointer_t)SetContentType;

    // Layer functions
    case HWC2::FunctionDescriptor::SetCursorPosition:
      return (hwc2_function_pointer_t)SetCursorPosition;
    case HWC2::FunctionDescriptor::SetLayerBlendMode:
      return (hwc2_function_pointer_t)SetLayerBlendMode;
    case HWC2::FunctionDescriptor::SetLayerBuffer:
      return (hwc2_function_pointer_t)SetLayerBuffer;
    case HWC2::FunctionDescriptor::SetLayerColor:
      return (hwc2_function_pointer_t)SetLayerColor;
    case HWC2::FunctionDescriptor::SetLayerCompositionType:
      return (hwc2_function_pointer_t)SetLayerCompositionType;
    case HWC2::FunctionDescriptor::SetLayerDataspace:
      return (hwc2_function_pointer_t)SetLayerDataspace;
    case HWC2::FunctionDescriptor::SetLayerDisplayFrame:
      return (hwc2_function_pointer_t)SetLayerDisplayFrame;
    case HWC2::FunctionDescriptor::SetLayerPlaneAlpha:
      return (hwc2_function_pointer_t)SetLayerPlaneAlpha;
    case HWC2::FunctionDescriptor::SetLayerSidebandStream:
      return (hwc2_function_pointer_t)SetLayerSidebandStream;
    case HWC2::FunctionDescriptor::SetLayerSourceCrop:
      return (hwc2_function_pointer_t)SetLayerSourceCrop;
    case HWC2::FunctionDescriptor::SetLayerSurfaceDamage:
      return (hwc2_function_pointer_t)SetLayerSurfaceDamage;
    case HWC2::FunctionDescriptor::SetLayerTransform:
      return (hwc2_function_pointer_t)SetLayerTransform;
    case HWC2::FunctionDescriptor::SetLayerVisibleRegion:
      return (hwc2_function_pointer_t)SetLayerVisibleRegion;
    case HWC2::FunctionDescriptor::SetLayerZOrder:
      return (hwc2_function_pointer_t)SetLayerZOrder;
    case HWC2::FunctionDescriptor::Invalid:
    default:
      return nullptr;
  }
}

// NOLINTEND(cppcoreguidelines-pro-type-cstyle-cast)

static int HookDevOpen(const struct hw_module_t *module, const char *name,
                       struct hw_device_t **dev) {
  if (strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
    ALOGE("Invalid module name- %s", name);
    return -EINVAL;
  }

  auto ctx = std::make_unique<Drmhwc2Device>();
  if (!ctx) {
    ALOGE("Failed to allocate DrmHwcTwo");
    return -ENOMEM;
  }

  ctx->common.tag = HARDWARE_DEVICE_TAG;
  ctx->common.version = HWC_DEVICE_API_VERSION_2_0;
  ctx->common.close = HookDevClose;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  ctx->common.module = (hw_module_t *)module;
  ctx->getCapabilities = HookDevGetCapabilities;
  ctx->getFunction = HookDevGetFunction;

  *dev = &ctx.release()->common;

  return 0;
}

}  // namespace android::drm_hwcomposer

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static struct hw_module_methods_t hwc2_module_methods = {
    .open = android::drm_hwcomposer::HookDevOpen,
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(2, 0),
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "DrmHwcTwo module",
    .author = "The Android Open Source Project",
    .methods = &hwc2_module_methods,
    .dso = nullptr,
    .reserved = {0},
};
