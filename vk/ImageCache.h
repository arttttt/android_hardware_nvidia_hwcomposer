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

#include <stdint.h>

#include <map>
#include <utility>
#include <vector>

#include <cutils/native_handle.h>

#include "vk/runtime/VulkanDeviceState.h"
#include "vk/runtime/loader/VulkanPfn.h"

namespace android::hwc::vk {

/* Gralloc buffers as Vulkan images, made once and kept.
 *
 * VK_ANDROID_native_buffer binds an image to one allocation for good. The
 * handle pointer a buffer arrives under is reused for other allocations --
 * a BufferQueue slot recycled, an fd number given to another dma-buf -- so
 * a hit is confirmed against a hash of the whole handle and the dma-buf's
 * inode, as the camera learned the hard way (VulkanGrallocCache there).
 *
 * Nothing here waits for the GPU. An entry found stale, or pushed out by
 * the size limit, may still be read by a frame in flight, so it is set
 * aside with the number of the last frame that used it and destroyed once
 * the caller says that frame is done.
 */
class ImageCache {
 public:
  struct Entry {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE; /* targets only */
    uint32_t hash = 0;
    uint64_t inode = 0;
    uint64_t last_frame = 0;
  };

  /* `render_pass` is the one targets are framed for. */
  ImageCache(const VulkanDeviceState &dev, VkRenderPass render_pass);
  ~ImageCache();

  ImageCache(const ImageCache &) = delete;
  ImageCache &operator=(const ImageCache &) = delete;

  /* The image for this buffer, made if need be; null if the driver turned
   * the import down. `target` asks for a colour attachment with its
   * framebuffer, otherwise a sampled image. `frame` is the frame about to
   * use it. */
  Entry *Get(const native_handle_t *handle, uint32_t width, uint32_t height, uint32_t stride,
             int32_t hal_format, uint64_t usage, VkFormat format, bool target, uint64_t frame);

  /* Every frame up to `frame` is done: what was set aside and last used
   * by one of them can go. */
  void Retire(uint64_t frame);

  size_t size() const { return entries_.size(); }

 private:
  using Key = std::pair<const native_handle_t *, bool>;

  void Destroy(const Entry &e);
  void SetAside(Entry e);

  /* Enough for every layer of a busy scene and the pool's targets, with
   * room for a BufferQueue's three buffers each. */
  static constexpr size_t kLimit = 64;

  const VulkanDeviceState &dev_;
  const VkRenderPass render_pass_;
  std::map<Key, Entry> entries_;
  std::vector<Entry> set_aside_;
};

}  // namespace android::hwc::vk
