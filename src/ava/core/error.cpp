#include "sys.h"
#include "ava/core/error.h"

#include <sstream>

namespace ava::core {

Error::Error(ErrorCategory category, std::string message, ErrorCode code) : category_(category), code_(code), message_(std::move(message))
{
}

ErrorCode Error::code() const noexcept
{
  return code_;
}

ErrorCategory Error::category() const noexcept
{
  return category_;
}

std::string const& Error::message() const noexcept
{
  return message_;
}

std::vector<ErrorContext> const& Error::context() const noexcept
{
  return context_;
}

Error& Error::with_context(std::string key, std::string value)
{
  context_.push_back(ErrorContext{.key = std::move(key), .value = std::move(value)});
  return *this;
}

std::string Error::format() const
{
  std::ostringstream out;
  out << to_string(category_) << ": " << message_;
  for (auto const& item : context_)
  {
    out << "\n  " << item.key << ": " << item.value;
  }
  return out.str();
}

std::string to_string(ErrorCategory category)
{
  switch (category)
  {
    case ErrorCategory::InvalidArgument:
      return "invalid_argument";
    case ErrorCategory::Io:
      return "io";
    case ErrorCategory::NotFound:
      return "not_found";
    case ErrorCategory::PermissionDenied:
      return "permission_denied";
    case ErrorCategory::Configuration:
      return "configuration";
    case ErrorCategory::Provider:
      return "provider";
    case ErrorCategory::Session:
      return "session";
    case ErrorCategory::Tool:
      return "tool";
    case ErrorCategory::Unknown:
      return "unknown";
  }
  return "unknown";
}

}  // namespace ava::core
