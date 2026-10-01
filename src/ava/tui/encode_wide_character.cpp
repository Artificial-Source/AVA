#include "encode_wide_character.h"
#include <climits>
#include <cwchar>

namespace ava::tui::runtime_input {

std::optional<std::string> encode_wide_character(wchar_t character)
{
  std::mbstate_t state{};
  char buffer[MB_LEN_MAX]{};
  auto const length = std::wcrtomb(buffer, character, &state);
  if (length == static_cast<std::size_t>(-1))
    return std::nullopt;
  return std::string(buffer, length);
}

} // namespace ava::tui::runtime_input
