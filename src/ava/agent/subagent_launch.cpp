#include "sys.h"
#include "ava/agent/subagent_launch.h"
#include "ava/core/json.h"
#include "ava/core/utf8_prefix.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace ava::agent {
namespace {

bool forbidden_terminal_control(std::string_view value) noexcept
{
  for (std::size_t index = 0; index < value.size(); ++index)
  {
    auto const byte = static_cast<unsigned char>(value[index]);
    if ((byte < 0x20U && byte != '\t' && byte != '\n' && byte != '\r') || byte == 0x7FU)
      return true;
    // UTF-8 encodings of C1 terminal controls U+0080..U+009F.
    if (byte == 0xC2U && index + 1 < value.size())
    {
      auto const next = static_cast<unsigned char>(value[index + 1]);
      if (next >= 0x80U && next <= 0x9FU)
        return true;
    }
  }
  return false;
}

std::string normalize_display_field(std::string_view raw, std::size_t max_bytes)
{
  if (!ava::core::json::is_valid_utf8(raw) || forbidden_terminal_control(raw))
    return {};

  std::string normalized;
  normalized.reserve(std::min(raw.size(), max_bytes));
  bool pending_space = false;
  for (char ch : raw)
  {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
    {
      pending_space = !normalized.empty();
      continue;
    }
    if (pending_space)
    {
      normalized.push_back(' ');
      pending_space = false;
    }
    normalized.push_back(ch);
  }
  normalized.resize(ava::core::utf8_prefix_size(normalized, max_bytes));
  while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
  return normalized;
}

}  // namespace

SubagentLaunchDisplay SubagentLaunchDisplay::normalized(std::string_view model_display_name,
                                                         std::optional<std::string_view> explicit_reasoning_level)
{
  auto model = normalize_display_field(model_display_name, kMaxSubagentLaunchModelDisplayNameBytes);
  auto reasoning = explicit_reasoning_level ? normalize_display_field(*explicit_reasoning_level, kMaxSubagentLaunchReasoningLabelBytes) : std::string{};
  if (reasoning.empty())
    reasoning = "default";
  return SubagentLaunchDisplay(std::move(model), std::move(reasoning));
}

}  // namespace ava::agent
