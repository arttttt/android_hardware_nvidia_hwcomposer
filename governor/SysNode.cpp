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

#include "governor/SysNode.h"

#include <stdlib.h>
#include <unistd.h>

namespace android::hwc::governor::tegra {

std::optional<uint64_t> ReadNumber(int fd) {
  if (fd < 0)
    return std::nullopt;
  char text[32];
  const ssize_t n = pread(fd, text, sizeof(text) - 1, 0);
  if (n <= 0)
    return std::nullopt;
  text[n] = '\0';
  char *end = nullptr;
  const unsigned long long value = strtoull(text, &end, 10);
  if (end == text)
    return std::nullopt;
  return static_cast<uint64_t>(value);
}

bool WriteQosWord(int fd, uint32_t value) {
  const int32_t word = static_cast<int32_t>(value);
  return write(fd, &word, sizeof(word)) == static_cast<ssize_t>(sizeof(word));
}

}  // namespace android::hwc::governor::tegra
