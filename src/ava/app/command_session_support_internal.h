#pragma once

#include "ava/app/runtime.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/session/session_metadata.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app::session_command_support {

[[nodiscard]] std::string trim_ascii(std::string_view text);
[[nodiscard]] bool contains_ascii_case_insensitive(std::string_view text, std::string_view query);
[[nodiscard]] ava::core::Result<std::vector<ava::session::SessionEntry>> load_runtime_entries(runtime::session_ts const& unlocked_session);
[[nodiscard]] std::string labels_text(std::vector<std::string> const& labels);
[[nodiscard]] std::string shorten_middle(std::string text, std::size_t max_columns);
[[nodiscard]] ava::core::Result<runtime::session_ts> reopen_session(runtime::session_ts const& unlocked_current, std::string_view session_id);

}  // namespace ava::app::session_command_support
