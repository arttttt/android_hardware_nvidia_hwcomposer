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

#ifndef UTILS_UNIQUE_FD_H
#define UTILS_UNIQUE_FD_H

#include <unistd.h>

#include <utility>

namespace android {
namespace hwc {

/* Owning wrapper for a file descriptor.
 *
 * Every fence that crosses this HAL is a file descriptor, and the composer
 * this one replaces fails precisely by losing track of who owns them. So
 * ownership is expressed in the type rather than in a comment: a descriptor
 * is moved, never copied, and closing is the destructor's job.
 *
 * -1 is the empty value throughout, matching what the HWC2 API uses for
 * "no fence".
 */
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd): mFd(fd) {}

    UniqueFd(UniqueFd &&other) noexcept: mFd(other.release()) {}

    UniqueFd &operator=(UniqueFd &&other) noexcept {
        reset(other.release());
        return *this;
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    ~UniqueFd() { reset(); }

    int get() const { return mFd; }
    explicit operator bool() const { return mFd >= 0; }

    /* Hands the descriptor to the caller, who becomes responsible for it. */
    int release() {
        int fd = mFd;
        mFd = -1;
        return fd;
    }

    void reset(int fd = -1) {
        if (mFd >= 0 && mFd != fd)
            close(mFd);
        mFd = fd;
    }

    /* A second descriptor for the same object. Needed where a fence has to
     * be both kept and handed out, which is the usual shape of a release
     * fence. Returns an empty UniqueFd if there is nothing to duplicate. */
    UniqueFd dup() const {
        return UniqueFd(mFd >= 0 ? ::dup(mFd) : -1);
    }

private:
    int mFd = -1;
};

}  // namespace hwc
}  // namespace android

#endif  // UTILS_UNIQUE_FD_H
