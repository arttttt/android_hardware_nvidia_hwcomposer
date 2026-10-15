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

#include "HwcLayer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/PlanInvalidation.h"
#include "utils/Logging.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

HwcLayer::HwcLayer(ICompositorDisplay* parent_display)
    : parent_(parent_display) {
  static std::atomic<uint64_t> next_serial{1};
  layer_data_.serial = next_serial.fetch_add(1, std::memory_order_relaxed);
}

void HwcLayer::SetLayerProperties(const LayerProperties& layer_properties) {
  if (layer_properties.buffer) {
    if (layer_data_.fb != layer_properties.buffer->fb) {
      /* A new buffer is the layer drawing -- the moment the liveness
       * clock restarts from. */
      MarkActivity();
    }

    /* A new buffer invalidates the plan only when it stops looking like the
     * old one -- a different size, format or memory arrangement. The usual
     * case, the next frame of the same surface, changes none of that, and
     * the plan does not care what the pixels are. Judged here, while the
     * buffer being replaced is still around to compare against. */
    {
      const BufferInfo& incoming = layer_properties.buffer->bi;
      /* An import that failed where the last one succeeded (or healed
       * where it failed) changes the layer's seat as surely as a new
       * shape: usability is judged from the import, and a stale plan
       * would seat an unusable layer on a window. */
      if (!layer_data_.bi ||
          layer_data_.bi->width != incoming.width ||
          layer_data_.bi->height != incoming.height ||
          layer_data_.bi->format != incoming.format ||
          layer_data_.bi->modifiers[0] != incoming.modifiers[0] ||
          (layer_data_.fb == nullptr) !=
              (layer_properties.buffer->fb == nullptr)) {
        plan_invalidators_ |= kBufferGeometry;
      }
    }

    /* A new buffer has arrived, so the one before it is about to stop being
     * needed -- and only now is there anything to release. Whether the client
     * must wait for that release depends on where the previous buffer went:
     * if the display controller was given it, it is still being read out of
     * memory and the client cannot draw over it yet.
     *
     * The type read here is the one the last validation settled on, which is
     * what this layer did with its PREVIOUS buffer -- the client sets buffers
     * before asking for a new validation.
     */
    HWC_LOGD("layer %p: buffer %p replaces %p%s", this,
             layer_properties.buffer->bi.handle,
             layer_data_.bi ? layer_data_.bi->handle : nullptr,
             prior_buffer_scanout_flag_ ? ", before the last one was released"
                                        : "");
    prior_buffer_scanout_flag_ = validated_type_ != CompositionType::kClient;

    /* Which buffer that was, taken before it is written over. Not every layer
     * given to the display is read by it -- some are drawn into a buffer of
     * the composer's own by a separate engine first, and those stop being read
     * as soon as that engine is done rather than when the frame is shown. The
     * two are told apart afterwards by recognising this. */
    prior_buffer_ = layer_data_.bi ? layer_data_.bi->handle : nullptr;

    has_buffer_set_ = true;
    layer_data_.bi = layer_properties.buffer->bi;
    layer_data_.fb = layer_properties.buffer->fb;
    layer_data_.acquire_fence = layer_properties.buffer->fence;
    layer_data_.generation++;
  }
  if (layer_properties.blend_mode) {
    if (blend_mode_ != layer_properties.blend_mode.value()) {
      plan_invalidators_ |= kBlendMode;
    }
    blend_mode_ = layer_properties.blend_mode.value();
  }
  if (layer_properties.colorspace) {
    if (colorspace_ != layer_properties.colorspace.value()) {
      plan_invalidators_ |= kDataspace;
    }
    colorspace_ = layer_properties.colorspace.value();
  }
  if (layer_properties.color_encoding) {
    if (color_encoding_ != layer_properties.color_encoding.value()) {
      plan_invalidators_ |= kDataspace;
    }
    color_encoding_ = layer_properties.color_encoding.value();
  }
  if (layer_properties.sample_range) {
    if (sample_range_ != layer_properties.sample_range.value()) {
      plan_invalidators_ |= kDataspace;
    }
    sample_range_ = layer_properties.sample_range.value();
  }
  if (layer_properties.transfer_func) {
    if (transfer_func_ != layer_properties.transfer_func.value()) {
      plan_invalidators_ |= kDataspace;
    }
    transfer_func_ = layer_properties.transfer_func.value();
  }
  if (layer_properties.composition_type) {
    /* Only the framework's own asks come through here; the composer settling
     * a layer to what validation decided goes through AcceptTypeChange and
     * raises nothing -- that settling is the plan, not a change to it. */
    if (sf_type_ != layer_properties.composition_type.value()) {
      plan_invalidators_ |= kCompositionType;
    }
    sf_type_ = layer_properties.composition_type.value();
  }
  if (layer_properties.display_frame) {
    if (sf_display_frame_ != layer_properties.display_frame.value()) {
      plan_invalidators_ |= kDisplayFrame;
      /* Movement without redrawing -- a window dragged, not repainted --
       * is a live layer that never posts a buffer. */
      MarkActivity();
    }
    sf_display_frame_ = layer_properties.display_frame.value();
    layer_data_.pi.display_frame = sf_display_frame_;
  }
  if (layer_properties.alpha) {
    if (layer_data_.pi.alpha != layer_properties.alpha.value()) {
      plan_invalidators_ |= kPlaneAlpha;
      /* A fade is a live layer that never posts a buffer and never
       * moves -- the alpha is the only thing drawing. */
      MarkActivity();
    }
    layer_data_.pi.alpha = layer_properties.alpha.value();
  }
  if (layer_properties.source_crop) {
    if (sf_source_crop_ != layer_properties.source_crop.value()) {
      plan_invalidators_ |= kSourceCrop;
      MarkActivity();
    }
    sf_source_crop_ = layer_properties.source_crop.value();
    layer_data_.pi.source_crop = sf_source_crop_;
  }
  if (layer_properties.transform) {
    const LayerTransform& incoming = layer_properties.transform.value();
    const LayerTransform& current = layer_data_.pi.transform;
    if (current.hflip != incoming.hflip || current.vflip != incoming.vflip ||
        current.rotate90 != incoming.rotate90) {
      plan_invalidators_ |= kTransform;
      MarkActivity();
    }
    layer_data_.pi.transform = incoming;
  }
  if (layer_properties.z_order) {
    if (z_order_ != layer_properties.z_order.value()) {
      plan_invalidators_ |= kZOrder;
    }
    z_order_ = layer_properties.z_order.value();
  }
  if (layer_properties.damage) {
    layer_data_.pi.damage = layer_properties.damage.value();
  }
  if (layer_properties.brightness) {
    brightness_ = layer_properties.brightness;
  }

  if (has_buffer_set_) {
    PopulateLayerData();
  }
}

void HwcLayer::PopulateLayerData() {
  if (!layer_data_.bi) {
    ALOGE("Internal error: PopulateLayerData called without valid bi.");
    return;
  }

  if (blend_mode_ != BufferBlendMode::kUndefined) {
    layer_data_.bi->blend_mode = blend_mode_;
  }
  if (colorspace_ != HwcColorspace::kDefault) {
    layer_data_.colorspace = colorspace_;
  }
  if (color_encoding_ != BufferColorEncoding::kUndefined) {
    layer_data_.bi->color_encoding = color_encoding_;
  }
  if (sample_range_ != BufferSampleRange::kUndefined) {
    layer_data_.bi->sample_range = sample_range_;
  }
  if (transfer_func_ != TransferFunction::kUnknown) {
    layer_data_.transfer_func = transfer_func_;
  }
  if (brightness_ >= 0.F) {
    layer_data_.brightness = brightness_;
  }
}

EffectResult HwcLayer::ApplyEffectMetadata(const char *key, size_t key_length,
                                           const uint8_t *value,
                                           size_t value_length) {
  const bool had = layer_data_.effects.Any();
  const EffectResult result = drm_hwcomposer::ApplyEffectMetadata(
      key, key_length, value, value_length, &layer_data_.effects);
  if (result == EffectResult::kApplied && had != layer_data_.effects.Any()) {
    plan_invalidators_ |= kEffects;
  }
  return result;
}

void HwcLayer::SetSolidColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const std::array<float, 4> c = {static_cast<float>(r) / 255.F,
                                  static_cast<float>(g) / 255.F,
                                  static_cast<float>(b) / 255.F,
                                  static_cast<float>(a) / 255.F};
  if (!layer_data_.solid_color) {
    plan_invalidators_ |= kEffects;
  }
  layer_data_.solid_color = c;
}

bool HwcLayer::RefreshLiveness() const {
  if (!LivenessWouldChange()) {
    return false;
  }
  live_ = !live_;
  return true;
}

void HwcLayer::MarkActivity() {
  const auto now = std::chrono::steady_clock::now();
  /* One frame may announce itself several times -- a new buffer and a new
   * crop arrive in the same transaction -- and that is one activity, not
   * the two a stream is told by. */
  if (now - last_activity_ >= kSameFrame)
    previous_activity_ = last_activity_;
  last_activity_ = now;
}

bool HwcLayer::IsSmall() const {
  const auto size = parent_->GetSize();
  const uint64_t panel = static_cast<uint64_t>(size.first) * size.second;
  return static_cast<uint64_t>(GetPixOps()) * kSmallShare <= panel;
}

bool HwcLayer::LivenessWouldChange() const {
  const bool recent = std::chrono::steady_clock::now() - last_activity_ <
                      kLayerQuietDelay;
  /* A small layer that draws once is not yet drawing: a status bar's clock
   * or battery, an icon that changed. Taken out of the merge for that one
   * frame, it would take a still screen apart -- every window lit again,
   * the memory clock raised for the scan -- and a second later the screen
   * would be merged whole again, every pixel of it redrawn, all for a
   * strip the merge redraws in part for a fraction of that. So a small
   * quiet layer turns live on its second frame within the quiet delay,
   * when it is a stream. A large one turns live on its first, as before:
   * that is an application starting to move, and its first frame is not
   * to wait on the engine. */
  const bool live =
      recent && (live_ || !IsSmall() ||
                 last_activity_ - previous_activity_ < kLayerQuietDelay);
  return live != live_;
}


/* Check that the layer has an active slot set, and there is a valid
   * framebuffer in the active slot.
 */
bool HwcLayer::IsLayerUsableAsDevice() const {
  if (!has_buffer_set_) {
    return false;
  }
  return layer_data_.fb != nullptr;
}

uint32_t HwcLayer::GetPixOps() const {
  const auto& df = GetLayerData().pi.display_frame;
  if (df.i_rect.has_value()) {
    return df.i_rect->Width() * df.i_rect->Height();
  }
  return parent_->GetSize().first * parent_->GetSize().second;
}

void HwcLayer::SetVisibleRegion(std::vector<IRect> region) {
  if (region.size() != sf_visible_.size() ||
      !std::equal(region.begin(), region.end(), sf_visible_.begin()))
    plan_invalidators_ |= kDisplayFrame;
  sf_visible_ = std::move(region);
}

occlusion::Layer HwcLayer::OcclusionInput(const IRect &screen) const {
  occlusion::Layer l;
  l.frame = sf_display_frame_.i_rect ? *sf_display_frame_.i_rect : screen;
  l.visible = sf_visible_;

  /* Opaque as the framework says it: a buffer it blends with nothing, or a
   * solid colour with no alpha in it -- either at full plane alpha. */
  const bool full_alpha = layer_data_.pi.alpha >= kAlphaOpaque;
  if (layer_data_.bi)
    l.opaque = full_alpha &&
               layer_data_.bi->blend_mode == BufferBlendMode::kNone;
  else if (layer_data_.solid_color)
    l.opaque = full_alpha && (*layer_data_.solid_color)[3] >= 1.F;

  const auto &corners = layer_data_.effects.corners;
  if (layer_data_.effects.HasCorners()) {
    l.corner_crop = IRect{static_cast<int32_t>(std::floor(corners[0])),
                          static_cast<int32_t>(std::floor(corners[1])),
                          static_cast<int32_t>(std::ceil(corners[2])),
                          static_cast<int32_t>(std::ceil(corners[3]))};
    l.radius = corners[4];
  }

  /* What reaches past the frame is never cut down to a part of it, and
   * the pointer is not the occlusion's to move. */
  l.keep = sf_type_ == CompositionType::kCursor ||
           layer_data_.effects.ReachesPastFrame();
  return l;
}

void HwcLayer::ApplyOcclusion(const occlusion::Shown &shown) {
  DstRectInfo frame = sf_display_frame_;
  SrcRectInfo crop = sf_source_crop_;

  if (!shown.hidden && frame.i_rect && shown.bounds != *frame.i_rect) {
    /* A missing crop is the whole buffer; a layer with no buffer has no
     * source to narrow, and only its frame moves. */
    std::optional<FRect> source = crop.f_rect;
    if (!source && layer_data_.bi)
      source = FRect{0.F, 0.F, static_cast<float>(layer_data_.bi->width),
                     static_cast<float>(layer_data_.bi->height)};
    const auto &t = layer_data_.pi.transform;
    if (source) {
      const auto narrowed = occlusion::CropSource(*source, *frame.i_rect,
                                                  shown.bounds, t.hflip,
                                                  t.vflip, t.rotate90);
      if (narrowed) {
        frame.i_rect = shown.bounds;
        crop.f_rect = narrowed;
      }
    } else if (!t.rotate90) {
      frame.i_rect = shown.bounds;
    }
  }

  /* A change in what is shown is a change in the plan, as SurfaceFlinger
   * moving the layer would be. */
  if (frame != layer_data_.pi.display_frame || hidden_ != shown.hidden)
    plan_invalidators_ |= kDisplayFrame;
  if (crop != layer_data_.pi.source_crop)
    plan_invalidators_ |= kSourceCrop;
  layer_data_.pi.display_frame = frame;
  layer_data_.pi.source_crop = crop;
  hidden_ = shown.hidden;
}

}  // namespace android::drm_hwcomposer
