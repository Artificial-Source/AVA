#pragma once

#include "ava/debug/print_members_on.h"

#include <string>
#include <utility>
#include <vector>

namespace ava::core {

enum class ErrorCategory
{
  InvalidArgument,
  Io,
  NotFound,
  PermissionDenied,
  Configuration,
  Provider,
  Session,
  Tool,
  Unknown,
};

// Internal control-flow identity; deliberately absent from format() and wire formats.
enum class ErrorCode
{
  Unspecified,
  Canceled,
  ProviderEventLimit,
  BodyOutputLimit,
};

struct ErrorContext
{
  std::string key;
  std::string value;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class Error
{
 public:
  Error(ErrorCategory category, std::string message, ErrorCode code = ErrorCode::Unspecified);

  [[nodiscard]] ErrorCode code() const noexcept;
  [[nodiscard]] ErrorCategory category() const noexcept;
  [[nodiscard]] std::string const& message() const noexcept;
  [[nodiscard]] std::vector<ErrorContext> const& context() const noexcept;
  [[nodiscard]] std::string format() const;

  Error& with_context(std::string key, std::string value);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  ErrorCategory category_;
  ErrorCode code_;
  std::string message_;
  std::vector<ErrorContext> context_;
};

[[nodiscard]] std::string to_string(ErrorCategory category);

}  // namespace ava::core
