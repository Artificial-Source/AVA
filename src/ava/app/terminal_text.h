#pragma once

#include <string>
#include <string_view>

namespace ava::app {

// Replace terminal control bytes and malformed UTF-8 without changing valid
// printable UTF-8. This app-owned contract is shared by all frontends that
// write untrusted text to a terminal.
[[nodiscard]] std::string sanitize_terminal_text(std::string_view text);

}  // namespace ava::app
