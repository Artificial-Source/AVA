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

// Shared ownership keeps the prompt's worker-borrowed callbacks alive until retirement.
using PendingQuestionRequest = PendingRequest<ava::agent::QuestionPrompt, ava::agent::QuestionAnswer, interrupted_question_result>;

} // namespace ava::tui
