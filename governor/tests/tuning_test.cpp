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

/* The powergate delay and the tuning file, checked on a workstation:
 *
 *     c++ -std=c++17 -I. -Igovernor/tests/stub governor/tests/tuning_test.cpp \
 *         governor/Tuning.cpp governor/Log.cpp -o /tmp/tuning_test && \
 *         /tmp/tuning_test
 *
 * The kernel's node holds "500\n" on the mocha; the rest are the ways a
 * file can fail to be that. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "governor/HwcGovernor.h"
#include "governor/Tuning.h"

using namespace android::hwc::governor;
using namespace android::hwc::governor::tegra;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      failures++;                                                       \
    }                                                                   \
  } while (0)

class QuietHost : public GovernorHost {
 public:
  int WarmEngine() override {
    return -1;
  }
  void TraceInt(const char *, int32_t) override {
  }
  void Log(int, const char *) override {
  }
};

std::string Write(const char *name, const char *text) {
  std::string path = std::string("/tmp/hwc_tuning_test_") + name;
  FILE *f = fopen(path.c_str(), "w");
  fputs(text, f);
  fclose(f);
  return path;
}

uint32_t DelayFrom(const char *text, bool *read) {
  Tuning t;
  t.powergate_ms = 777;
  *read = ReadPowergateDelay(Write("delay", text).c_str(), &t);
  return t.powergate_ms;
}

void TestDelayParsed() {
  bool read = false;
  CHECK(DelayFrom("500\n", &read) == 500 && read);
  CHECK(DelayFrom("1500", &read) == 1500 && read);
  CHECK(DelayFrom("  25 \n", &read) == 25 && read);
  /* Zero is the engine powered down at once, a real setting. */
  CHECK(DelayFrom("0\n", &read) == 0 && read);
}

void TestDelayRejected() {
  bool read = true;
  const char *bad[] = {"", "\n", "abc\n", "-5\n", "+5\n", "12ms\n",
                       "99999999999\n"};
  for (const char *text : bad) {
    CHECK(DelayFrom(text, &read) == 777);
    CHECK(!read);
  }

  Tuning t;
  t.powergate_ms = 777;
  CHECK(!ReadPowergateDelay("/tmp/hwc_tuning_test_no_such_file", &t));
  CHECK(t.powergate_ms == 777);
}

/* The order the governor reads them in: the kernel first, the tuning
 * file over it, so a line in the file still has the last word. */
void TestTuningFileOverridesKernel() {
  QuietHost host;
  Tuning t;
  CHECK(ReadPowergateDelay(Write("delay", "1500\n").c_str(), &t));
  CHECK(ReadTuningFile(Write("conf", "powergate_ms=900\n").c_str(), &t, host));
  CHECK(t.powergate_ms == 900);

  Tuning u;
  CHECK(ReadPowergateDelay(Write("delay", "1500\n").c_str(), &u));
  CHECK(ReadTuningFile(Write("conf", "warm_ahead=1\n").c_str(), &u, host));
  CHECK(u.powergate_ms == 1500);
}

}  // namespace

int main() {
  TestDelayParsed();
  TestDelayRejected();
  TestTuningFileOverridesKernel();
  unlink("/tmp/hwc_tuning_test_delay");
  unlink("/tmp/hwc_tuning_test_conf");
  if (failures == 0)
    printf("tuning_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
