#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/commands.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace ava::app {

void add_output(CommandResult& result, std::string text)
{
  result.output.push_back(std::move(text));
}

CommandResult handled_text(std::string text)
{
  CommandResult result;
  result.handled = true;
  add_output(result, std::move(text));
  return result;
}

std::string display_path(std::filesystem::path const& path, std::filesystem::path const& base)
{
  std::error_code error;
  auto const relative = std::filesystem::relative(path, base, error);
  if (!error)
    return relative.generic_string();
  return path.generic_string();
}

std::string sanitize_inline_text(std::string text)
{
  for (auto& ch : text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
      ch = '?';
  }
  return text;
}

std::string joined_strings(std::vector<std::string> const& values, std::string_view separator)
{
  std::string output;
  for (auto const& value : values)
  {
    if (!output.empty())
      output += separator;
    output += value;
  }
  return output;
}

std::string missing_argument(std::string_view usage)
{
  return "usage: " + std::string(usage);
}

std::string command_argument(std::string_view line, std::string_view command)
{
  if (line.size() <= command.size() || line[command.size()] != ' ')
    return {};
  return std::string(line.substr(command.size() + 1));
}

std::vector<std::string> split_command_arguments(std::string_view text)
{
  std::vector<std::string> parts;
  std::size_t index = 0;
  while (index < text.size())
  {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) ++index;
    auto const start = index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) ++index;
    if (start < index)
      parts.emplace_back(text.substr(start, index - start));
  }
  return parts;
}

std::string_view id_payload_after_family_prefix(std::string_view id) noexcept
{
  auto const separator = id.find('_');
  if (separator == std::string_view::npos || separator + 1 >= id.size())
    return id;
  return id.substr(separator + 1);
}

std::vector<std::string> unique_short_id_refs(std::vector<std::string> const& ids, std::size_t min_length)
{
  if (min_length == 0)
    min_length = 1;

  std::vector<std::string_view> payloads;
  payloads.reserve(ids.size());
  for (auto const& id : ids)
    payloads.push_back(id_payload_after_family_prefix(id));

  auto payload_has_prefix = [](std::string_view payload, std::string_view prefix) noexcept {
    return payload.size() >= prefix.size() && payload.substr(0, prefix.size()) == prefix;
  };

  std::vector<std::string> refs(ids.size());
  for (std::size_t index = 0; index < ids.size(); ++index)
  {
    auto const payload = payloads[index];
    if (payload.empty())
    {
      // Degenerate ids keep a stable non-empty token from the full id.
      refs[index] = ids[index];
      continue;
    }

    std::size_t length = std::min(std::max(min_length, std::size_t{1}), payload.size());
    for (;;)
    {
      auto const candidate = payload.substr(0, length);
      std::size_t matches = 0;
      for (auto const other : payloads)
      {
        if (payload_has_prefix(other, candidate))
          ++matches;
      }
      if (matches == 1 || length >= payload.size())
      {
        refs[index] = std::string(candidate);
        break;
      }
      ++length;
    }
  }

  // Identical payloads (distinct full ids) cannot be abbreviated apart; fall back
  // to the full id so the displayed set never collapses two authorities together.
  for (std::size_t index = 0; index < ids.size(); ++index)
  {
    for (std::size_t other = index + 1; other < ids.size(); ++other)
    {
      if (ids[index] == ids[other] || refs[index] != refs[other])
        continue;
      refs[index] = ids[index];
      refs[other] = ids[other];
    }
  }

  return refs;
}

}  // namespace ava::app
