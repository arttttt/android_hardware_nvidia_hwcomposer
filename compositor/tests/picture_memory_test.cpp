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

/* Host test for the memory of drawn pictures.
 *
 *   clang++ -std=c++17 -Wall -Wextra -I. \
 *       compositor/tests/picture_memory_test.cpp compositor/PictureMemory.cpp \
 *       -o /tmp/picture_memory_test && /tmp/picture_memory_test */

#include <cstdio>

#include "compositor/PictureMemory.h"

using namespace android::drm_hwcomposer;
using Verdict = PictureMemory::Verdict;
using Member = PictureMemory::Member;

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      failures++;                                                 \
    }                                                             \
  } while (0)

/* The recents card's two halves, as the GPU executor draws them. */
std::vector<Member> Card() {
  Member left;
  left.source = 101;
  left.generation = 7;
  left.src = {0, 0, 768, 1940};
  left.dst = {230, 331, 768, 1689};
  left.opaque = true;
  left.corners = {230, 331, 1305, 1689, 3.6F};
  Member right = left;
  right.source = 102;
  right.dst = {768, 331, 1305, 1689};
  return {left, right};
}

/* Nothing remembered at first, and after a forgotten frame. */
void TestFirstSightAndForget() {
  PictureMemory m;
  CHECK(m.Judge(0, 1536, 2048, Card()) == Verdict::kFirstSight);
  m.Remember(0, 1536, 2048, Card());
  CHECK(m.Judge(0, 1536, 2048, Card()) == Verdict::kSame);
  m.Forget();
  CHECK(m.Judge(0, 1536, 2048, Card()) == Verdict::kFirstSight);
}

/* The same members over an idle overview: the same picture. */
void TestSame() {
  PictureMemory m;
  m.Remember(0, 1536, 2048, Card());
  CHECK(m.Judge(0, 1536, 2048, Card()) == Verdict::kSame);
}

/* Each way a picture can differ is told apart, the client drawing into the
 * same buffer again first among them. */
void TestEachChange() {
  PictureMemory m;
  m.Remember(0, 1536, 2048, Card());

  auto c = Card();
  c[0].generation++;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedFrame);

  c = Card();
  c[1].source = 103;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedIdentity);

  c = Card();
  c[0].dst.left = 229;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedGeometry);

  c = Card();
  c[0].src.bottom = 1939.5F;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedGeometry);

  c = Card();
  c[1].alpha = 0.99F;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedBlend);

  c = Card();
  c[0].transform = 1;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedTransform);

  c = Card();
  c[0].corners[4] = 3.5F;
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedEffects);

  c = Card();
  c.pop_back();
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kChangedShape);
  CHECK(m.Judge(1, 1536, 2048, Card()) == Verdict::kChangedShape);
  CHECK(m.Judge(0, 1536, 1024, Card()) == Verdict::kChangedSize);
}

/* A member with no name is never recognised; a solid colour is its colour. */
void TestNamelessAndSolid() {
  PictureMemory m;
  auto c = Card();
  c[0].source = 0;
  m.Remember(0, 1536, 2048, c);
  CHECK(m.Judge(0, 1536, 2048, c) == Verdict::kNameless);

  Member dim;
  dim.solid = true;
  dim.color = {0, 0, 0, 0.6F};
  dim.dst = {0, 0, 1536, 2048};
  m.Remember(0, 1536, 2048, {dim});
  CHECK(m.Judge(0, 1536, 2048, {dim}) == Verdict::kSame);
  dim.color[3] = 0.5F;
  CHECK(m.Judge(0, 1536, 2048, {dim}) == Verdict::kChangedIdentity);
}

}  // namespace

/* A newer frame of the same layers, whichever of their buffers it came in,
 * is the remembered picture but for frames; anything else is not. */
void TestSameButFrames() {
  std::vector<Member> was = Card();
  was[0].serial = 11;
  was[1].serial = 12;
  PictureMemory m;
  CHECK(!m.SameButFrames(3, 1536, 2048, was));
  m.Remember(3, 1536, 2048, was);

  std::vector<Member> now = was;
  now[1].source = 202;
  now[1].generation = 8;
  CHECK(m.Judge(3, 1536, 2048, now) == Verdict::kChangedIdentity);
  CHECK(m.SameButFrames(3, 1536, 2048, now));

  now = was;
  now[0].generation = 9;
  CHECK(m.SameButFrames(3, 1536, 2048, now));

  now = was;
  now[0].dst.top += 1;
  CHECK(!m.SameButFrames(3, 1536, 2048, now));

  now = was;
  now[1].serial = 13;
  CHECK(!m.SameButFrames(3, 1536, 2048, now));

  now = was;
  now[0].alpha = 0.5F;
  CHECK(!m.SameButFrames(3, 1536, 2048, now));

  CHECK(!m.SameButFrames(4, 1536, 2048, was));
  CHECK(!m.SameButFrames(3, 1536, 1024, was));
  CHECK(!m.SameButFrames(3, 1536, 2048, {was[0]}));

  /* A layer with no serial cannot be followed across its buffers. */
  std::vector<Member> unnamed = was;
  unnamed[0].serial = 0;
  PictureMemory n;
  n.Remember(3, 1536, 2048, unnamed);
  CHECK(!n.SameButFrames(3, 1536, 2048, unnamed));
}

int main() {
  TestFirstSightAndForget();
  TestSame();
  TestEachChange();
  TestNamelessAndSolid();
  TestSameButFrames();
  if (failures == 0)
    std::printf("picture_memory_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
