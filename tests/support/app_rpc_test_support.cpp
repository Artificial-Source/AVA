#include "sys.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "ava/core/json.h"

#include <optional>
#include <string>
#include <string_view>

namespace ava::tests {

std::optional<std::string_view> rpc_response_line(std::string_view jsonl, std::string_view id)
{
  while (!jsonl.empty())
  {
    auto const end = jsonl.find('\n');
    if (end == std::string_view::npos)
      return std::nullopt;
    auto const line = jsonl.substr(0, end);
    if (ava::core::json::is_valid_object(line) && ava::core::json::string_field(line, "type") == "response" && ava::core::json::string_field(line, "id") == id)
      return line;
    jsonl.remove_prefix(end + 1);
  }
  return std::nullopt;
}

bool wait_for_rpc_response(ThreadSafeStringBuf& output, std::string_view id, std::chrono::milliseconds timeout)
{
  return output.wait_for_record(
      [id](std::string_view line) {
        return ava::core::json::is_valid_object(line) && ava::core::json::string_field(line, "type") == "response" &&
               ava::core::json::string_field(line, "id") == id;
      },
      timeout);
}

std::optional<std::string> rpc_string_field_from_output(std::string_view jsonl, std::string_view field)
{
  auto value = extract_json_string_field(jsonl, field);
  if (!value.empty() || jsonl.find("\"" + std::string(field) + "\":\"\"") != std::string_view::npos)
    return value;
  return std::nullopt;
}

}  // namespace ava::tests
