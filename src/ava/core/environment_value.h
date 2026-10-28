#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace ava::core {

// Read the current process environment and copy a nonempty value. The name must
// be NUL-terminated. This does not validate HOME or establish trusted identity.
[[nodiscard]] inline std::optional<std::string> nonempty_environment_value(char const* name)
{
  auto const* value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    return std::nullopt;
  return std::string(value);
}

}  // namespace ava::core
