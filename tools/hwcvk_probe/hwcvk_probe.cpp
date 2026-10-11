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

/* Probe: can the composer compose on the GPU itself, with no copy?
 *
 * The question the composer's Vulkan executor stands on. Writing into a
 * gralloc buffer through VK_ANDROID_native_buffer is known to work on this
 * driver -- the camera does it, through the fragment stage only. Reading
 * one as a texture never was tried: the camera samples its own memory.
 * The executor needs both, on buffers made the way SurfaceFlinger's
 * clients make them, so this walks exactly that path once:
 *
 *   1. a source the size of the panel, allocated with an application
 *      layer's usage (render | texture | composer), four quadrants of
 *      known colour written through gralloc's lock;
 *   2. a target with a scanout buffer's usage (render | composer);
 *   3. both imported with VkNativeBufferANDROID, the source SAMPLED, the
 *      target a COLOR_ATTACHMENT, acquired through vkAcquireImageANDROID;
 *   4. one full-screen draw: the source sampled, cut by a rounded-corner
 *      mask, premultiplied into the cleared target;
 *   5. released through vkQueueSignalReleaseImageANDROID, its sync fd
 *      waited on, and the target read back through gralloc's lock.
 *
 * PASS means the corners are clear and every quadrant came through; the
 * time is the draw's, fence to fence, over twenty runs.
 *
 *   mmm hardware/nvidia/hwcomposer/tools/hwcvk_probe
 *   adb push .../system/bin/hwcvk_probe /data/local/tmp/ && \
 *       adb shell /data/local/tmp/hwcvk_probe
 */

#define LOG_TAG "hwcvk_probe"

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include <hardware/gralloc.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>

#include <vulkan/vulkan.h>

#include "vk/runtime/VulkanDeviceState.h"
#include "vk/runtime/loader/VulkanPfn.h"

/* SPIR-V, compiled from shaders/probe.{vert,frag} with glslang 11:
 *   glslangValidator -V --target-env vulkan1.0 --vn kProbeVertSpv \
 *       -o shaders/probe.vert.spv.h shaders/probe.vert
 * and the same for the fragment stage. */
#include "shaders/probe.frag.spv.h"
#include "shaders/probe.vert.spv.h"

using namespace android;

/* VK_ANDROID_native_buffer, declared by hand: the platform header keeps it
 * for the loader's own use. */
#define VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID_PROBE ((VkStructureType)1000010000)
struct NativeBufferProbe {
  VkStructureType sType;
  const void *pNext;
  const native_handle_t *handle;
  int stride;
  int format;
  int usage;
};
#define P(...)                    \
  do {                            \
    fprintf(stderr, __VA_ARGS__); \
    ALOGI(__VA_ARGS__);           \
  } while (0)
#define FAIL(...)                       \
  do {                                  \
    fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fprintf(stderr, "\n");              \
    return 1;                           \
  } while (0)
#define VK(call)                                     \
  do {                                               \
    VkResult r_ = (call);                            \
    if (r_ != VK_SUCCESS) FAIL("%s = %d", #call, r_); \
  } while (0)

namespace {

constexpr uint32_t kW = 1536;
constexpr uint32_t kH = 2048;
constexpr float kRadius = 128.f;

/* Opaque RGBA as gralloc stores RGBA_8888: R in the lowest byte. */
constexpr uint32_t kRed = 0xff0000ff;
constexpr uint32_t kGreen = 0xff00ff00;
constexpr uint32_t kBlue = 0xffff0000;
constexpr uint32_t kWhite = 0xffffffff;

uint32_t QuadrantColour(uint32_t x, uint32_t y) {
  const bool right = x >= kW / 2;
  const bool bottom = y >= kH / 2;
  return bottom ? (right ? kWhite : kBlue) : (right ? kGreen : kRed);
}

const char kVertex[] =
    "#version 450\n"
    "void main() {\n"
    "    float x = float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0;\n"
    "    float y = float(gl_VertexIndex & 2) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "}\n";

/* Sample the source at the fragment, cut the corners by distance from the
 * corner's centre, premultiply. */
const char kFragment[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform PC { vec2 size; float radius; } pc;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() {\n"
    "    vec2 p = gl_FragCoord.xy;\n"
    "    vec4 c = texture(src, p / pc.size);\n"
    "    vec2 q = abs(p - pc.size * 0.5) - (pc.size * 0.5 - vec2(pc.radius));\n"
    "    float d = length(max(q, vec2(0.0))) - pc.radius;\n"
    "    float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "    outColor = vec4(c.rgb * c.a * a, c.a * a);\n"
    "}\n";

double NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

bool WaitFd(int fd) {
  pollfd p = {fd, POLLIN, 0};
  return poll(&p, 1, 2000) == 1;
}

}  // namespace

int main(int argc, char **argv) {
  /* SPIR-V by default; "glsl" takes the NV GLSL extension instead, to
   * compare the two on the same driver. */
  const bool glsl = argc > 1 && !strcmp(argv[1], "glsl");
  P("shaders: %s\n", glsl ? "GLSL via VK_NV_glsl_shader" : "SPIR-V");
  /* The camera's runtime, unchanged: the loader picks the HAL module or
   * the system libvulkan by what the driver offers, and the device comes
   * up with the native-buffer, GLSL and external-sync extensions. */
  VulkanDeviceState state;
  if (!state.init()) FAIL("Vulkan device did not come up");
  if (!state.nativeBufferAvailable()) FAIL("VK_ANDROID_native_buffer absent: no zero-copy import");
  const VulkanPfn &vk = *state.pfn();
  const VkDevice dev = state.device();
  const uint32_t qfi = state.queueFamily();
  P("device up, queue family %u\n", qfi);

  const VkQueue queue = state.queue();

  /* --- the buffers, made the way a client's and a scanout's are. --- */
  const uint64_t src_usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
                             GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_WRITE_OFTEN;
  const uint64_t dst_usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER |
                             GRALLOC_USAGE_SW_READ_OFTEN;
  sp<GraphicBuffer> src = new GraphicBuffer(kW, kH, PIXEL_FORMAT_RGBA_8888, 1, src_usage,
                                            "hwcvk_probe src");
  sp<GraphicBuffer> dst = new GraphicBuffer(kW, kH, PIXEL_FORMAT_RGBA_8888, 1, dst_usage,
                                            "hwcvk_probe dst");
  if (src->initCheck() != NO_ERROR || dst->initCheck() != NO_ERROR) FAIL("gralloc alloc");
  P("src stride %u, dst stride %u\n", src->getStride(), dst->getStride());

  {
    void *p = nullptr;
    if (src->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &p) != NO_ERROR || !p) FAIL("lock src");
    auto *px = static_cast<uint32_t *>(p);
    for (uint32_t y = 0; y < kH; y++)
      for (uint32_t x = 0; x < kW; x++) px[y * src->getStride() + x] = QuadrantColour(x, y);
    src->unlock();
  }

  auto import = [&](const sp<GraphicBuffer> &gb, VkImageUsageFlags usage, VkImage *img,
                    VkImageView *view) -> VkResult {
    NativeBufferProbe nb = {};
    nb.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID_PROBE;
    nb.handle = gb->handle;
    nb.stride = static_cast<int>(gb->getStride());
    nb.format = gb->getPixelFormat();
    nb.usage = static_cast<int>(gb->getUsage());
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &nb;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kW, kH, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vk.CreateImage(dev, &ci, nullptr, img);
    if (r != VK_SUCCESS) return r;
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vk.CreateImageView(dev, &vi, nullptr, view);
  };
  VkImage src_img, dst_img;
  VkImageView src_view, dst_view;
  {
    VkResult r = import(src, VK_IMAGE_USAGE_SAMPLED_BIT, &src_img, &src_view);
    P("import source as SAMPLED: %d\n", r);
    if (r != VK_SUCCESS) FAIL("a client's buffer cannot be sampled");
    r = import(dst, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &dst_img, &dst_view);
    P("import target as COLOR_ATTACHMENT: %d\n", r);
    if (r != VK_SUCCESS) FAIL("a scanout buffer cannot be drawn into");
  }

  /* --- the pipeline: one pass, one draw. --- */
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
  VkRenderPass rp;
  VK(vk.CreateRenderPass(dev, &rpi, nullptr, &rp));

  VkFramebufferCreateInfo fbi = {};
  fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbi.renderPass = rp;
  fbi.attachmentCount = 1;
  fbi.pAttachments = &dst_view;
  fbi.width = kW;
  fbi.height = kH;
  fbi.layers = 1;
  VkFramebuffer fb;
  VK(vk.CreateFramebuffer(dev, &fbi, nullptr, &fb));

  VkSamplerCreateInfo si = {};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler;
  VK(vk.CreateSampler(dev, &si, nullptr, &sampler));

  VkDescriptorSetLayoutBinding b = {};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dli = {};
  dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dli.bindingCount = 1;
  dli.pBindings = &b;
  VkDescriptorSetLayout dsl;
  VK(vk.CreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));

  VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 3 * sizeof(float)};
  VkPipelineLayoutCreateInfo pli = {};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &dsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  VkPipelineLayout pl;
  VK(vk.CreatePipelineLayout(dev, &pli, nullptr, &pl));

  auto shader = [&](const char *source, const uint32_t *spirv, size_t spirv_bytes,
                    VkShaderModule *m) {
    VkShaderModuleCreateInfo smi = {};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    if (glsl) {
      smi.codeSize = strlen(source);
      smi.pCode = reinterpret_cast<const uint32_t *>(source);
    } else {
      smi.codeSize = spirv_bytes;
      smi.pCode = spirv;
    }
    return vk.CreateShaderModule(dev, &smi, nullptr, m);
  };
  VkShaderModule vs, fs;
  VK(shader(kVertex, kProbeVertSpv, sizeof(kProbeVertSpv), &vs));
  VK(shader(kFragment, kProbeFragSpv, sizeof(kProbeFragSpv), &fs));

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  VkPipelineVertexInputStateCreateInfo vin = {};
  vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia = {};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp = {0, 0, float(kW), float(kH), 0, 1};
  VkRect2D sc = {{0, 0}, {kW, kH}};
  VkPipelineViewportStateCreateInfo vps = {};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs = {};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms = {};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {};
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
  gpi.layout = pl;
  gpi.renderPass = rp;
  VkPipeline pipe;
  VK(vk.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));

  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi = {};
  dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  VkDescriptorPool dp;
  VK(vk.CreateDescriptorPool(dev, &dpi, nullptr, &dp));
  VkDescriptorSetAllocateInfo dai = {};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = dp;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &dsl;
  VkDescriptorSet ds;
  VK(vk.AllocateDescriptorSets(dev, &dai, &ds));
  VkDescriptorImageInfo dii = {sampler, src_view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = ds;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &dii;
  vk.UpdateDescriptorSets(dev, 1, &w, 0, nullptr);

  VkCommandPoolCreateInfo cpi = {};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = qfi;
  VkCommandPool cpool;
  VK(vk.CreateCommandPool(dev, &cpi, nullptr, &cpool));
  VkCommandBufferAllocateInfo cai = {};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = cpool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK(vk.AllocateCommandBuffers(dev, &cai, &cmd));

  VkSemaphoreCreateInfo sci = {};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkSemaphore src_ready, dst_ready;
  VK(vk.CreateSemaphore(dev, &sci, nullptr, &src_ready));
  VK(vk.CreateSemaphore(dev, &sci, nullptr, &dst_ready));

  /* --- the draw, twenty times, fence to fence. --- */
  constexpr int kRuns = 20;
  double total = 0, worst = 0;
  for (int run = 0; run < kRuns; run++) {
    const double t0 = NowMs();
    VK(vk.AcquireImageANDROID(dev, src_img, -1, src_ready, VK_NULL_HANDLE));
    VK(vk.AcquireImageANDROID(dev, dst_img, -1, dst_ready, VK_NULL_HANDLE));

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vk.BeginCommandBuffer(cmd, &bi));
    /* The client's picture is to be read as it is: UNDEFINED would allow
     * the driver to drop it, so the source goes from GENERAL. */
    VkImageMemoryBarrier ib = {};
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = src_img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &ib);
    VkClearValue clear = {};
    VkRenderPassBeginInfo rbi = {};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = sc;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vk.CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, nullptr);
    const float pc[3] = {float(kW), float(kH), kRadius};
    vk.CmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vk.CmdDraw(cmd, 3, 1, 0, 0);
    vk.CmdEndRenderPass(cmd);
    VK(vk.EndCommandBuffer(cmd));

    VkSemaphore waits[2] = {src_ready, dst_ready};
    VkPipelineStageFlags stages_w[2] = {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo sub_i = {};
    sub_i.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub_i.waitSemaphoreCount = 2;
    sub_i.pWaitSemaphores = waits;
    sub_i.pWaitDstStageMask = stages_w;
    sub_i.commandBufferCount = 1;
    sub_i.pCommandBuffers = &cmd;
    VK(vk.QueueSubmit(queue, 1, &sub_i, VK_NULL_HANDLE));

    int done = -1;
    VK(vk.QueueSignalReleaseImageANDROID(queue, 0, nullptr, dst_img, &done));
    if (done >= 0) {
      if (!WaitFd(done)) FAIL("release fence never signalled");
      close(done);
    } else {
      vk.QueueWaitIdle(queue);
    }
    /* The source was acquired too; give it back the same way. */
    int src_done = -1;
    VK(vk.QueueSignalReleaseImageANDROID(queue, 0, nullptr, src_img, &src_done));
    if (src_done >= 0) close(src_done);
    vk.QueueWaitIdle(queue);

    const double ms = NowMs() - t0;
    total += ms;
    if (ms > worst) worst = ms;
    if (run == 0) P("first composition: %.2f ms (fence fd %s)\n", ms, done >= 0 ? "yes" : "none");
  }
  P("composition %ux%u: mean %.2f ms, worst %.2f ms over %d\n", kW, kH, total / kRuns, worst,
    kRuns);

  /* --- what came out. --- */
  void *p = nullptr;
  if (dst->lock(GRALLOC_USAGE_SW_READ_OFTEN, &p) != NO_ERROR || !p) FAIL("lock dst");
  const auto *px = static_cast<const uint32_t *>(p);
  const uint32_t stride = dst->getStride();
  int failures = 0;
  struct Probe {
    uint32_t x, y, want;
    const char *what;
  } probes[] = {
      {0, 0, 0, "top-left corner cut"},
      {kW - 1, 0, 0, "top-right corner cut"},
      {0, kH - 1, 0, "bottom-left corner cut"},
      {kW - 1, kH - 1, 0, "bottom-right corner cut"},
      {kW / 4, kH / 4, kRed, "red quadrant"},
      {3 * kW / 4, kH / 4, kGreen, "green quadrant"},
      {kW / 4, 3 * kH / 4, kBlue, "blue quadrant"},
      {3 * kW / 4, 3 * kH / 4, kWhite, "white quadrant"},
      {kW / 2 - 4, 0, kRed, "top edge, inside the corner"},
  };
  for (const Probe &pr : probes) {
    const uint32_t got = px[pr.y * stride + pr.x];
    const bool ok = got == pr.want;
    P("%s %-28s at %4u,%4u: 0x%08x (want 0x%08x)\n", ok ? "ok  " : "BAD ", pr.what, pr.x, pr.y,
      got, pr.want);
    failures += ok ? 0 : 1;
  }
  dst->unlock();
  P("%s\n", failures == 0 ? "PASS: a client's buffer is composed on the GPU with no copy"
                          : "FAIL: the composition is not what was drawn");
  return failures == 0 ? 0 : 1;
}
