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
#include <future>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "vk/HwcVk.h"
#include "vk/ImageCache.h"
#include "vk/runtime/VulkanDeviceState.h"

namespace android::hwc::vk {

/* The executor: one render pass into the target, one draw per layer,
 * blended premultiplied-over.
 *
 * Everything Vulkan runs on the executor's own thread, in the order it was
 * handed over -- RenderEngineThreaded's model -- so nothing below the
 * public calls needs a lock. It never waits for the GPU: work goes out in one of three
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
  bool Begin(const Target &target, const Layer *layers, size_t count) override;
  int Finish() override;
  bool Warm() override;
  void Prepare(const Layer &layer) override;

 private:
  explicit VkCompositor(Host *host) : host_(host) {}

  /* The thread: brings Vulkan up, runs what is handed over, takes Vulkan
   * down. Post queues work; the public calls are built on it. */
  void ThreadMain(std::promise<bool> *started);
  void Post(std::function<void()> work);
  void Teardown();

  /* The work itself, on the thread. */
  int ComposeNow(const Target &target, const Layer *layers, size_t count);
  bool WarmNow();
  void PrepareNow(const Layer &layer);

  bool Init();
  bool InitPipeline();
  bool InitDummy();
  bool InitSlots();
  bool InitWarm();

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

  /* Warm(): a small image of its own with its framebuffer, a command
   * buffer recorded once and a fence saying whether the last one is done. */
  VkImage warm_image_ = VK_NULL_HANDLE;
  VkDeviceMemory warm_memory_ = VK_NULL_HANDLE;
  VkImageView warm_view_ = VK_NULL_HANDLE;
  VkFramebuffer warm_fb_ = VK_NULL_HANDLE;
  VkCommandBuffer warm_cmd_ = VK_NULL_HANDLE;
  VkDescriptorPool warm_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet warm_set_ = VK_NULL_HANDLE;
  VkFence warm_done_ = VK_NULL_HANDLE;
  bool warm_busy_ = false;

  std::array<Slot, kSlots> slots_{};
  size_t next_slot_ = 0;
  uint64_t frame_ = 0;
  uint64_t retired_ = 0;

  std::set<int> logged_;

  /* What Begin was handed, copied, with fences of its own -- read by the
   * thread until the drawing is sent, then by Finish to close them. */
  struct Begun {
    Target target;
    std::vector<Layer> layers;
    std::promise<int> done;
    std::future<int> fence;
  };
  std::unique_ptr<Begun> begun_;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> work_;
  bool stopping_ = false;
};

}  // namespace android::hwc::vk
