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

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace android::drm_hwcomposer {

/* The work one engine is asked for, between the thread that plans a frame
 * and the thread that hands it to the engine.
 *
 * The composer's engines -- the image engine merging a group, the GPU
 * drawing the client range -- do the same thing on different hardware:
 * take a planned picture, draw it into a buffer of the composer's own,
 * hand back the fence the drawing signals. This is the part of that which
 * is not the hardware's: a job is queued when the frame is validated, a
 * worker of the engine's own takes it and submits it, and the frame's
 * present collects the outcome. The engine's worker never runs on the
 * binder thread the client waits on.
 *
 * A job not yet submitted is dropped when a newer one arrives, if it says
 * it may be: the newer picture supersedes it, and the frame that wanted
 * the older shows the last one drawn instead -- one frame repeated in
 * time rather than one frame late, which would make every frame after it
 * late too. Never two in a row: the second time, the older is drawn after
 * all, so a stream of jobs that each arrive too late still moves. Once a
 * job is submitted it is past dropping; the hardware has it.
 *
 * `Job` is what the worker draws; `Result` what the drawing gives back.
 * Neither is looked into here, and nothing here is Android's, so the same
 * queue serves any engine and is tested on its own.
 */
template <typename Job, typename Result>
class JobQueue {
 public:
  using Id = uint64_t;

  enum class State {
    kQueued,      /* waiting for the worker */
    kSubmitting,  /* the worker has it */
    kSubmitted,   /* handed to the engine; the result is its outcome */
    kRefused,     /* the engine would not take it */
    kDropped,     /* superseded before the worker took it */
    kCancelled,   /* no longer wanted by the frame that queued it */
  };

  struct Outcome {
    State state;
    /* Set for kSubmitted, and for kRefused when the worker said why. */
    std::optional<Result> result;
  };

  /* After this many drops in a row the next superseded job is drawn
   * anyway. */
  static constexpr uint32_t kMostDroppedInARow = 1;

  /* Queues `job`; never waits. Jobs still queued that may be dropped are
   * dropped, within the bound above. */
  Id Queue(Job job, bool droppable) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry &e : entries_) {
      if (e.state != State::kQueued || !e.droppable)
        continue;
      if (dropped_in_a_row_ >= kMostDroppedInARow)
        break;
      e.state = State::kDropped;
      e.job.reset();
      dropped_in_a_row_++;
      dropped_++;
    }
    const Id id = ++last_id_;
    entries_.push_back(Entry{id, State::kQueued, droppable, std::move(job),
                             std::nullopt});
    changed_.notify_all();
    return id;
  }

  /* The worker's side: the oldest queued job, marked as taken; nothing once
   * the queue is stopped. Waits for one. */
  std::optional<std::pair<Id, Job>> Take() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (stopped_)
        return std::nullopt;
      for (Entry &e : entries_) {
        if (e.state != State::kQueued)
          continue;
        e.state = State::kSubmitting;
        std::pair<Id, Job> taken{e.id, std::move(*e.job)};
        e.job.reset();
        return taken;
      }
      changed_.wait(lock);
    }
  }

  /* The worker's side: how the job it took went. */
  void Done(Id id, bool submitted, std::optional<Result> result) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry *e = Find(id);
    if (e == nullptr)
      return;
    if (submitted)
      dropped_in_a_row_ = 0;
    if (e->cancelled) {
      Forget(id);
      return;
    }
    e->state = submitted ? State::kSubmitted : State::kRefused;
    e->result = std::move(result);
    changed_.notify_all();
  }

  /* The frame's side, at present: the job's outcome, and the job forgotten.
   * With `wait`, a job still queued or being submitted is waited for; a
   * dropped one answers at once either way. Without it, a job still queued
   * that may be dropped is dropped now -- the frame will not wait for it --
   * and one being submitted is waited for, there being no taking it back. */
  Outcome Resolve(Id id, bool wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      Entry *e = Find(id);
      if (e == nullptr)
        return Outcome{State::kCancelled, std::nullopt};
      if (e->state == State::kQueued && !wait && e->droppable &&
          dropped_in_a_row_ < kMostDroppedInARow) {
        e->state = State::kDropped;
        e->job.reset();
        dropped_in_a_row_++;
        dropped_++;
      }
      if (e->state != State::kQueued && e->state != State::kSubmitting) {
        Outcome out{e->state, std::move(e->result)};
        Forget(id);
        return out;
      }
      if (stopped_) {
        Forget(id);
        return Outcome{State::kCancelled, std::nullopt};
      }
      changed_.wait(lock);
    }
  }

  /* The frame's side: the job is not wanted any more. One the worker has
   * taken runs to its end, but its outcome is not kept. */
  void Cancel(Id id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry *e = Find(id);
    if (e == nullptr)
      return;
    if (e->state == State::kSubmitting) {
      e->cancelled = true;
      return;
    }
    Forget(id);
  }

  /* No more jobs are taken; the worker's Take answers nothing, and whoever
   * waits in Resolve is answered. Jobs not taken are let go. */
  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    for (Entry &e : entries_)
      if (e.state == State::kQueued)
        e.job.reset();
    changed_.notify_all();
  }

  /* For the dump and the governor. */
  uint64_t dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }
  size_t waiting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const Entry &e : entries_)
      if (e.state == State::kQueued)
        n++;
    return n;
  }

 private:
  struct Entry {
    Id id;
    State state;
    bool droppable;
    std::optional<Job> job;
    std::optional<Result> result;
    bool cancelled = false;
  };

  Entry *Find(Id id) {
    for (Entry &e : entries_)
      if (e.id == id)
        return &e;
    return nullptr;
  }

  /* An entry goes once its frame has its outcome, or has given it up; a
   * cancelled one the worker had goes when the worker is done with it. */
  void Forget(Id id) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->id == id) {
        entries_.erase(it);
        return;
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Entry> entries_;
  Id last_id_ = 0;
  uint32_t dropped_in_a_row_ = 0;
  uint64_t dropped_ = 0;
  bool stopped_ = false;
};

}  // namespace android::drm_hwcomposer
