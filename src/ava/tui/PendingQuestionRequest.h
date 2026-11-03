#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/agent/question.h"
#include "ava/core/result.h"

#include <condition_variable>
#include <mutex>
#include <optional>

namespace ava::tui {

// Worker and coordinator share the request until its UI resolution retires.
// In particular, auto_resolve may borrow worker state that must remain alive
// until await returns; interruption cannot release a claimed waiter early.
class PendingQuestionRequest final
{
 public:
  explicit PendingQuestionRequest(ava::agent::QuestionPrompt prompt);
  PendingQuestionRequest(PendingQuestionRequest const&) = delete;
  PendingQuestionRequest& operator=(PendingQuestionRequest const&) = delete;

  [[nodiscard]] ava::agent::QuestionPrompt const& prompt() const noexcept;
  // Exactly one resolver worker consumes this request's result.
  [[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> await();
  [[nodiscard]] bool begin_resolution();
  void complete(ava::core::Result<ava::agent::QuestionAnswer> result);
  void interrupt();

  // Question prompts and answers may contain secret or arbitrary user text.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ava::agent::QuestionPrompt const prompt_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<ava::core::Result<ava::agent::QuestionAnswer>> result_;
  bool resolving_ = false;
};

}  // namespace ava::tui
