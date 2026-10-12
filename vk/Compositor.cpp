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

#include "vk/Compositor.h"

#include <android/log.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "vk/Geometry.h"
#include "vk/runtime/loader/VulkanPfn.h"
#include "vk/shaders/compose.frag.spv.h"
#include "vk/shaders/compose.vert.spv.h"

namespace android::hwc::vk {

namespace {

/* HAL_PIXEL_FORMAT_*, spelled here rather than pulled from the graphics
 * headers for four numbers. */
constexpr int32_t kHalRgba8888 = 1;
constexpr int32_t kHalRgbx8888 = 2;
constexpr int32_t kHalRgb565 = 4;
constexpr int32_t kHalBgra8888 = 5;

/* What a gralloc format is read as, and whether its alpha means nothing;
 * VK_FORMAT_UNDEFINED for a format this executor does not read. */
VkFormat FormatOf(int32_t hal, bool *opaque) {
  *opaque = false;
  switch (hal) {
    case kHalRgba8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case kHalRgbx8888:
      *opaque = true;
      return VK_FORMAT_R8G8B8A8_UNORM;
    case kHalBgra8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case kHalRgb565:
      *opaque = true;
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

/* Mirrors the push-constant block of vk/shaders/compose.{vert,frag}. */
struct LayerConstants {
  float ndc[4];
  float uv_tl[2];
  float uv_tr[2];
  float uv_bl[2];
  float pad0[2];
  float color[4];
  float corner[4];
  float radius;
  float alpha;
  int32_t flags;
  float pad1;
};
static_assert(sizeof(LayerConstants) == 96, "the shaders' push-constant block");

constexpr int32_t kFlagSolid = 1;
constexpr int32_t kFlagPremultiplied = 2;
constexpr int32_t kFlagOpaque = 4;

enum LogKind {
  kLogFormat,
  kLogImport,
  kLogTarget,
  kLogBusy,
  kLogAcquire,
  kLogSubmit,
  kLogRelease,
  kLogTooMany,
};

}  // namespace

std::unique_ptr<VkCompositor> VkCompositor::Create(Host *host) {
  std::unique_ptr<VkCompositor> c(new VkCompositor(host));
  std::promise<bool> started;
  auto up = started.get_future();
  c->thread_ = std::thread(&VkCompositor::ThreadMain, c.get(), &started);
  if (!up.get()) {
    /* The thread has already taken down what it brought up and returned. */
    c->thread_.join();
    return nullptr;
  }
  return c;
}

void VkCompositor::ThreadMain(std::promise<bool> *started) {
  pthread_setname_np(pthread_self(), "hwc-vk");
  /* SCHED_FIFO at 2, as RenderEngineThreaded's thread: the frame waits on
   * this thread, which must not queue behind ordinary work. */
  struct sched_param param = {};
  param.sched_priority = 2;
  const bool fifo = sched_setscheduler(0, SCHED_FIFO, &param) == 0;

  const bool up = Init();
  if (up && !fifo && host_)
    host_->Log(ANDROID_LOG_WARN, "vk: could not run at SCHED_FIFO; running as ordinary");
  started->set_value(up);
  if (!up) {
    Teardown();
    return;
  }

  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !work_.empty(); });
      if (work_.empty())
        break; /* stopping, and nothing left */
      work = std::move(work_.front());
      work_.pop_front();
    }
    work();
  }
  Teardown();
}

void VkCompositor::Post(std::function<void()> work) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    work_.push_back(std::move(work));
  }
  wake_.notify_one();
}

int VkCompositor::Compose(const Target &target, const Layer *layers, size_t count) {
  /* Handed over and waited for, as RenderEngineThreaded::drawLayers: the
   * frame needs the fence before it can go up. The layers and fences are
   * borrowed for exactly as long as this call. */
  std::promise<int> done;
  auto fence = done.get_future();
  Post([&] { done.set_value(ComposeNow(target, layers, count)); });
  /* Images the GPU is done with go after the fence is handed back: the
   * last reference to a closed app's buffer is often ours, and freeing it
   * (SMMU unmap, pages back to nvmap's pool) is milliseconds the frame
   * need not wait for. */
  Post([this] { cache_->Retire(retired_); });
  return fence.get();
}

bool VkCompositor::Begin(const Target &target, const Layer *layers, size_t count) {
  if (begun_)
    return false;
  auto begun = std::make_unique<Begun>();
  begun->target = target;
  begun->target.ready_fence = target.ready_fence >= 0 ? dup(target.ready_fence) : -1;
  begun->layers.assign(layers, layers + count);
  for (Layer &l : begun->layers)
    l.acquire_fence = l.acquire_fence >= 0 ? dup(l.acquire_fence) : -1;
  begun->fence = begun->done.get_future();
  begun_ = std::move(begun);

  Begun *b = begun_.get();
  Post([this, b] {
    b->done.set_value(ComposeNow(b->target, b->layers.data(), b->layers.size()));
  });
  Post([this] { cache_->Retire(retired_); });
  return true;
}

int VkCompositor::Finish() {
  if (!begun_)
    return -1;
  const int fence = begun_->fence.get();
  if (begun_->target.ready_fence >= 0)
    close(begun_->target.ready_fence);
  for (const Layer &l : begun_->layers)
    if (l.acquire_fence >= 0)
      close(l.acquire_fence);
  begun_.reset();
  return fence;
}

bool VkCompositor::Warm() {
  Post([this] { WarmNow(); });
  return true;
}

void VkCompositor::Prepare(const Layer &layer) {
  if (layer.buffer == nullptr)
    return;
  /* Copied: the caller does not wait. The handle is the client's buffer,
   * which outlives the frame it came with. */
  Post([this, l = layer] { PrepareNow(l); });
}

void VkCompositor::PrepareNow(const Layer &layer) {
  bool opaque = false;
  const VkFormat f = FormatOf(layer.format, &opaque);
  if (f == VK_FORMAT_UNDEFINED)
    return;
  cache_->Get(layer.buffer, layer.width, layer.height, layer.stride, layer.format, layer.usage,
              f, false, frame_, layer.height_word);
}

void VkCompositor::LogOnce(int kind, int prio, const char *what) {
  if (host_ == nullptr || !logged_.insert(kind).second)
    return;
  host_->Log(prio, what);
}

bool VkCompositor::Init() {
  if (!state_.init()) {
    if (host_)
      host_->Log(ANDROID_LOG_WARN, "vk: Vulkan did not come up; no GPU executor");
    return false;
  }
  if (!state_.nativeBufferAvailable()) {
    if (host_)
      host_->Log(ANDROID_LOG_WARN, "vk: no VK_ANDROID_native_buffer; no zero-copy import");
    return false;
  }
  if (!InitPipeline() || !InitSlots() || !InitDummy() || !InitWarm()) {
    if (host_)
      host_->Log(ANDROID_LOG_ERROR, "vk: the executor's pipeline would not build");
    return false;
  }
  cache_ = std::make_unique<ImageCache>(state_, render_pass_);

  /* The pipeline's first use, paid now rather than in a frame: the driver
   * translates to GL and builds the program on the first draw (0.6 ms off
   * the first frame, measured). The one wait outside a destructor. */
  if (WarmNow())
    state_.pfn()->WaitForFences(state_.device(), 1, &warm_done_, VK_TRUE, UINT64_MAX);
  return true;
}

bool VkCompositor::InitWarm() {
  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();

  VkImageCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {16, 16, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk.CreateImage(dev, &ci, nullptr, &warm_image_) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vk.GetImageMemoryRequirements(dev, warm_image_, &req);
  VkMemoryAllocateInfo mai = {};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      state_.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vk.AllocateMemory(dev, &mai, nullptr, &warm_memory_) != VK_SUCCESS ||
      vk.BindImageMemory(dev, warm_image_, warm_memory_, 0) != VK_SUCCESS)
    return false;
  VkImageViewCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = warm_image_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk.CreateImageView(dev, &vi, nullptr, &warm_view_) != VK_SUCCESS)
    return false;
  VkFramebufferCreateInfo fi = {};
  fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fi.renderPass = render_pass_;
  fi.attachmentCount = 1;
  fi.pAttachments = &warm_view_;
  fi.width = 16;
  fi.height = 16;
  fi.layers = 1;
  if (vk.CreateFramebuffer(dev, &fi, nullptr, &warm_fb_) != VK_SUCCESS)
    return false;

  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi = {};
  dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  if (vk.CreateDescriptorPool(dev, &dpi, nullptr, &warm_pool_) != VK_SUCCESS)
    return false;
  VkDescriptorSetAllocateInfo dai = {};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = warm_pool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set_layout_;
  if (vk.AllocateDescriptorSets(dev, &dai, &warm_set_) != VK_SUCCESS)
    return false;
  VkDescriptorImageInfo info = {sampler_, dummy_view_, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = warm_set_;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &info;
  vk.UpdateDescriptorSets(dev, 1, &w, 0, nullptr);

  VkFenceCreateInfo fci = {};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vk.CreateFence(dev, &fci, nullptr, &warm_done_) != VK_SUCCESS)
    return false;

  /* Recorded once: both paths of the shader -- a sampled layer with
   * corners, then a solid one -- into the small image. */
  VkCommandBufferAllocateInfo cai = {};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = command_pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (vk.AllocateCommandBuffers(dev, &cai, &warm_cmd_) != VK_SUCCESS)
    return false;
  VkCommandBufferBeginInfo bi = {};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (vk.BeginCommandBuffer(warm_cmd_, &bi) != VK_SUCCESS)
    return false;
  VkClearValue clear = {};
  const VkRect2D area = {{0, 0}, {16, 16}};
  VkRenderPassBeginInfo rbi = {};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = render_pass_;
  rbi.framebuffer = warm_fb_;
  rbi.renderArea = area;
  rbi.clearValueCount = 1;
  rbi.pClearValues = &clear;
  vk.CmdBeginRenderPass(warm_cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  vk.CmdBindPipeline(warm_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  const VkViewport vp = {0, 0, 16, 16, 0, 1};
  vk.CmdSetViewport(warm_cmd_, 0, 1, &vp);
  vk.CmdSetScissor(warm_cmd_, 0, 1, &area);
  vk.CmdBindDescriptorSets(warm_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                           &warm_set_, 0, nullptr);
  LayerConstants pc = {};
  pc.ndc[0] = -1;
  pc.ndc[1] = -1;
  pc.ndc[2] = 1;
  pc.ndc[3] = 1;
  pc.uv_tr[0] = 1;
  pc.uv_bl[1] = 1;
  pc.corner[2] = 16;
  pc.corner[3] = 16;
  pc.radius = 4;
  pc.alpha = 1;
  pc.flags = kFlagPremultiplied;
  vk.CmdPushConstants(warm_cmd_, pipeline_layout_,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                      &pc);
  vk.CmdDraw(warm_cmd_, 4, 1, 0, 0);
  pc.flags = kFlagSolid;
  pc.color[3] = 1;
  vk.CmdPushConstants(warm_cmd_, pipeline_layout_,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                      &pc);
  vk.CmdDraw(warm_cmd_, 4, 1, 0, 0);
  vk.CmdEndRenderPass(warm_cmd_);
  return vk.EndCommandBuffer(warm_cmd_) == VK_SUCCESS;
}

bool VkCompositor::WarmNow() {
  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();
  if (warm_busy_) {
    if (vk.WaitForFences(dev, 1, &warm_done_, VK_TRUE, 0) != VK_SUCCESS)
      return false;
    vk.ResetFences(dev, 1, &warm_done_);
    warm_busy_ = false;
  }
  VkSubmitInfo sub = {};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &warm_cmd_;
  if (vk.QueueSubmit(state_.queue(), 1, &sub, warm_done_) != VK_SUCCESS)
    return false;
  warm_busy_ = true;
  return true;
}

bool VkCompositor::InitPipeline() {
  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();

  /* The target is cleared, drawn and stored; what it held before does not
   * matter, the layers cover what they cover and the rest is clear. */
  VkAttachmentDescription att = {};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi = {};
  rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  if (vk.CreateRenderPass(dev, &rpi, nullptr, &render_pass_) != VK_SUCCESS)
    return false;

  /* For drawing part of a target: cleared and drawn only inside the render
   * area, which is all the load and store touch, and begun from the layout
   * the pass above ends in rather than from nothing, which would let the
   * driver drop the rest. Compatible with the first, so the framebuffers
   * and the pipeline made for one serve both. */
  att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (vk.CreateRenderPass(dev, &rpi, nullptr, &keep_pass_) != VK_SUCCESS)
    keep_pass_ = VK_NULL_HANDLE;

  VkSamplerCreateInfo si = {};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vk.CreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS)
    return false;

  VkDescriptorSetLayoutBinding b = {};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dli = {};
  dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dli.bindingCount = 1;
  dli.pBindings = &b;
  if (vk.CreateDescriptorSetLayout(dev, &dli, nullptr, &set_layout_) != VK_SUCCESS)
    return false;

  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(LayerConstants)};
  VkPipelineLayoutCreateInfo pli = {};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &set_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vk.CreatePipelineLayout(dev, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS)
    return false;

  VkShaderModuleCreateInfo smi = {};
  smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  smi.codeSize = sizeof(kComposeVertSpv);
  smi.pCode = kComposeVertSpv;
  if (vk.CreateShaderModule(dev, &smi, nullptr, &vert_) != VK_SUCCESS)
    return false;
  smi.codeSize = sizeof(kComposeFragSpv);
  smi.pCode = kComposeFragSpv;
  if (vk.CreateShaderModule(dev, &smi, nullptr, &frag_) != VK_SUCCESS)
    return false;

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_;

  VkPipelineVertexInputStateCreateInfo vin = {};
  vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia = {};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  /* Viewport and scissor follow the target, which is not always the same
   * size: set per call. */
  VkPipelineViewportStateCreateInfo vps = {};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.scissorCount = 1;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds = {};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkPipelineRasterizationStateCreateInfo rs = {};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms = {};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  /* Premultiplied over: the shader hands out premultiplied colour. */
  VkPipelineColorBlendAttachmentState cba = {};
  cba.blendEnable = VK_TRUE;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.colorBlendOp = VK_BLEND_OP_ADD;
  cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.alphaBlendOp = VK_BLEND_OP_ADD;
  cba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb = {};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;

  VkGraphicsPipelineCreateInfo gpi = {};
  gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpi.stageCount = 2;
  gpi.pStages = stages;
  gpi.pVertexInputState = &vin;
  gpi.pInputAssemblyState = &ia;
  gpi.pViewportState = &vps;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pColorBlendState = &cb;
  gpi.pDynamicState = &ds;
  gpi.layout = pipeline_layout_;
  gpi.renderPass = render_pass_;
  if (vk.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_) !=
      VK_SUCCESS)
    return false;

  cba.blendEnable = VK_FALSE;
  if (vk.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr,
                                 &replace_pipeline_) != VK_SUCCESS)
    replace_pipeline_ = VK_NULL_HANDLE;
  return true;
}

bool VkCompositor::InitSlots() {
  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();

  VkCommandPoolCreateInfo cpi = {};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = state_.queueFamily();
  if (vk.CreateCommandPool(dev, &cpi, nullptr, &command_pool_) != VK_SUCCESS)
    return false;

  for (Slot &s : slots_) {
    VkCommandBufferAllocateInfo cai = {};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = command_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vk.AllocateCommandBuffers(dev, &cai, &s.cmd) != VK_SUCCESS)
      return false;

    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk.CreateFence(dev, &fi, nullptr, &s.done) != VK_SUCCESS)
      return false;

    /* The slot's descriptor sets are allocated once and rewritten each
     * call: one per layer, enough for the most a call takes. */
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxLayers};
    VkDescriptorPoolCreateInfo dpi = {};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = kMaxLayers;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vk.CreateDescriptorPool(dev, &dpi, nullptr, &s.pool) != VK_SUCCESS)
      return false;
    std::array<VkDescriptorSetLayout, kMaxLayers> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo dai = {};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = kMaxLayers;
    dai.pSetLayouts = layouts.data();
    if (vk.AllocateDescriptorSets(dev, &dai, s.sets.data()) != VK_SUCCESS)
      return false;

    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i <= kMaxLayers; ++i) {
      if (vk.CreateSemaphore(dev, &sci, nullptr, &s.acquired[i]) != VK_SUCCESS ||
          vk.CreateSemaphore(dev, &sci, nullptr, &s.drawn[i]) != VK_SUCCESS)
        return false;
    }
  }
  return true;
}

bool VkCompositor::InitDummy() {
  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();

  VkImageCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {1, 1, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk.CreateImage(dev, &ci, nullptr, &dummy_image_) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vk.GetImageMemoryRequirements(dev, dummy_image_, &req);
  VkMemoryAllocateInfo mai = {};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      state_.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vk.AllocateMemory(dev, &mai, nullptr, &dummy_memory_) != VK_SUCCESS ||
      vk.BindImageMemory(dev, dummy_image_, dummy_memory_, 0) != VK_SUCCESS)
    return false;
  VkImageViewCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = dummy_image_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk.CreateImageView(dev, &vi, nullptr, &dummy_view_) != VK_SUCCESS)
    return false;

  /* Into the layout every sampled image here is read in, once, at start;
   * waiting is fine here and nowhere else. */
  VkCommandBuffer cmd = slots_[0].cmd;
  VkCommandBufferBeginInfo bi = {};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vk.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    return false;
  VkImageMemoryBarrier ib = {};
  ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ib.image = dummy_image_;
  ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
  if (vk.EndCommandBuffer(cmd) != VK_SUCCESS)
    return false;
  VkSubmitInfo sub = {};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &cmd;
  if (vk.QueueSubmit(state_.queue(), 1, &sub, VK_NULL_HANDLE) != VK_SUCCESS)
    return false;
  return vk.QueueWaitIdle(state_.queue()) == VK_SUCCESS;
}

VkCompositor::~VkCompositor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  if (thread_.joinable())
    thread_.join();
  /* A drawing begun and never finished: the thread ran it before it
   * stopped, so its fence and the copies Begin took are closed here. */
  if (begun_) {
    const int fence = Finish();
    if (fence >= 0)
      close(fence);
  }
}

/* On the thread, after its last work: everything Vulkan goes down where it
 * came up. */
void VkCompositor::Teardown() {
  if (!state_.isReady())
    return;
  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();
  vk.DeviceWaitIdle(dev);
  cache_.reset();
  for (Slot &s : slots_) {
    for (size_t i = 0; i <= kMaxLayers; ++i) {
      if (s.acquired[i])
        vk.DestroySemaphore(dev, s.acquired[i], nullptr);
      if (s.drawn[i])
        vk.DestroySemaphore(dev, s.drawn[i], nullptr);
    }
    if (s.pool)
      vk.DestroyDescriptorPool(dev, s.pool, nullptr);
    if (s.done)
      vk.DestroyFence(dev, s.done, nullptr);
  }
  if (warm_done_)
    vk.DestroyFence(dev, warm_done_, nullptr);
  if (warm_pool_)
    vk.DestroyDescriptorPool(dev, warm_pool_, nullptr);
  if (warm_fb_)
    vk.DestroyFramebuffer(dev, warm_fb_, nullptr);
  if (warm_view_)
    vk.DestroyImageView(dev, warm_view_, nullptr);
  if (warm_image_)
    vk.DestroyImage(dev, warm_image_, nullptr);
  if (warm_memory_)
    vk.FreeMemory(dev, warm_memory_, nullptr);
  if (command_pool_)
    vk.DestroyCommandPool(dev, command_pool_, nullptr);
  if (dummy_view_)
    vk.DestroyImageView(dev, dummy_view_, nullptr);
  if (dummy_image_)
    vk.DestroyImage(dev, dummy_image_, nullptr);
  if (dummy_memory_)
    vk.FreeMemory(dev, dummy_memory_, nullptr);
  if (pipeline_)
    vk.DestroyPipeline(dev, pipeline_, nullptr);
  if (replace_pipeline_)
    vk.DestroyPipeline(dev, replace_pipeline_, nullptr);
  if (vert_)
    vk.DestroyShaderModule(dev, vert_, nullptr);
  if (frag_)
    vk.DestroyShaderModule(dev, frag_, nullptr);
  if (pipeline_layout_)
    vk.DestroyPipelineLayout(dev, pipeline_layout_, nullptr);
  if (set_layout_)
    vk.DestroyDescriptorSetLayout(dev, set_layout_, nullptr);
  if (sampler_)
    vk.DestroySampler(dev, sampler_, nullptr);
  if (keep_pass_)
    vk.DestroyRenderPass(dev, keep_pass_, nullptr);
  if (render_pass_)
    vk.DestroyRenderPass(dev, render_pass_, nullptr);
  state_.destroy();
}

int VkCompositor::ComposeNow(const Target &target, const Layer *layers, size_t count) {
  if (count == 0 || layers == nullptr || target.buffer == nullptr)
    return -1;
  if (count > kMaxLayers) {
    LogOnce(kLogTooMany, ANDROID_LOG_WARN, "vk: refused a call with more layers than it takes");
    return -1;
  }

  const VulkanPfn &vk = *state_.pfn();
  const VkDevice dev = state_.device();

  /* The next slot, if its last work is done; asked, not waited for. */
  Slot &slot = slots_[next_slot_];
  if (slot.busy) {
    if (vk.WaitForFences(dev, 1, &slot.done, VK_TRUE, 0) != VK_SUCCESS) {
      LogOnce(kLogBusy, ANDROID_LOG_INFO, "vk: a call found every slot still drawing");
      return -1;
    }
    vk.ResetFences(dev, 1, &slot.done);
    slot.busy = false;
    retired_ = std::max(retired_, slot.frame);
  }
  const uint64_t frame = frame_ + 1;

  /* Every image first: nothing is acquired until all of them are known. */
  bool target_opaque = false;
  if (target.format != kHalRgba8888) {
    LogOnce(kLogTarget, ANDROID_LOG_WARN, "vk: a target not in RGBA 8888");
    return -1;
  }
  ImageCache::Entry *dst =
      cache_->Get(target.buffer, target.width, target.height, target.stride, target.format,
                  target.usage, FormatOf(target.format, &target_opaque), true, frame);
  if (dst == nullptr) {
    LogOnce(kLogImport, ANDROID_LOG_WARN, "vk: the driver would not import a buffer");
    return -1;
  }

  std::array<ImageCache::Entry *, kMaxLayers> src{};
  std::array<bool, kMaxLayers> opaque{};
  for (size_t i = 0; i < count; ++i) {
    const Layer &l = layers[i];
    if (l.buffer == nullptr)
      continue;
    const VkFormat f = FormatOf(l.format, &opaque[i]);
    if (f == VK_FORMAT_UNDEFINED) {
      LogOnce(kLogFormat, ANDROID_LOG_INFO, "vk: refused a layer in a format it does not read");
      return -1;
    }
    src[i] = cache_->Get(l.buffer, l.width, l.height, l.stride, l.format, l.usage, f, false,
                         frame, l.height_word);
    if (src[i] == nullptr) {
      LogOnce(kLogImport, ANDROID_LOG_WARN, "vk: the driver would not import a buffer");
      return -1;
    }
  }

  /* One descriptor set per layer, the slot's own, rewritten. */
  const auto &sets = slot.sets;
  std::array<VkDescriptorImageInfo, kMaxLayers> infos{};
  std::array<VkWriteDescriptorSet, kMaxLayers> writes{};
  for (size_t i = 0; i < count; ++i) {
    infos[i] = {sampler_, src[i] ? src[i]->view : dummy_view_, VK_IMAGE_LAYOUT_GENERAL};
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = sets[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &infos[i];
  }
  vk.UpdateDescriptorSets(dev, static_cast<uint32_t>(count), writes.data(), 0, nullptr);

  /* The distinct images of the call: a buffer that appears twice is
   * acquired and released once. Index 0 is the target. */
  std::array<VkImage, kMaxLayers + 1> images{};
  std::array<int, kMaxLayers + 1> fences{};
  size_t n_images = 0;
  images[n_images] = dst->image;
  fences[n_images++] = target.ready_fence;
  for (size_t i = 0; i < count; ++i) {
    if (src[i] == nullptr)
      continue;
    const auto end = images.begin() + n_images;
    if (std::find(images.begin(), end, src[i]->image) != end)
      continue;
    images[n_images] = src[i]->image;
    fences[n_images++] = layers[i].acquire_fence;
  }

  VkCommandBuffer cmd = slot.cmd;
  vk.ResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo bi = {};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk.BeginCommandBuffer(cmd, &bi);

  /* The clients' pictures are read as they are: GENERAL to GENERAL, never
   * from UNDEFINED, which would let the driver drop them. */
  std::array<VkImageMemoryBarrier, kMaxLayers> barriers{};
  for (size_t k = 1; k < n_images; ++k) {
    VkImageMemoryBarrier &ib = barriers[k - 1];
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = images[k];
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  if (n_images > 1)
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                          static_cast<uint32_t>(n_images - 1), barriers.data());

  /* All of the target, or only the part asked for -- when the target holds
   * a picture of ours to keep the rest of. The layers are drawn whole and
   * cut by the scissor, so a member crossing the edge of the part is drawn
   * exactly as it would be in the whole. */
  VkClearValue clear = {};
  VkRect2D area = {{0, 0}, {target.width, target.height}};
  VkRenderPass pass = render_pass_;
  if (target.partial && keep_pass_ != VK_NULL_HANDLE && dst->drawn) {
    const int32_t l = std::max(target.dirty_left, 0);
    const int32_t t = std::max(target.dirty_top, 0);
    const int32_t r = std::min(target.dirty_right, static_cast<int32_t>(target.width));
    const int32_t b = std::min(target.dirty_bottom, static_cast<int32_t>(target.height));
    if (r > l && b > t) {
      area = {{l, t}, {static_cast<uint32_t>(r - l), static_cast<uint32_t>(b - t)}};
      pass = keep_pass_;
    }
  }
  VkRenderPassBeginInfo rbi = {};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = pass;
  rbi.framebuffer = dst->framebuffer;
  rbi.renderArea = area;
  rbi.clearValueCount = 1;
  rbi.pClearValues = &clear;
  vk.CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  VkPipeline bound = pipeline_;
  vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
  const VkViewport vp = {0, 0, static_cast<float>(target.width),
                         static_cast<float>(target.height), 0, 1};
  vk.CmdSetViewport(cmd, 0, 1, &vp);
  vk.CmdSetScissor(cmd, 0, 1, &area);

  /* Where this drawing has put something so far. The area starts cleared,
   * so a layer that lands on none of it blends onto nothing and may as
   * well be written; so may one that hides whatever it lands on. */
  std::array<VkRect2D, kMaxLayers> drawn_rects{};
  size_t drawn = 0;

  for (size_t i = 0; i < count; ++i) {
    const Layer &l = layers[i];
    const int32_t rl = std::max(l.dst_left, area.offset.x);
    const int32_t rt = std::max(l.dst_top, area.offset.y);
    const int32_t rr = std::min(l.dst_right, area.offset.x + static_cast<int32_t>(area.extent.width));
    const int32_t rb = std::min(l.dst_bottom, area.offset.y + static_cast<int32_t>(area.extent.height));
    bool lands_on_drawn = false;
    for (size_t k = 0; k < drawn && !lands_on_drawn; ++k) {
      const VkRect2D &d = drawn_rects[k];
      lands_on_drawn = rl < d.offset.x + static_cast<int32_t>(d.extent.width) &&
                       d.offset.x < rr &&
                       rt < d.offset.y + static_cast<int32_t>(d.extent.height) &&
                       d.offset.y < rb;
    }
    const bool hides = l.alpha >= 1.f && l.corner_radius <= 0.f &&
                       (l.buffer != nullptr ? (l.opaque || opaque[i]) : l.color[3] >= 1.f);
    const VkPipeline want =
        replace_pipeline_ != VK_NULL_HANDLE && (!lands_on_drawn || hides) ? replace_pipeline_
                                                                          : pipeline_;
    if (want != bound) {
      bound = want;
      vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
    }
    if (rr > rl && rb > rt && drawn < drawn_rects.size())
      drawn_rects[drawn++] = {{rl, rt},
                              {static_cast<uint32_t>(rr - rl), static_cast<uint32_t>(rb - rt)}};

    const Quad q = PlaceLayer(l.src_left, l.src_top, l.src_right, l.src_bottom,
                              l.buffer ? l.width : 1, l.buffer ? l.height : 1, l.dst_left,
                              l.dst_top, l.dst_right, l.dst_bottom, l.transform, target.width,
                              target.height);
    LayerConstants pc = {};
    pc.ndc[0] = q.ndc_left;
    pc.ndc[1] = q.ndc_top;
    pc.ndc[2] = q.ndc_right;
    pc.ndc[3] = q.ndc_bottom;
    std::memcpy(pc.uv_tl, q.uv_tl, sizeof(pc.uv_tl));
    std::memcpy(pc.uv_tr, q.uv_tr, sizeof(pc.uv_tr));
    std::memcpy(pc.uv_bl, q.uv_bl, sizeof(pc.uv_bl));
    std::memcpy(pc.color, l.color, sizeof(pc.color));
    pc.corner[0] = l.corner_left;
    pc.corner[1] = l.corner_top;
    pc.corner[2] = l.corner_right;
    pc.corner[3] = l.corner_bottom;
    pc.radius = l.corner_radius;
    pc.alpha = l.alpha;
    pc.flags = (l.buffer == nullptr ? kFlagSolid : 0) | (l.premultiplied ? kFlagPremultiplied : 0) |
               (l.opaque || opaque[i] ? kFlagOpaque : 0);
    vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                             &sets[i], 0, nullptr);
    vk.CmdPushConstants(cmd, pipeline_layout_,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                        &pc);
    vk.CmdDraw(cmd, 4, 1, 0, 0);
  }
  vk.CmdEndRenderPass(cmd);
  vk.EndCommandBuffer(cmd);

  /* Acquire: the GPU waits on each fence through the image's semaphore.
   * The fd is the driver's from here, so it is given a copy. */
  size_t acquired = 0;
  for (; acquired < n_images; ++acquired) {
    const int fd = fences[acquired] >= 0 ? dup(fences[acquired]) : -1;
    if (vk.AcquireImageANDROID(dev, images[acquired], fd, slot.acquired[acquired],
                               VK_NULL_HANDLE) != VK_SUCCESS)
      break;
  }

  VkSubmitInfo sub = {};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  std::array<VkPipelineStageFlags, kMaxLayers + 1> stages;
  stages.fill(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  sub.waitSemaphoreCount = static_cast<uint32_t>(acquired);
  sub.pWaitSemaphores = slot.acquired.data();
  sub.pWaitDstStageMask = stages.data();
  sub.signalSemaphoreCount = static_cast<uint32_t>(acquired);
  sub.pSignalSemaphores = slot.drawn.data();
  /* A refused acquire still has to give back what was acquired: the work
   * goes out with no drawing in it, only the semaphores, and the call
   * refuses. */
  const bool whole = acquired == n_images;
  sub.commandBufferCount = whole ? 1 : 0;
  sub.pCommandBuffers = &cmd;
  if (!whole)
    LogOnce(kLogAcquire, ANDROID_LOG_WARN, "vk: the driver would not acquire a buffer");
  if (vk.QueueSubmit(state_.queue(), 1, &sub, slot.done) != VK_SUCCESS) {
    LogOnce(kLogSubmit, ANDROID_LOG_ERROR, "vk: submit failed");
    return -1;
  }
  slot.busy = true;
  slot.frame = frame;
  frame_ = frame;
  if (whole)
    dst->drawn = true;
  next_slot_ = (next_slot_ + 1) % kSlots;

  /* Each image released on its own drawn semaphore; the target's fence is
   * the caller's, the sources' are covered by it and closed. */
  int out = -1;
  for (size_t k = 0; k < acquired; ++k) {
    int fd = -1;
    if (vk.QueueSignalReleaseImageANDROID(state_.queue(), 1, &slot.drawn[k], images[k], &fd) !=
        VK_SUCCESS) {
      LogOnce(kLogRelease, ANDROID_LOG_ERROR, "vk: release failed");
      continue;
    }
    if (k == 0 && whole)
      out = fd;
    else if (fd >= 0)
      close(fd);
  }
  return out;
}

}  // namespace android::hwc::vk

extern "C" {

uint32_t hwc_vk_api_version() {
  return android::hwc::vk::apiVersion;
}

android::hwc::vk::Compositor *hwc_vk_create(android::hwc::vk::Host *host) {
  return android::hwc::vk::VkCompositor::Create(host).release();
}

void hwc_vk_destroy(android::hwc::vk::Compositor *compositor) {
  delete compositor;
}
}
