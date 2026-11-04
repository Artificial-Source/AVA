// Shared prompt rendezvous used by the TUI architecture's worker-to-UI handoff.
#pragma once

#include "ava/core/result.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include "debug.h"

namespace ava::tui {

// A resolver worker and the prompt coordinator share one request. The immutable
// prompt may borrow worker-local callbacks, so a result alone cannot release the
// worker: the UI must also retire its claim. Prompt and Answer select the payload;
// Interrupted prepares the feature's cancellation result before queue admission.
// The request owns its mutex/result; the resolver, queue and TUI claim own shares.
// Only the TUI holds a claim, and final destruction can occur on either participant
// after all shares are released. No UI callback may run after that claim retires.
template <typename Prompt, typename Answer, ava::core::Result<Answer> (*Interrupted)()>
class PendingRequest final
{
 public:
  // Worker thread, before enqueue. Allocation here may fail without leaving an
  // admitted waiter behind; later cancellation only moves the prepared result.
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
  // TUI thread, before constructing callbacks. Claim only an unresolved request;
  // failure means cancellation already won and the prompt must not be displayed.
  [[nodiscard]] bool begin_resolution()
  {
    std::lock_guard lock(mutex_);
    if (result_ || resolving_)
      return false;
    resolving_ = true;
    return true;
  }
  // TUI thread, after callbacks retire. Record the first result, or preserve a
  // result already stored for a retired request. Clear the claim before waking
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
  // TUI thread: cancel a queued request only while no TUI claim holds it.
  // The coordinator holds its queue lock so claiming cannot race this call.
  // Preserve a result already stored for a retired request; never replace an answer.
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
  // TUI thread, only after borrowed callbacks retire. Release a claimed waiter
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
  // Serializes claim and result transitions between worker and TUI threads.
  std::mutex mutex_;
  // Wakes the sole worker after a result/claim transition; predicate prevents early release.
  std::condition_variable ready_;
  // First completion or interruption wins under mutex_; await moves it exactly once.
  std::optional<ava::core::Result<Answer>> result_;
  // Prepared during construction, then moved once under mutex_ on cancellation.
  ava::core::Result<Answer> interrupted_;
  // Set by UI claim, cleared only after callbacks retire; protected by mutex_.
  bool resolving_ = false;
};

} // namespace ava::tui
