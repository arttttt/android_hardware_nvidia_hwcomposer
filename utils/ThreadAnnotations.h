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

#include <android-base/thread_annotations.h>

#include <mutex>

/* One class this platform's thread annotations do not have yet.
 *
 * Where a lock is taken through a unique_lock the compiler's thread-safety
 * analysis loses track of it, and this is what tells the analysis the lock is
 * held for the rest of the scope. It generates no code and checks nothing at
 * run time -- it exists so that a warning about reading a guarded member is
 * not raised where the member is in fact guarded.
 *
 * The macros it is written in terms of are all here; only the class arrived
 * in a later release. It is declared where the platform would declare it, so
 * that the code using it needs no change beyond finding this file.
 */
namespace android {
namespace base {

class SCOPED_CAPABILITY ScopedLockAssertion {
public:
    explicit ScopedLockAssertion(std::mutex &mutex) ACQUIRE(mutex) {
        (void)mutex;
    }

    ~ScopedLockAssertion() RELEASE() = default;

    ScopedLockAssertion(const ScopedLockAssertion &) = delete;
    ScopedLockAssertion &operator=(const ScopedLockAssertion &) = delete;
};

}  // namespace base
}  // namespace android
