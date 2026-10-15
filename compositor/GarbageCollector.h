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

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace android::drm_hwcomposer {

/* Where the composer's threads leave what is theirs to let go.
 *
 * Letting go of a buffer is not free: the last reference to a dma-buf
 * takes its SMMU mappings down page by page and hands its pages back to
 * nvmap's pool, and the kernel does it on whichever thread closes that
 * reference -- on the device a few milliseconds a second of the GPU
 * executor's thread, which runs at FIFO beside the frame. A thread with
 * the frame to finish hands the last reference here instead, and a thread
 * of the collector's own, at a lower priority, closes it.
 *
 * Not idle priority: a collector that never ran under load would hold the
 * memory it was given, and the pool would run dry -- the very thing the
 * GPU executor's image cache was once found doing. A nice of ten yields
 * to the frame and still runs.
 *
 * Jobs run in the order posted. The thread starts with the first job and
 * ends with the collector, after every job posted has run.
 */
class GarbageCollector {
 public:
  explicit GarbageCollector(std::string name) : name_(std::move(name)) {
  }

  ~GarbageCollector() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  GarbageCollector(const GarbageCollector &) = delete;
  GarbageCollector &operator=(const GarbageCollector &) = delete;

  /* `job` runs on the collector's thread, soon. */
  void Post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back(std::move(job));
      if (!thread_.joinable())
        thread_ = std::thread(&GarbageCollector::Loop, this);
    }
    changed_.notify_all();
  }

  /* The descriptor closed on the collector's thread; nothing for a negative
   * one. */
  void Close(int fd) {
    if (fd < 0)
      return;
    Post([fd] { close(fd); });
  }

  /* Waits until every job posted so far has run. */
  void Flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !running_; });
  }

  /* Jobs posted and not yet run, for the dump. */
  size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size() + (running_ ? 1 : 0);
  }

 private:
  void Loop() {
#if defined(__APPLE__) /* the workstation the tests run on */
    pthread_setname_np(name_.substr(0, 15).c_str());
#else
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
#endif
#if !defined(__APPLE__)
    /* Started from whichever thread posts first -- a FIFO one, as often
     * as not, whose policy a new thread inherits and a nice would not
     * touch. */
    struct sched_param normal = {};
    sched_setscheduler(0, SCHED_OTHER, &normal);
#endif
    setpriority(PRIO_PROCESS, 0, 10);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      changed_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty())
        break; /* stopping, and nothing left */
      std::function<void()> job = std::move(jobs_.front());
      jobs_.pop_front();
      running_ = true;
      lock.unlock();
      job();
      lock.lock();
      running_ = false;
      if (jobs_.empty())
        idle_.notify_all();
    }
    idle_.notify_all();
  }

  const std::string name_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> jobs_;
  std::thread thread_;
  bool running_ = false;
  bool stopping_ = false;
};

}  // namespace android::drm_hwcomposer
