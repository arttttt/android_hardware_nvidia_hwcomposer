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

#include <optional>

/* The two ways the governor talks to the kernel besides the engine's own
 * descriptor: a sysfs attribute read for a number, and a pm_qos node
 * written a request. */

namespace android::hwc::governor::tegra {

/* The number at the start of a sysfs attribute held open: re-read from
 * offset nought, since the attribute is regenerated on every read. Empty
 * if it could not be read. */
std::optional<uint64_t> ReadNumber(int fd);

/* A request written to a pm_qos node held open: exactly four bytes, which
 * the node takes as a binary word -- anything else it parses as
 * hexadecimal text, which is not what a rate written in decimal means. */
bool WriteQosWord(int fd, uint32_t value);

}  // namespace android::hwc::governor::tegra
