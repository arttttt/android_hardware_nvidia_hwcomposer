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

#include <cstdint>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/PlanInvalidation.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

HwcLayer::HwcLayer(ICompositorDisplay* parent_display)
    : parent_(parent_display) {
}

void HwcLayer::SetLayerProperties(const LayerProperties& layer_properties) {
  if (layer_properties.buffer) {
    if (layer_data_.fb != layer_properties.buffer->fb) {
      layer_data_.frame_time_history.AddFrameTime();
      /* A new buffer is the layer drawing -- the moment the liveness
       * clock restarts from. */
      last_activity_ = std::chrono::steady_clock::now();
    }

    /* A new buffer invalidates the plan only when it stops looking like the
     * old one -- a different size, format or memory arrangement. The usual
     * case, the next frame of the same surface, changes none of that, and
     * the plan does not care what the pixels are. Judged here, while the
     * buffer being replaced is still around to compare against. */
    {
      const BufferInfo& incoming = layer_properties.buffer->bi;
      if (!layer_data_.bi ||
          layer_data_.bi->width != incoming.width ||
          layer_data_.bi->height != incoming.height ||
          layer_data_.bi->format != incoming.format ||
          layer_data_.bi->modifiers[0] != incoming.modifiers[0]) {
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
    if (layer_data_.pi.display_frame != layer_properties.display_frame
                                            .value()) {
      plan_invalidators_ |= kDisplayFrame;
      /* Movement without redrawing -- a window dragged, not repainted --
       * is a live layer that never posts a buffer. */
      last_activity_ = std::chrono::steady_clock::now();
    }
    layer_data_.pi.display_frame = layer_properties.display_frame.value();
  }
  if (layer_properties.alpha) {
    if (layer_data_.pi.alpha != layer_properties.alpha.value()) {
      plan_invalidators_ |= kPlaneAlpha;
      /* A fade is a live layer that never posts a buffer and never
       * moves -- the alpha is the only thing drawing. */
      last_activity_ = std::chrono::steady_clock::now();
    }
    layer_data_.pi.alpha = layer_properties.alpha.value();
  }
  if (layer_properties.source_crop) {
    if (layer_data_.pi.source_crop != layer_properties.source_crop.value()) {
      plan_invalidators_ |= kSourceCrop;
      last_activity_ = std::chrono::steady_clock::now();
    }
    layer_data_.pi.source_crop = layer_properties.source_crop.value();
  }
  if (layer_properties.transform) {
    const LayerTransform& incoming = layer_properties.transform.value();
    const LayerTransform& current = layer_data_.pi.transform;
    if (current.hflip != incoming.hflip || current.vflip != incoming.vflip ||
        current.rotate90 != incoming.rotate90) {
      plan_invalidators_ |= kTransform;
      last_activity_ = std::chrono::steady_clock::now();
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

bool HwcLayer::RefreshLiveness() const {
  const bool live = std::chrono::steady_clock::now() - last_activity_ <
                    kLayerQuietDelay;
  if (live == live_) {
    return false;
  }
  live_ = live;
  return true;
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

}  // namespace android::drm_hwcomposer
