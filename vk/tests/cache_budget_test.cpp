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

/* Host test for the image cache's budget.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. vk/tests/cache_budget_test.cpp \
 *       -o /tmp/vk_cache_budget_test && /tmp/vk_cache_budget_test
 */

#include <cstdio>

#include "vk/CacheBudget.h"

using namespace android::hwc::vk;

namespace {

int failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                            \
    }                                                        \
  } while (0)

constexpr size_t kScreen = size_t{1536} * 2048 * 4;

/* Bytes bound before the count does for full-screen buffers: ten fit,
 * the eleventh does not; small ones run into the count instead. */
void TestOver() {
  const CacheBudget b;
  CHECK(!b.Over(0, 0, kScreen));
  CHECK(!b.Over(9, 9 * kScreen, kScreen));
  CHECK(b.Over(10, 10 * kScreen, kScreen));
  CHECK(!b.Over(62, 62 * 4096, 4096));
  CHECK(b.Over(64, 64 * 4096, 4096));
  /* One buffer over the whole budget is over even in an empty cache:
   * the caller lets everything go and takes it anyway. */
  CHECK(b.Over(0, 0, b.most_bytes + 1));
}

/* The least recently asked for goes; what the frame under way asked
 * for stays, even when it is all there is. */
void TestVictim() {
  CHECK(!PickVictim({}, 0).has_value());
  const std::vector<CacheUse> uses = {{5}, {2}, {9}, {7}};
  CHECK(PickVictim(uses, 100) == size_t{1});
  CHECK(PickVictim(uses, 3) == size_t{1});
  CHECK(PickVictim(uses, 2) == std::nullopt);
  /* With the oldest in the frame's hands -- it was asked for again -- the
   * next oldest outside goes. */
  const std::vector<CacheUse> asked_again = {{5}, {12}, {9}, {7}};
  CHECK(PickVictim(asked_again, 10) == size_t{0});
}

}  // namespace

int main() {
  TestOver();
  TestVictim();
  if (failures == 0)
    std::printf("cache_budget_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
