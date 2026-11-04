// Shares one prompt and its result between a waiting worker and the TUI.
#pragma once

#include "ava/core/result.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include "debug.h"

namespace ava::tui {

// The resolver worker, coordinator queue and active TUI prompt share this request.
// TUI callbacks in the immutable prompt may borrow the waiting worker's stack.
// begin_resolution() takes a TUI hold (the "claim"): await() needs both a result
// and release of that hold, after those callbacks can no longer run.
// Prompt and Answer select the payload; Interrupted prepares cancellation before
// enqueueing. The request owns its mutex/result and may be destroyed on either
// thread after the last share is released. Workers never read screen views.
template <typename Prompt, typename Answer, ava::core::Result<Answer> (*Interrupted)()>
class PendingRequest final
{
 public:
  // Worker thread, before enqueue. Allocation here may fail without leaving an
  // queued waiter behind; later cancellation only moves the prepared result.
  explicit PendingRequest(Prompt prompt) : prompt_(std::move(prompt)), interrupted_(Interrupted()) { }
  // Not copyable: each worker waits on this one synchronization identity.
  PendingRequest(PendingRequest const&) = delete;
  PendingRequest& operator=(PendingRequest const&) = delete;

  // Either participant may read while retaining shared ownership of this request.
  [[nodiscard]] Prompt const& prompt() const noexcept { return prompt_; }
  // Exactly one resolver worker waits. Return only after a result exists AND the
  // UI has retired callbacks that may borrow this worker's stack. Consume once.
  [[nodiscard]] ava::core::Result<Answer> await()
  {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return result_.has_value() && !resolving_; });
    return std::move(*result_);
  }
  // TUI thread, before using borrowed callbacks: hold an unresolved request.
  // False means a result or another hold already exists; do not start this prompt.
  [[nodiscard]] bool begin_resolution()
  {
    std::lock_guard lock(mutex_);
    if (result_ || resolving_)
      return false;
    resolving_ = true;
    return true;
  }
  // TUI thread, after callbacks retire. Record the first result, or preserve a
  // result already stored for this request. Clear the hold before waking
  // the worker, and do not keep the request mutex locked during notification.
  void complete(ava::core::Result<Answer> result)
  {
    {
      std::lock_guard lock(mutex_);
      if (!result_)
        result_ = std::move(result);
      resolving_ = false;
    }
    ready_.notify_one();
  }
  // TUI thread: cancel a queued request only while no active TUI prompt holds it.
  // The coordinator holds its queue lock so claiming cannot race this call.
  // Preserve a result already stored for this request; never replace an answer.
  void interrupt_queued()
  {
    {
      std::lock_guard lock(mutex_);
      // Do not interrupt a claimed request. Retire its callbacks, then call
      // complete() or abandon(); queued interruption must never choose its answer.
      ASSERT(!resolving_);
      interrupt_locked();
    }
    ready_.notify_one();
  }
  // TUI thread, only after borrowed callbacks retire. Release a held waiter
  // with the prepared cancellation value; no allocation occurs on this path.
  void abandon()
  {
    {
      std::lock_guard lock(mutex_);
      interrupt_locked();
      resolving_ = false;
    }
    ready_.notify_one();
  }

  // Prompts, answers and callback captures may contain arbitrary user secrets.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Caller holds mutex_. Move cancellation at most once, preserving first result.
  void interrupt_locked()
  {
    if (!result_)
      result_ = std::move(interrupted_);
  }
  // Initialized by the worker; immutable until resolver, queue and TUI shares all retire.
  Prompt const prompt_;
  // Serializes the TUI hold and result updates between worker and TUI threads.
  std::mutex mutex_;
  // Wakes the sole worker after a result/hold change; the predicate prevents early release.
  std::condition_variable ready_;
  // First completion or interruption wins under mutex_; await moves it exactly once.
  std::optional<ava::core::Result<Answer>> result_;
  // Prepared during construction, then moved once under mutex_ on cancellation.
  ava::core::Result<Answer> interrupted_;
  // TUI hold: set before using borrowed callbacks, cleared after retirement; protected by mutex_.
  bool resolving_ = false;
};

} // namespace ava::tui
