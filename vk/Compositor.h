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

#include <array>
#include <memory>
#include <set>
#include <vector>

#include "vk/HwcVk.h"
#include "vk/ImageCache.h"
#include "vk/runtime/VulkanDeviceState.h"

namespace android::hwc::vk {

/* The executor: one render pass into the target, one draw per layer,
 * blended premultiplied-over.
 *
 * Called from the composer's commit thread and from nowhere else, so it
 * keeps no lock. It never waits for the GPU: work goes out in one of three
 * slots, and a slot is reused only once its fence has come due -- a call
 * that finds the next slot still busy refuses, and the composer takes
 * another way for that frame. The sources' acquire fences and the target's
 * ready fence are waited on by the GPU through vkAcquireImageANDROID, and
 * each image is released on its own semaphore signalled by the drawing,
 * so the fence handed back comes due when the drawing is done.
 */
class VkCompositor : public Compositor {
 public:
  static std::unique_ptr<VkCompositor> Create(Host *host);
  ~VkCompositor() override;

  int Compose(const Target &target, const Layer *layers, size_t count) override;

 private:
  explicit VkCompositor(Host *host) : host_(host) {}

  bool Init();
  bool InitPipeline();
  bool InitDummy();
  bool InitSlots();

  /* Says `what` once per kind, never per frame. */
  void LogOnce(int kind, int prio, const char *what);

  static constexpr size_t kSlots = 3;
  /* Layers in one call: more than any scene the planner would send. */
  static constexpr size_t kMaxLayers = 16;

  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    /* One per layer, allocated once and rewritten each call. */
    std::array<VkDescriptorSet, kMaxLayers> sets{};
    /* One acquired and one drawn semaphore per image of the call: the
     * target and up to kMaxLayers sources. */
    std::array<VkSemaphore, kMaxLayers + 1> acquired{};
    std::array<VkSemaphore, kMaxLayers + 1> drawn{};
    uint64_t frame = 0;
    bool busy = false;
  };

  Host *const host_;
  VulkanDeviceState state_;
  std::unique_ptr<ImageCache> cache_;

  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkShaderModule vert_ = VK_NULL_HANDLE;
  VkShaderModule frag_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;

  /* Bound for a solid colour, which samples nothing but still needs the
   * binding the pipeline declares. */
  VkImage dummy_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_memory_ = VK_NULL_HANDLE;
  VkImageView dummy_view_ = VK_NULL_HANDLE;

  std::array<Slot, kSlots> slots_{};
  size_t next_slot_ = 0;
  uint64_t frame_ = 0;
  uint64_t retired_ = 0;

  std::set<int> logged_;
};

}  // namespace android::hwc::vk
