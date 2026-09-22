#pragma once

#include "ava/core/error.h"
#include "ava/core/result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ava::agent {

[[nodiscard]] ava::core::VoidResult validate_provider_tool_call_id(std::string_view id);
[[nodiscard]] ava::core::Error output_limit_error(std::string message, std::string_view limit_name, std::size_t limit);
[[nodiscard]] ava::core::Error provider_event_limit_error(std::size_t limit);
[[nodiscard]] bool would_exceed(std::size_t current, std::size_t added, std::size_t limit);

}  // namespace ava::agent
