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

#include "vk/ImageCache.h"

#include <sys/stat.h>

#include <algorithm>

namespace android::hwc::vk {

namespace {

/* VK_ANDROID_native_buffer, as the platform header keeps it to itself. */
constexpr VkStructureType kNativeBufferType = static_cast<VkStructureType>(1000010000);
struct NativeBuffer {
  VkStructureType sType;
  const void *pNext;
  const native_handle_t *handle;
  int stride;
  int format;
  int usage;
};

/* FNV-1a over every int of the handle: any change means another
 * allocation, whatever pointer it arrived under. */
uint32_t HashHandle(const native_handle_t *h) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < h->numFds + h->numInts; ++i) {
    hash ^= static_cast<uint32_t>(h->data[i]);
    hash *= 16777619u;
  }
  return hash;
}

/* The dma-buf behind the first fd: fd numbers are reused, inodes are not. */
uint64_t InodeOf(const native_handle_t *h) {
  struct stat st;
  if (h->numFds <= 0 || fstat(h->data[0], &st) != 0)
    return 0;
  return static_cast<uint64_t>(st.st_ino);
}

}  // namespace

ImageCache::ImageCache(const VulkanDeviceState &dev, VkRenderPass render_pass)
    : dev_(dev), render_pass_(render_pass) {
}

ImageCache::~ImageCache() {
  /* The owner has waited for the device to go idle before this. */
  for (auto &kv : entries_)
    Destroy(kv.second);
  for (const Entry &e : set_aside_)
    Destroy(e);
}

void ImageCache::Destroy(const Entry &e) {
  const VulkanPfn &vk = *dev_.pfn();
  if (e.framebuffer != VK_NULL_HANDLE)
    vk.DestroyFramebuffer(dev_.device(), e.framebuffer, nullptr);
  if (e.view != VK_NULL_HANDLE)
    vk.DestroyImageView(dev_.device(), e.view, nullptr);
  if (e.image != VK_NULL_HANDLE)
    vk.DestroyImage(dev_.device(), e.image, nullptr);
}

void ImageCache::SetAside(Entry e) {
  set_aside_.push_back(e);
}

void ImageCache::Retire(uint64_t frame) {
  auto done = std::partition(set_aside_.begin(), set_aside_.end(),
                             [frame](const Entry &e) { return e.last_frame > frame; });
  for (auto it = done; it != set_aside_.end(); ++it)
    Destroy(*it);
  set_aside_.erase(done, set_aside_.end());
}

ImageCache::Entry *ImageCache::Get(const native_handle_t *handle, uint32_t width,
                                   uint32_t height, uint32_t stride, int32_t hal_format,
                                   uint64_t usage, VkFormat format, bool target,
                                   uint64_t frame) {
  const uint32_t hash = HashHandle(handle);
  const uint64_t inode = InodeOf(handle);
  const Key key{handle, target};

  auto it = entries_.find(key);
  if (it != entries_.end()) {
    if (it->second.hash == hash && it->second.inode == inode) {
      it->second.last_frame = frame;
      return &it->second;
    }
    SetAside(it->second);
    entries_.erase(it);
  }

  /* The least recently used goes when the cache is full. */
  if (entries_.size() >= kLimit) {
    auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                   [](const auto &a, const auto &b) {
                                     return a.second.last_frame < b.second.last_frame;
                                   });
    SetAside(oldest->second);
    entries_.erase(oldest);
  }

  const VulkanPfn &vk = *dev_.pfn();
  NativeBuffer nb = {};
  nb.sType = kNativeBufferType;
  nb.handle = handle;
  nb.stride = static_cast<int>(stride);
  nb.format = hal_format;
  nb.usage = static_cast<int>(usage);

  VkImageCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.pNext = &nb;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {width, height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  Entry e;
  e.hash = hash;
  e.inode = inode;
  e.last_frame = frame;
  if (vk.CreateImage(dev_.device(), &ci, nullptr, &e.image) != VK_SUCCESS)
    return nullptr;

  VkImageViewCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = e.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk.CreateImageView(dev_.device(), &vi, nullptr, &e.view) != VK_SUCCESS) {
    Destroy(e);
    return nullptr;
  }

  if (target) {
    VkFramebufferCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = render_pass_;
    fi.attachmentCount = 1;
    fi.pAttachments = &e.view;
    fi.width = width;
    fi.height = height;
    fi.layers = 1;
    if (vk.CreateFramebuffer(dev_.device(), &fi, nullptr, &e.framebuffer) != VK_SUCCESS) {
      Destroy(e);
      return nullptr;
    }
  }

  return &entries_.emplace(key, e).first->second;
}

}  // namespace android::hwc::vk
