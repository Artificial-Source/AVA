#include "sys.h"
#include "PendingQuestionRequest.h"

#include <utility>

namespace ava::tui {

PendingQuestionRequest::PendingQuestionRequest(ava::agent::QuestionPrompt prompt) : prompt_(std::move(prompt))
{
}

ava::agent::QuestionPrompt const& PendingQuestionRequest::prompt() const noexcept
{
  return prompt_;
}

ava::core::Result<ava::agent::QuestionAnswer> PendingQuestionRequest::await()
{
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [this]() { return result_.has_value() && !resolving_; });
  return std::move(*result_);
}

bool PendingQuestionRequest::begin_resolution()
{
  std::lock_guard lock(mutex_);
  if (result_ || resolving_)
    return false;
  resolving_ = true;
  return true;
}

void PendingQuestionRequest::complete(ava::core::Result<ava::agent::QuestionAnswer> result)
{
  {
    std::lock_guard lock(mutex_);
    if (!result_)
      result_ = std::move(result);
    resolving_ = false;
  }
  ready_.notify_one();
}

void PendingQuestionRequest::interrupt()
{
  {
    std::lock_guard lock(mutex_);
    if (!result_)
      result_ = std::move(interrupted_);
  }
  ready_.notify_one();
}

void PendingQuestionRequest::abandon()
{
  {
    std::lock_guard lock(mutex_);
    if (!result_)
      result_ = std::move(interrupted_);
    resolving_ = false;
  }
  ready_.notify_one();
}

}  // namespace ava::tui
