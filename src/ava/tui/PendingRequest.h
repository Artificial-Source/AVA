// Shared prompt rendezvous used by the TUI architecture's worker-to-UI handoff.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace ava::tui {

// A resolver worker and the prompt coordinator share one request. The immutable
// prompt may borrow worker-local callbacks, so a result alone cannot release the
// worker: the UI must also retire its claim. Prompt and Answer select the payload;
// Interrupted prepares the feature's cancellation result before queue admission.
template <typename Prompt, typename Answer, ava::core::Result<Answer> (*Interrupted)()>
class PendingRequest final
{
 public:
  // Worker thread, before enqueue. Allocation here may fail without leaving an
  // admitted waiter behind; later cancellation only moves the prepared result.
  explicit PendingRequest(Prompt prompt) : prompt_(std::move(prompt)), interrupted_(Interrupted()) { }
  // A synchronization identity must not be duplicated while either side waits.
  PendingRequest(PendingRequest const&) = delete;
  // A live rendezvous cannot be replaced by another request's synchronization state.
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
  // TUI thread, after callbacks retire. Preserve any earlier cancellation result,
  // release the claim, then wake the worker without retaining the request mutex.
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
  // Either thread may cancel a queued request. A claimed request keeps the worker
  // blocked until complete/abandon retires the claim; cancellation wins only once.
  void interrupt()
  {
    {
      std::lock_guard lock(mutex_);
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
  // Initialized by the worker; immutable until both shared owners release it.
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
