#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ava::core {

// Preserve the current C locale's one-byte folding. This is not Unicode case folding;
// callers requiring locale-independent ASCII semantics should use ascii_lower_copy.
inline void locale_lower_in_place(std::string& value) noexcept
{
  for (char& ch : value)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

[[nodiscard]] inline std::string locale_lower_copy(std::string_view value)
{
  std::string lowered(value);
  locale_lower_in_place(lowered);
  return lowered;
}

}  // namespace ava::core
