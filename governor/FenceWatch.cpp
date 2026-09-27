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

#include "governor/FenceWatch.h"

#include <unistd.h>

namespace android::hwc::governor::tegra {

FenceWatch::~FenceWatch() {
  DropAll();
}

void FenceWatch::Add(int fd, uint64_t seq, int64_t now_ns) {
  /* Reported is reported, descriptor or not: a merge whose fence could
   * not be copied still went to the engine. */
  if (seq > latest_seq_)
    latest_seq_ = seq;
  if (fd < 0)
    return;
  watched_.push_back(Watched{fd, seq, now_ns});
}

void FenceWatch::AppendPollSet(std::vector<struct pollfd> *fds) const {
  for (const Watched &w : watched_)
    fds->push_back(pollfd{w.fd, POLLIN, 0});
}

int64_t FenceWatch::NextGiveUpNs(int64_t patience_ns) const {
  int64_t earliest = 0;
  for (const Watched &w : watched_) {
    const int64_t at = w.since_ns + patience_ns;
    if (earliest == 0 || at < earliest)
      earliest = at;
  }
  return earliest;
}

bool FenceWatch::Judge(const std::vector<struct pollfd> &fds,
                       size_t first_slot, int64_t now_ns,
                       int64_t patience_ns) {
  bool any_done = false;
  std::vector<Watched> kept;
  for (size_t i = 0; i < watched_.size(); ++i) {
    const size_t slot = first_slot + i;
    const bool signaled =
        slot < fds.size() &&
        (fds[slot].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
    const bool given_up = now_ns - watched_[i].since_ns >= patience_ns;
    if (signaled || given_up) {
      close(watched_[i].fd);
      any_done = true;
    } else {
      kept.push_back(watched_[i]);
    }
  }
  watched_.swap(kept);
  return any_done;
}

void FenceWatch::DropAll() {
  for (const Watched &w : watched_)
    close(w.fd);
  watched_.clear();
}

}  // namespace android::hwc::governor::tegra
