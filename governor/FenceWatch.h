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

#include <poll.h>
#include <stdint.h>

#include <vector>

namespace android::hwc::governor::tegra {

/* A merge fence handed to the governor: owned here from the moment it is
 * added until it is closed. */
struct Watched {
  int fd;
  uint64_t seq;
  int64_t since_ns;
};

/* The fences of merges in flight, and how they join the thread's poll.
 *
 * A fence is done when its descriptor reports readable, or when it has
 * been waited on past the patience -- a merge the engine never finishes is
 * not a reason to hold the clock forever. Either way the descriptor is
 * closed here. */
class FenceWatch {
 public:
  FenceWatch() = default;
  ~FenceWatch();

  FenceWatch(const FenceWatch &) = delete;
  FenceWatch &operator=(const FenceWatch &) = delete;

  void Add(int fd, uint64_t seq, int64_t now_ns);

  bool empty() const { return watched_.empty(); }

  /* The highest frame number among the fences ever added, nought before
   * the first: whether a plan's merge has already been reported. */
  uint64_t latest_seq() const { return latest_seq_; }

  /* One poll slot per fence, appended in the list's order. Judge reads
   * the answers back from the same slots, so nothing may be added between
   * the two -- add after judging. */
  void AppendPollSet(std::vector<struct pollfd> *fds) const;

  /* The earliest moment a fence will be given up on, or nought with
   * nothing watched. */
  int64_t NextGiveUpNs(int64_t patience_ns) const;

  /* Closes and forgets every fence whose slot -- the first at
   * `first_slot` -- reports it due, or that has outlived the patience.
   * True if any was. */
  bool Judge(const std::vector<struct pollfd> &fds, size_t first_slot,
             int64_t now_ns, int64_t patience_ns);

  void DropAll();

 private:
  std::vector<Watched> watched_;
  uint64_t latest_seq_ = 0;
};

}  // namespace android::hwc::governor::tegra
