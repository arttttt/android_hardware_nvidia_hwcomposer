/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "fd.h"

#include <fcntl.h>  // IWYU pragma: keep
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "utils/log.h"

namespace android::drm_hwcomposer {

static void CloseFd(const int *fd) {
  if (fd != nullptr) {
    if (*fd >= 0)
      close(*fd);

    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    delete fd;
  }
}

auto MakeUniqueFd(int fd) -> UniqueFd {
  if (fd < 0)
    return {nullptr, CloseFd};

  return {new int(fd), CloseFd};
}

auto MakeSharedFd(int fd) -> SharedFd {
  if (fd < 0)
    return {nullptr, CloseFd};

  return {new int(fd), CloseFd};
}

auto DupFd(SharedFd const &fd) -> int {
  /* No descriptor to copy is an ordinary answer, not a failure: a layer whose
   * fence has already come due is handed out as -1. */
  if (!fd)
    return -1;

  /* dup(), not fcntl(F_DUPFD_CLOEXEC), and the two are not interchangeable in
   * practice. fcntl has been observed to refuse a descriptor a driver has
   * only just handed back, while dup() on that same descriptor in that same
   * moment succeeds -- and inserting a single log line between the two made
   * the refusal go away, which is the signature of something in the kernel
   * not yet being finished. Upstream reported it, could not get to the
   * bottom of it, and settled on dup() everywhere.
   *
   * Nothing here needs the flag set atomically with the copy -- this process
   * never execs -- so close-on-exec is a second step, and a failure to set it
   * is worth saying but not worth losing the fence over.
   */
  const int dup_fd = dup(*fd);
  if (dup_fd < 0) {
    ALOGE("Cannot copy a fence descriptor: %s", strerror(errno));
    return -1;
  }

  // NOLINTNEXTLINE(misc-include-cleaner)
  if (fcntl(dup_fd, F_SETFD, FD_CLOEXEC) < 0) {
    ALOGW("Cannot mark a copied fence descriptor close-on-exec: %s",
          strerror(errno));
  }

  return dup_fd;
}

}  // namespace android::drm_hwcomposer
