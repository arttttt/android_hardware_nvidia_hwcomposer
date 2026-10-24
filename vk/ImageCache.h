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

#include "compositor/GarbageCollector.h"
#include "vk/CacheBudget.h"
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
 * the budget, may still be read by a frame in flight, so it is set aside
 * with the number of the last frame that used it and destroyed once the
 * caller says that frame is done. One imported ahead and never drawn has
 * no frame, and goes at the next word that any frame is done.
 *
 * Kept within a budget of bytes as well as of images -- see CacheBudget:
 * an image keeps its buffer's memory from going back to the pool.
 */
class ImageCache {
 public:
  struct Entry {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE; /* targets only */
    uint32_t hash = 0;
    uint64_t inode = 0;
    /* The last frame that read or drew it; nought for one only imported
     * ahead. */
    uint64_t last_frame = 0;
    /* When it was last asked for, for the budget's choice. */
    uint64_t last_use = 0;
    /* The composer's layer it last came with, 0 for none: when that
     * layer goes, Forget lets the image go without waiting for the
     * budget. */
    uint64_t owner = 0;
    size_t bytes = 0;
    /* The copy of the handle the image was imported from, when the
     * height had to be told differently; owned, closed with the image. */
    native_handle_t *copy = nullptr;
    /* A copy of a client buffer's handle, its descriptors duplicated: the
     * cache's own reference, so that the last one is the cache's and is
     * let go on the garbage collector's thread, not by whoever destroys
     * the image. Null for a target, which is ours anyway. */
    native_handle_t *hold = nullptr;
    /* A target drawn into at least once: its contents are a picture of
     * ours, and it was left in the attachment layout -- what a drawing of
     * only part of it builds on. */
    bool drawn = false;
  };

  /* `render_pass` is the one targets are framed for; `gc` lets go of the
   * client buffers the cache held, off the drawing thread. */
  ImageCache(const VulkanDeviceState &dev, VkRenderPass render_pass,
             drm_hwcomposer::GarbageCollector *gc);
  ~ImageCache();

  ImageCache(const ImageCache &) = delete;
  ImageCache &operator=(const ImageCache &) = delete;

  /* The image for this buffer, made if need be; null if the driver turned
   * the import down. `target` asks for a colour attachment with its
   * framebuffer, otherwise a sampled image. `frame` is the frame about to
   * use it, nought for an import ahead of any. */
  Entry *Get(const native_handle_t *handle, uint32_t width, uint32_t height, uint32_t stride,
             int32_t hal_format, uint64_t usage, VkFormat format, bool target, uint64_t frame,
             int32_t height_word = -1, uint64_t owner = 0);

  /* The composer's layer `owner` is gone: every client buffer that last
   * came with it is set aside, to go once the frames that read it are
   * done. */
  void Forget(uint64_t owner);

  /* Every frame up to `frame` is done: what was set aside and last used
   * by one of them can go. */
  void Retire(uint64_t frame);

  /* A frame starts asking for its images: what it asks for from here on
   * is not let go to make room until the next one starts. */
  void BeginFrame() { frame_start_ = tick_ + 1; }

  size_t size() const { return entries_.size(); }
  size_t bytes() const { return bytes_; }

 private:
  using Key = std::pair<const native_handle_t *, bool>;

  void Destroy(const Entry &e);
  void SetAside(Entry e);
  /* Out of the cache and set aside. */
  void Drop(std::map<Key, Entry>::iterator it);
  /* Lets images go, least recently asked for first, until `adding` more
   * bytes fit. */
  void MakeRoom(size_t adding);

  const CacheBudget budget_;

  const VulkanDeviceState &dev_;
  const VkRenderPass render_pass_;
  drm_hwcomposer::GarbageCollector *const gc_;
  std::map<Key, Entry> entries_;
  std::vector<Entry> set_aside_;
  size_t bytes_ = 0;
  uint64_t tick_ = 0;
  uint64_t frame_start_ = 0;
};

}  // namespace android::hwc::vk
