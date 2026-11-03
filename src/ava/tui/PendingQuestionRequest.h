// Question specialization of the shared worker-to-TUI prompt rendezvous.
#pragma once

#include "PendingRequest.h"
#include "ava/agent/question.h"

namespace ava::tui {

// Worker construction allocates the error before admission, so cancellation can
// release an abandoned waiter even when allocation later fails during teardown.
inline ava::core::Result<ava::agent::QuestionAnswer> interrupted_question_result()
{
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted"));
}

// Resolver and TUI share the request; its held claim keeps the worker blocked
// while question callbacks can borrow the worker's stack. See PendingRequest.
using PendingQuestionRequest = PendingRequest<ava::agent::QuestionPrompt, ava::agent::QuestionAnswer, interrupted_question_result>;

} // namespace ava::tui
