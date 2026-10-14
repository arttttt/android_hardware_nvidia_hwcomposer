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

/* The zone stand: the same picture into four kinds of target, 720x720.
 *
 * The composer's three targets today are gralloc buffers in IOVMM; the
 * question is whether a target taken from the composer's carveout -- the
 * zone the VIC's slots live in, physically contiguous, statically mapped
 * for the DC -- draws as fast, and imports at all. The zone holds
 * 3 x 12.6 MB of VIC slots in its 40 MB, leaving about 2.26 MB: a
 * 720x720 RGBA target is 2.07 MB and fits.
 *
 * The targets:
 *   gralloc-bl  -- the arrangement today: HW_RENDER|HW_COMPOSER, imported
 *                  through VK_ANDROID_native_buffer (the executor's path);
 *   gralloc-pitch -- gralloc with SW usage stacked on, which the
 *                  allocator lays out in rows;
 *   zone-wc     -- nvmap CREATE/ALLOC from NVMAP_HEAP_CARVEOUT_COMPOSER
 *                  with WRITE_COMBINE (the way VicSession makes the VIC
 *                  slots), imported as OPAQUE_FD, tiling LINEAR;
 *   zone-cached -- the same without write-combine.
 *
 * Every frame is three textured draws by the fragment shader -- on this
 * driver a compute write into an imported image comes out as garbage, and
 * the executor itself draws by raster, so the stand does too. Timing is
 * vkQueueSubmit -> vkWaitForFences on the CPU, 10 warm-up frames thrown
 * away and 120 kept; the first frame (import included) is printed apart.
 * Correctness: the picture is quadrant colours with the corners cut, and
 * a few pixels are read back at the end (gralloc by lock, the zone by its
 * own mapping) and checked.
 *
 *   m hwcvk_zone_stand
 *   adb push .../system/bin/hwcvk_zone_stand /data/local/tmp/
 *   adb shell /data/local/tmp/hwcvk_zone_stand
 */

#define LOG_TAG "hwcvk_zone_stand"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include <android/log.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBuffer.h>
#include <vulkan/vulkan.h>

#include "tegra/nvmap.h"
#include "vk/runtime/VulkanDeviceState.h"
#include "vk/runtime/loader/VulkanPfn.h"

using namespace android;

namespace {

/* The zone's own pitch grain, as VicSession keeps it for the engine's
 * parser; the driver's own pitch for a LINEAR image is asked of the image
 * itself, and the read-back follows whichever it said. */
constexpr uint32_t kZonePitchGrain = 256;

constexpr uint32_t kSrcW = 1536, kSrcH = 2048;
constexpr uint32_t kW = 720, kH = 720;
constexpr uint32_t kCorner = 48;

constexpr uint32_t kRed = 0xff0000ff;
constexpr uint32_t kGreen = 0xff00ff00;
constexpr uint32_t kBlue = 0xffff0000;
constexpr uint32_t kWhite = 0xffffffff;

int failures = 0;

double NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

bool WaitFd(int fd) {
  pollfd p = {fd, POLLIN, 0};
  return poll(&p, 1, 2000) == 1;
}

#define P(...)                    \
  do {                            \
    fprintf(stderr, __VA_ARGS__); \
    ALOGI(__VA_ARGS__);           \
  } while (0)
#define FAIL(...)                            \
  do {                                       \
    fprintf(stderr, "FAIL: " __VA_ARGS__);   \
    fprintf(stderr, "\n");                   \
    return 1;                                \
  } while (0)
#define VK(call)                                                     \
  do {                                                               \
    VkResult r_ = (call);                                            \
    if (r_ != VK_SUCCESS) FAIL("%s = %d", #call, r_);                \
  } while (0)

/* VK_ANDROID_native_buffer, declared by hand as the probe does. */
#define VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID_PROBE ((VkStructureType)1000010000)
struct NativeBufferProbe {
  VkStructureType sType;
  const void *pNext;
  const native_handle_t *handle;
  int stride;
  int format;
  int usage;
};

/* Two pNext structs the runtime's headers do not carry. */
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR ((VkStructureType)1000072001)
struct VkExternalMemoryImageCreateInfoLocal {
  VkStructureType sType;
  const void *pNext;
  uint32_t handleTypes; /* VkExternalMemoryHandleTypeFlagsKHR */
};
#define VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR ((VkStructureType)1000076000)
struct VkMemoryDedicatedAllocateInfoLocal {
  VkStructureType sType;
  const void *pNext;
  VkImage image;
  VkBuffer buffer;
};

uint32_t QuadrantColour(uint32_t x, uint32_t y, uint32_t index) {
  const bool right = x >= kSrcW / 2, bottom = y >= kSrcH / 2;
  const uint32_t c[4] = {kRed, kGreen, kBlue, kWhite};
  const uint32_t q = (bottom ? 2 : 0) + (right ? 1 : 0);
  /* A tint per layer so the three draws each leave their own mark and a
   * wrong count of draws cannot pass for a right one. */
  return c[(q + index) & 3];
}

/* One fullscreen quad; the fragment samples its own layer and cuts the
 * corners by distance, premultiplied, as the executor's shader does. */
const char kVertex[] =
    "#version 450\n"
    "void main() {\n"
    "    float x = float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0;\n"
    "    float y = float(gl_VertexIndex & 2) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "}\n";
const char kFragment[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform PC { vec4 colour; vec2 size; float radius; } pc;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() {\n"
    "    vec2 p = gl_FragCoord.xy;\n"
    "    vec4 c = texture(src, p / pc.size);\n"
    "    vec2 q = abs(p - pc.size * 0.5) - (pc.size * 0.5 - vec2(pc.radius));\n"
    "    float d = length(max(q, vec2(0.0))) - pc.radius;\n"
    "    float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "    float m = 0.34 * pc.colour.a;\n" /* each layer adds a third */
    "    vec3 rgb = c.rgb * (1.0 - m) + pc.colour.rgb * m;\n"
    "    outColor = vec4(rgb * a, a);\n"
    "}\n";

struct Push {
  float r, g, b, a;
  float w, h;
  float radius;
};

/* A target variant: an image, a framebuffer, and the way to read it back
 * and to describe it in words. */
struct Target {
  const char *name = "?";
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE; /* the imported one, if any */
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  bool linear = false;
  uint32_t row_pitch = 0; /* the writer's pitch: gralloc stride or the image's own */
  uint32_t mapped_height = kH;
  void *pixels = nullptr; /* the read-back mapping, gralloc lock or mmap */
  sp<GraphicBuffer> gb;   /* keeps a gralloc target alive */
  int zone_fd = -1;       /* keeps a zone target's dma-buf alive */
  bool gb_locked = false;
};

void CloseTarget(VulkanDeviceState &state, Target &t, VulkanPfn &vk) {
  if (t.pixels) {
    if (t.gb_locked && t.gb) {
      t.gb->unlock();
    } else {
      munmap(t.pixels, static_cast<size_t>(t.row_pitch) * t.mapped_height);
    }
    t.pixels = nullptr;
  }
  if (t.framebuffer) vk.DestroyFramebuffer(state.device(), t.framebuffer, nullptr);
  if (t.view) vk.DestroyImageView(state.device(), t.view, nullptr);
  if (t.image) vk.DestroyImage(state.device(), t.image, nullptr);
  /* For a gralloc target the driver owns the memory; for a zone target
   * the memory holds the import reference and frees it. */
  if (t.memory) vk.FreeMemory(state.device(), t.memory, nullptr);
  if (t.zone_fd >= 0) close(t.zone_fd);
  t.gb = nullptr;
}

/* --- the zone, as VicSession takes from it --- */
struct ZoneBuffer {
  int fd = -1;
  uint32_t size = 0;
  void *pixels = nullptr;
};

ZoneBuffer TakeZone(int nvmap_fd, bool write_combine, uint32_t bytes) {
  ZoneBuffer z;
  struct nvmap_create_handle create = {};
  create.size = bytes;
  if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &create) != 0) {
    P("  zone: CREATE %u: %s\n", bytes, strerror(errno));
    return z;
  }
  struct nvmap_alloc_handle alloc = {};
  alloc.handle = create.handle;
  alloc.heap_mask = NVMAP_HEAP_CARVEOUT_COMPOSER;
  alloc.flags = (write_combine ? NVMAP_HANDLE_WRITE_COMBINE : NVMAP_HANDLE_CACHEABLE) |
                (0x5a << 16); /* a stand tag, out of the composer's way */
  alloc.align = 4096;
  if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &alloc) != 0) {
    P("  zone: ALLOC (%s): %s -- the zone is busy or full, variant skipped\n",
      write_combine ? "WC" : "cached", strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, create.handle);
    return z;
  }
  struct nvmap_create_handle get = {};
  get.handle = create.handle;
  if (ioctl(nvmap_fd, NVMAP_IOC_GET_FD, &get) != 0) {
    P("  zone: GET_FD: %s\n", strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, create.handle);
    return z;
  }
  ioctl(nvmap_fd, NVMAP_IOC_FREE, create.handle);
  z.fd = get.fd;
  z.size = bytes;
  z.pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, z.fd, 0);
  if (z.pixels == MAP_FAILED) {
    P("  zone: mmap: %s\n", strerror(errno));
    close(z.fd);
    z.fd = -1;
    z.pixels = nullptr;
  }
  return z;
}

/* The zone's pitch grain is what the engine's parser wants; the size is
 * rounded to the grain VicSession rounds to, 128K. */
uint32_t ZoneBytesFor(uint32_t width, uint32_t height) {
  const uint32_t pitch =
      ((width * 4 + kZonePitchGrain - 1) / kZonePitchGrain) * kZonePitchGrain;
  const uint64_t raw = static_cast<uint64_t>(pitch) * ((height + 3) & ~3u);
  return static_cast<uint32_t>((raw + 131071) / 131072 * 131072);
}

}  // namespace

int main() {
  VulkanDeviceState state;
  if (!state.init()) FAIL("Vulkan device did not come up");
  const VulkanPfn &vk = *state.pfn();
  const VkDevice dev = state.device();
  const VkQueue queue = state.queue();
  P("device up; native_buffer=%d external_memory_fd reported by probe earlier\n",
    state.nativeBufferAvailable() ? 1 : 0);

  /* A hand-loaded entry the runtime does not carry. */
  auto SubresourceLayout = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(
      vk.GetDeviceProcAddr(dev, "vkGetImageSubresourceLayout"));
  if (!SubresourceLayout) FAIL("vkGetImageSubresourceLayout absent");

  /* --- the scene's three sources, filled and imported as the executor does. */
  const uint64_t src_usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
                             GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_WRITE_OFTEN;
  std::vector<sp<GraphicBuffer>> sources;
  std::vector<VkImage> src_images;
  std::vector<VkImageView> src_views;
  for (uint32_t i = 0; i < 3; i++) {
    sp<GraphicBuffer> gb =
        new GraphicBuffer(kSrcW, kSrcH, PIXEL_FORMAT_RGBA_8888, 1, src_usage, "zone stand src");
    if (gb->initCheck() != NO_ERROR) FAIL("gralloc source");
    void *p = nullptr;
    if (gb->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &p) != NO_ERROR || !p) FAIL("lock source");
    auto *px = static_cast<uint32_t *>(p);
    for (uint32_t y = 0; y < kSrcH; y++)
      for (uint32_t x = 0; x < kSrcW; x++)
        px[y * gb->getStride() + x] = QuadrantColour(x, y, i);
    gb->unlock();

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
    ci.extent = {kSrcW, kSrcH, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage img;
    VkImageView view;
    VK(vk.CreateImage(dev, &ci, nullptr, &img));
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK(vk.CreateImageView(dev, &vi, nullptr, &view));
    sources.push_back(gb);
    src_images.push_back(img);
    src_views.push_back(view);
  }

  /* --- the pipeline: one pass, three draws. --- */
  VkAttachmentDescription att = {};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

  VkSamplerCreateInfo si = {};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler;
  VK(vk.CreateSampler(dev, &si, nullptr, &sampler));

  VkDescriptorSetLayoutBinding b = {};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b.pImmutableSamplers = &sampler;
  VkDescriptorSetLayoutCreateInfo dli = {};
  dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dli.bindingCount = 1;
  dli.pBindings = &b;
  VkDescriptorSetLayout dsl;
  VK(vk.CreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));

  VkPushConstantRange pc = {};
  pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pc.offset = 0;
  pc.size = sizeof(Push);
  VkPipelineLayoutCreateInfo pli = {};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &dsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pc;
  VkPipelineLayout layout;
  VK(vk.CreatePipelineLayout(dev, &pli, nullptr, &layout));

  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
  VkDescriptorPoolCreateInfo dpi = {};
  dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpi.maxSets = 3;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &pool_size;
  VkDescriptorPool pool;
  VK(vk.CreateDescriptorPool(dev, &dpi, nullptr, &pool));

  std::vector<VkDescriptorSet> sets(3, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo dai = {};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = pool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &dsl;
  for (uint32_t i = 0; i < 3; i++) {
    VK(vk.AllocateDescriptorSets(dev, &dai, &sets[i]));
    VkDescriptorImageInfo dii = {};
    dii.sampler = sampler;
    dii.imageView = src_views[i];
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = sets[i];
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vk.UpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }

  /* GLSL text as shader code, the NV way the runtime enables: the size is
   * the string length and the pointer the text itself. */
  VkShaderModuleCreateInfo si_ = {};
  si_.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  si_.pCode = reinterpret_cast<const uint32_t *>(kVertex);
  si_.codeSize = strlen(kVertex);
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VK(vk.CreateShaderModule(dev, &si_, nullptr, &vert));
  si_.pCode = reinterpret_cast<const uint32_t *>(kFragment);
  si_.codeSize = strlen(kFragment);
  VK(vk.CreateShaderModule(dev, &si_, nullptr, &frag));

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  VkPipelineVertexInputStateCreateInfo vi_in = {};
  vi_in.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia = {};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkViewport viewport = {0.f, 0.f, float(kW), float(kH), 0.f, 1.f};
  VkRect2D scissor = {{0, 0}, {kW, kH}};
  VkPipelineViewportStateCreateInfo vp = {};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.pViewports = &viewport;
  vp.scissorCount = 1;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs = {};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms = {};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
  gpi.pVertexInputState = &vi_in;
  gpi.pInputAssemblyState = &ia;
  gpi.pViewportState = &vp;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pColorBlendState = &cb;
  gpi.layout = layout;
  gpi.renderPass = rp;
  gpi.subpass = 0;
  VkPipeline pipeline;
  VK(vk.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));

  VkCommandPoolCreateInfo cpi = {};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = state.queueFamily();
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmd_pool;
  VK(vk.CreateCommandPool(dev, &cpi, nullptr, &cmd_pool));
  VkCommandBufferAllocateInfo cbi = {};
  cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbi.commandPool = cmd_pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK(vk.AllocateCommandBuffers(dev, &cbi, &cmd));
  VkFenceCreateInfo fi = {};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  VK(vk.CreateFence(dev, &fi, nullptr, &fence));

  const Push pushes[3] = {
      {1.f, 0.f, 0.f, 1.f, float(kW), float(kH), float(kCorner)},
      {0.f, 1.f, 0.f, 1.f, float(kW), float(kH), float(kCorner)},
      {0.f, 0.f, 1.f, 1.f, float(kW), float(kH), float(kCorner)},
  };

  /* One frame into a target: three draws by the fragment shader. */
  auto DrawFrame = [&](Target &t) {
    VK(vk.ResetFences(dev, 1, &fence));
    VK(vk.ResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vk.BeginCommandBuffer(cmd, &bi));
    VkClearValue clear = {};
    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = rp;
    rb.framebuffer = t.framebuffer;
    rb.renderArea = {{0, 0}, {kW, kH}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vk.CmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    for (uint32_t i = 0; i < 3; i++) {
      vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                               &sets[i], 0, nullptr);
      vk.CmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                          &pushes[i]);
      vk.CmdDraw(cmd, 4, 1, 0, 0);
    }
    vk.CmdEndRenderPass(cmd);
    VK(vk.EndCommandBuffer(cmd));
    VkSubmitInfo subi = {};
    subi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    subi.commandBufferCount = 1;
    subi.pCommandBuffers = &cmd;
    const double t0 = NowMs();
    VK(vk.QueueSubmit(queue, 1, &subi, fence));
    VK(vk.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    return NowMs() - t0;
  };

  /* Correctness: quadrants of the LAST layer's tint over the picture,
   * corners cut to nothing. */
  auto CheckTarget = [&](Target &t) {
    if (!t.pixels) return;
    if (t.zone_fd >= 0 && !t.linear) {
      P("  (OPTIMAL tiling: the layout is the driver's own, pixel check skipped)\n");
      return;
    }
    auto *px = static_cast<uint32_t *>(t.pixels);
    const uint32_t s = t.row_pitch ? t.row_pitch : kW * 4;
    auto at = [&](uint32_t x, uint32_t y) { return px[(y * s) / 4 + x]; };
    struct Probe {
      uint32_t x, y;
      bool inside;
    } probes[] = {
        {kW / 4, kH / 4, true},   {3 * kW / 4, kH / 4, true},
        {kW / 4, 3 * kH / 4, true}, {3 * kW / 4, 3 * kH / 4, true},
        {2, 2, false},            {kW - 3, 2, false},
        {2, kH - 3, false},       {kW - 3, kH - 3, false},
    };
    for (const Probe &p : probes) {
      const uint32_t got = at(p.x, p.y);
      /* Loose on purpose: full alpha and a coloured centre inside, exact
       * zero in the cut corners. The exact tint is three folded draws. */
      const bool ok = p.inside ? (((got >> 24) == 0xff) && ((got & 0x00ffffff) != 0))
                               : (got == 0);
      P("  %-6s %u,%u: 0x%08x%s\n", p.inside ? "inside" : "corner", p.x, p.y, got,
        ok ? "" : "  <-- UNEXPECTED");
      failures += ok ? 0 : 1;
    }
  };

  /* --- the targets --- */
  std::vector<Target> targets;
  auto FinishTarget = [&](Target &t) {
    VkFramebufferCreateInfo fbi = {};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = rp;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &t.view;
    fbi.width = kW;
    fbi.height = kH;
    fbi.layers = 1;
    if (vk.CreateFramebuffer(dev, &fbi, nullptr, &t.framebuffer) != VK_SUCCESS) {
      P("  %s: framebuffer refused\n", t.name);
      CloseTarget(state, t, const_cast<VulkanPfn &>(vk));
      return false;
    }
    return true;
  };

  /* (a, b) gralloc targets through VK_ANDROID_native_buffer. */
  auto MakeGralloc = [&](const char *name, uint64_t usage, double *creation_ms) -> bool {
    const double tc0 = NowMs();
    Target t;
    t.name = name;
    t.gb = new GraphicBuffer(kW, kH, PIXEL_FORMAT_RGBA_8888, 1, usage, name);
    if (t.gb->initCheck() != NO_ERROR) {
      P("  %s: gralloc refused\n", name);
      return false;
    }
    NativeBufferProbe nb = {};
    nb.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID_PROBE;
    nb.handle = t.gb->handle;
    nb.stride = static_cast<int>(t.gb->getStride());
    nb.format = t.gb->getPixelFormat();
    nb.usage = static_cast<int>(t.gb->getUsage());
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
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (vk.CreateImage(dev, &ci, nullptr, &t.image) != VK_SUCCESS) {
      P("  %s: CreateImage (native_buffer) refused\n", name);
      t.gb = nullptr;
      return false;
    }
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.CreateImageView(dev, &vi, nullptr, &t.view) != VK_SUCCESS) {
      P("  %s: view refused\n", name);
      CloseTarget(state, t, const_cast<VulkanPfn &>(vk));
      return false;
    }
    t.row_pitch = t.gb->getStride() * 4;
    P("  %s: stride %u px (%s layout by usage)\n", name, t.gb->getStride(), name);
    if (!FinishTarget(t)) return false;
    targets.push_back(t);
    if (creation_ms) *creation_ms = NowMs() - tc0;
    return true;
  };

  /* (c, d) the zone, imported as OPAQUE_FD. */
  auto MakeZone = [&](const char *name, bool write_combine, double *creation_ms) -> bool {
    const double tc0 = NowMs();
    const int nvmap_fd = open("/dev/nvmap", O_RDWR | O_CLOEXEC);
    if (nvmap_fd < 0) {
      P("  %s: /dev/nvmap: %s\n", name, strerror(errno));
      return false;
    }
    ZoneBuffer z = TakeZone(nvmap_fd, write_combine, ZoneBytesFor(kW, kH));
    close(nvmap_fd);
    if (z.fd < 0) return false;

    Target t;
    t.name = name;
    t.zone_fd = z.fd;

    VkMemoryFdPropertiesKHR fdp = {};
    fdp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (!vk.GetMemoryFdPropertiesKHR ||
        vk.GetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
                                    z.fd, &fdp) != VK_SUCCESS ||
        fdp.memoryTypeBits == 0) {
      P("  %s: fd properties refused (bits 0x%x) -- the camera's path is closed here\n",
        name, fdp.memoryTypeBits);
      munmap(z.pixels, z.size);
      close(z.fd);
      return false;
    }
    P("  %s: fd typeBits 0x%x\n", name, fdp.memoryTypeBits);

    VkExternalMemoryImageCreateInfoLocal emi = {};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &emi;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kW, kH, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_LINEAR;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult r = vk.CreateImage(dev, &ci, nullptr, &t.image);
    if (r != VK_SUCCESS) {
      P("  %s: LINEAR refused (%d), trying OPTIMAL\n", name, r);
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      r = vk.CreateImage(dev, &ci, nullptr, &t.image);
      if (r != VK_SUCCESS) {
        P("  %s: OPTIMAL refused too (%d) -- variant skipped\n", name, r);
        munmap(z.pixels, z.size);
        close(z.fd);
        return false;
      }
    } else {
      t.linear = true;
    }

    VkMemoryRequirements req;
    vk.GetImageMemoryRequirements(dev, t.image, &req);
    uint32_t type = 0;
    bool have_type = false;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++) {
      if ((req.memoryTypeBits & (1u << i)) && (fdp.memoryTypeBits & (1u << i))) {
        type = i;
        have_type = true;
        break;
      }
    }
    if (!have_type) {
      P("  %s: no memory type both accept (img 0x%x fd 0x%x) -- variant skipped\n", name,
        req.memoryTypeBits, fdp.memoryTypeBits);
      vk.DestroyImage(dev, t.image, nullptr);
      munmap(z.pixels, z.size);
      close(z.fd);
      return false;
    }
    if (req.size > z.size) {
      P("  %s: driver wants %llu bytes, the zone buffer has %u -- variant skipped\n", name,
        (unsigned long long)req.size, z.size);
      vk.DestroyImage(dev, t.image, nullptr);
      munmap(z.pixels, z.size);
      close(z.fd);
      return false;
    }

    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
    import.fd = dup(z.fd);
    VkMemoryDedicatedAllocateInfoLocal dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
    dedicated.image = t.image;
    dedicated.pNext = &import;
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    mai.pNext = &dedicated;
    r = vk.AllocateMemory(dev, &mai, nullptr, &t.memory);
    if (r != VK_SUCCESS) {
      P("  %s: dedicated+import refused (%d), retrying without dedicated\n", name, r);
      mai.pNext = &import;
      r = vk.AllocateMemory(dev, &mai, nullptr, &t.memory);
      if (r != VK_SUCCESS) {
        P("  %s: import refused (%d) -- variant skipped\n", name, r);
        vk.DestroyImage(dev, t.image, nullptr);
        munmap(z.pixels, z.size);
        close(z.fd);
        return false;
      }
    }
    VK(vk.BindImageMemory(dev, t.image, t.memory, 0));

    uint32_t pitch = kW * 4;
    if (t.linear) {
      VkImageSubresource sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
      VkSubresourceLayout lay;
      SubresourceLayout(dev, t.image, &sr, &lay);
      pitch = static_cast<uint32_t>(lay.rowPitch);
      P("  %s: LINEAR rowPitch %u offset %llu (zone grain pitch %u)\n", name, pitch,
        (unsigned long long)lay.offset,
        ((kW * 4 + kZonePitchGrain - 1) / kZonePitchGrain) * kZonePitchGrain);
    } else {
      P("  %s: OPTIMAL tiling -- the layout is the driver's own, DC cannot be told it\n",
        name);
    }
    t.row_pitch = pitch;

    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.CreateImageView(dev, &vi, nullptr, &t.view) != VK_SUCCESS) {
      P("  %s: view refused\n", name);
      CloseTarget(state, t, const_cast<VulkanPfn &>(vk));
      return false;
    }
    if (!FinishTarget(t)) return false;
    t.pixels = z.pixels; /* the read-back mapping, mmap'ed already */
    t.mapped_height = kH;
    targets.push_back(t);
    if (creation_ms) *creation_ms = NowMs() - tc0;
    return true;
  };

  P("targets 720x720 RGBA, scene: 3 gralloc sources 1536x2048, fragment ROP\n");
  double creation_ms = -1;
  /* The executor's own target usage (tegra/GpuExecutor.cpp kTargetUsage):
   * with a CPU-read bit the allocator would lay it out in rows instead. */
  MakeGralloc("gralloc-bl", GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER,
              &creation_ms);
  const double bl_ms = creation_ms;
  creation_ms = -1;
  MakeGralloc("gralloc-pitch",
              GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_READ_OFTEN |
                  GRALLOC_USAGE_SW_WRITE_OFTEN,
              &creation_ms);
  const double pitch_ms = creation_ms;
  creation_ms = -1;
  MakeZone("zone-wc", true, &creation_ms);
  const double wc_ms = creation_ms;
  creation_ms = -1;
  MakeZone("zone-cached", false, &creation_ms);
  const double cached_ms = creation_ms;

  if (targets.empty()) FAIL("no target came up at all");

  P("import (alloc+import+fb) ms: gralloc-bl %.3f  gralloc-pitch %.3f  zone-wc %.3f  zone-cached %.3f\n",
    bl_ms, pitch_ms, wc_ms, cached_ms);

  const int kWarmup = 10, kFrames = 120;
  for (Target &t : targets) {
    std::vector<double> ms;
    double first = -1;
    for (int i = 0; i < kWarmup + kFrames; i++) {
      const double d = DrawFrame(t);
      if (i == 0) first = d; /* the first drawn frame after the import */
      if (i >= kWarmup) ms.push_back(d);
    }
    std::sort(ms.begin(), ms.end());
    P("%-13s first %.3f | fence ms: median %.3f  p90 %.3f  worst %.3f  (n=%d)\n", t.name,
      first, ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back(), int(ms.size()));

  }

  /* Correctness, and the read-back for the gralloc targets by lock. */
  for (Target &t : targets) {
    if (!t.pixels && t.gb) {
      void *p = nullptr;
      if (t.gb->lock(GRALLOC_USAGE_SW_READ_OFTEN, &p) == NO_ERROR && p) {
        t.pixels = p;
        t.gb_locked = true;
        t.row_pitch = t.gb->getStride() * 4;
      }
    }
    P("%s readback:\n", t.name);
    CheckTarget(t);
  }

  for (Target &t : targets) CloseTarget(state, t, const_cast<VulkanPfn &>(vk));
  vk.DestroyFence(dev, fence, nullptr);
  vk.FreeCommandBuffers(dev, cmd_pool, 1, &cmd);
  vk.DestroyCommandPool(dev, cmd_pool, nullptr);
  vk.DestroyPipeline(dev, pipeline, nullptr);
  vk.DestroyShaderModule(dev, frag, nullptr);
  vk.DestroyShaderModule(dev, vert, nullptr);
  vk.DestroyDescriptorPool(dev, pool, nullptr);
  vk.DestroyPipelineLayout(dev, layout, nullptr);
  vk.DestroyDescriptorSetLayout(dev, dsl, nullptr);
  vk.DestroySampler(dev, sampler, nullptr);
  vk.DestroyRenderPass(dev, rp, nullptr);
  for (uint32_t i = 0; i < 3; i++) {
    vk.DestroyImageView(dev, src_views[i], nullptr);
    vk.DestroyImage(dev, src_images[i], nullptr);
  }
  P("%s\n", failures == 0 ? "PASS" : "CHECK FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
