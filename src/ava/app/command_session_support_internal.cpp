#include "sys.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/runtime/Session.h"
#include "ava/core/text_case.h"

#include <cctype>
#include <utility>

namespace ava::app::session_command_support {

std::string trim_ascii(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
  return std::string(text);
}

bool contains_ascii_case_insensitive(std::string_view text, std::string_view query)
{
  if (query.empty())
    return true;
  return ava::core::locale_lower_copy(text).find(ava::core::locale_lower_copy(query)) != std::string::npos;
}

ava::core::Result<std::vector<ava::session::SessionEntry>> load_runtime_entries(runtime::session_ts const& unlocked_session)
{
  auto read_authority = runtime::session_ts::crat(unlocked_session)->read_authority_1();
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  return read_authority->load();
}

std::string labels_text(std::vector<std::string> const& labels)
{
  std::string text;
  for (std::size_t index = 0; index < labels.size(); ++index)
  {
    if (index > 0)
      text += ",";
    text += labels[index];
  }
  return text;
}

std::string shorten_middle(std::string text, std::size_t max_columns)
{
  if (text.size() <= max_columns || max_columns < 8)
    return text;
  auto const front = (max_columns - 3) / 2;
  auto const back = max_columns - 3 - front;
  return text.substr(0, front) + "..." + text.substr(text.size() - back);
}

ava::core::Result<runtime::session_ts> reopen_session(runtime::session_ts const& unlocked_current, std::string_view session_id)
{
  auto current_context = runtime::session_ts::crat(unlocked_current)->replacement_open_context({});
  runtime::SessionLifecycleRequest request;
  request.requested_session_id = std::string(session_id);
  return runtime::Session::open(current_context, request);
}

}  // namespace ava::app::session_command_support
