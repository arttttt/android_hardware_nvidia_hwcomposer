/*
 * Copyright (C) 2015 - 2023 The Android Open Source Project
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

/* Adapted from drm-hwcomposer's drm/DrmDisplayPipeline.h.
 *
 * The two templates below say who a piece of display hardware currently
 * belongs to, and they say it without knowing what that piece is or what a
 * display is driven through -- which is why they are here rather than in the
 * file they were written in, whose other half is entirely about DRM.
 *
 * The only change is the type of the thing bound to: a pipeline as this
 * composer describes one, rather than a DRM pipeline.
 */

#pragma once

#include <memory>
#include <utility>
#include <vector>

namespace android::drm_hwcomposer {

struct DisplayPipeline;

template <class O>
class BindingOwner;

template <class O>
class PipelineBindable {
  friend class BindingOwner<O>;

 public:

  // Header implementation required for template instantiation.
  auto BindPipeline(const DisplayPipeline *pipeline,
                    bool return_object_if_bound = false)
      -> std::shared_ptr<BindingOwner<O>> {
    auto owner_object = owner_object_.lock();
    if (owner_object) {
      if (bound_pipeline_ == pipeline && return_object_if_bound) {
        return owner_object;
      }

      return {};
    }
    owner_object = std::make_shared<BindingOwner<O>>(static_cast<O *>(this));

    owner_object_ = owner_object;
    bound_pipeline_ = pipeline;
    return owner_object;
  }

 private:
  const DisplayPipeline *bound_pipeline_{};
  std::weak_ptr<BindingOwner<O>> owner_object_;
};

class Plane;

/* What a display is given when it asks which planes are its: the ones it may
 * put layers on, and the one meant for a cursor if there is one. */
using UsablePlanes = std::pair<std::vector<std::shared_ptr<BindingOwner<Plane>>>,
                               std::shared_ptr<BindingOwner<Plane>>>;

template <class B>
class BindingOwner {
 public:
  explicit BindingOwner(B *pb) : bindable_(pb){};
  ~BindingOwner() {
    bindable_->bound_pipeline_ = nullptr;
  }

  B *Get() {
    return bindable_;
  }

 private:
  B *const bindable_;
};

}  // namespace android::drm_hwcomposer
