#include "sys.h"
#include "ava/agent/todo.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <nlohmann/json.hpp>

namespace ava::agent {
namespace {

using Json = nlohmann::json;
using namespace ava::agent::tool_dispatch;

constexpr std::string_view kToolName = kTodoWriteToolName;

bool is_ascii_semantic_id(std::string_view value) noexcept
{
  if (value.empty() || value.size() > kMaxTodoIdBytes)
    return false;
  for (unsigned char const ch : value)
  {
    // Locale-independent [A-Za-z0-9_-] — never use std::isalnum here.
    bool const is_ascii_alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    if (!(is_ascii_alnum || ch == '_' || ch == '-'))
      return false;
  }
  return true;
}

ava::core::Error argument_error(std::string message, std::string_view field = {})
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("tool", std::string(kToolName));
  if (!field.empty())
    error.with_context("argument", std::string(field));
  return error;
}

ava::core::Error result_error(std::string message)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("tool", std::string(kToolName));
  return error;
}

ava::core::Result<TodoItem> parse_todo_item_object(Json const& item, std::size_t index)
{
  if (!item.is_object())
    return std::unexpected(argument_error("todowrite todos[" + std::to_string(index) + "] must be an object", "todos"));

  for (auto const& [key, _] : item.items())
  {
    if (key != "id" && key != "content" && key != "status")
      return std::unexpected(argument_error("todowrite todos item contains an unknown field", key));
  }

  if (!item.contains("id") || !item["id"].is_string())
    return std::unexpected(argument_error("todowrite todo id is required", "id"));
  if (!item.contains("content") || !item["content"].is_string())
    return std::unexpected(argument_error("todowrite todo content is required", "content"));
  if (!item.contains("status") || !item["status"].is_string())
    return std::unexpected(argument_error("todowrite todo status is required", "status"));

  TodoItem parsed;
  try
  {
    parsed.id = item["id"].get<std::string>();
    parsed.content = item["content"].get<std::string>();
  }
  catch (...)
  {
    return std::unexpected(argument_error("todowrite todo fields must be strings", "todos"));
  }

  if (!is_ascii_semantic_id(parsed.id))
  {
    return std::unexpected(argument_error("todowrite todo id must be 1..32 ASCII bytes matching [A-Za-z0-9_-]", "id"));
  }
  if (parsed.content.empty())
    return std::unexpected(argument_error("todowrite todo content must be nonempty", "content"));
  if (parsed.content.size() > kMaxTodoContentBytes)
  {
    auto error = argument_error("todowrite todo content exceeds the 512-byte limit", "content");
    error.with_context("max_bytes", std::to_string(kMaxTodoContentBytes));
    return std::unexpected(std::move(error));
  }
  if (!ava::core::json::is_valid_utf8(parsed.content))
    return std::unexpected(argument_error("todowrite todo content must be valid UTF-8", "content"));
  if (auto safe = reject_control_arg(parsed.content, "content", kToolName); !safe)
    return std::unexpected(std::move(safe.error()));

  auto const status = parse_todo_status(item["status"].get_ref<std::string const&>());
  if (!status)
    return std::unexpected(argument_error("todowrite todo status must be pending, in_progress, or completed", "status"));
  parsed.status = *status;
  return parsed;
}

ava::core::Result<TodoSnapshot> parse_todos_array(Json const& todos_json, bool arguments_mode)
{
  if (!todos_json.is_array())
  {
    return std::unexpected(arguments_mode ? argument_error("todowrite todos must be an array", "todos")
                                          : result_error("todowrite result todos must be an array"));
  }
  if (todos_json.size() > kMaxTodoItems)
  {
    auto error = arguments_mode ? argument_error("todowrite accepts at most 50 todos", "todos") : result_error("todowrite result exceeds the 50-item limit");
    error.with_context("max_items", std::to_string(kMaxTodoItems));
    return std::unexpected(std::move(error));
  }

  TodoSnapshot snapshot;
  snapshot.todos.reserve(todos_json.size());
  std::unordered_set<std::string> seen_ids;
  seen_ids.reserve(todos_json.size() * 2 + 1);
  std::size_t in_progress = 0;

  for (std::size_t index = 0; index < todos_json.size(); ++index)
  {
    auto item = parse_todo_item_object(todos_json[index], index);
    if (!item)
      return std::unexpected(std::move(item.error()));
    if (!seen_ids.insert(item->id).second)
      return std::unexpected(argument_error("todowrite todo ids must be unique", "id"));
    if (item->status == TodoStatus::InProgress)
      ++in_progress;
    snapshot.todos.push_back(std::move(*item));
  }
  if (in_progress > 1)
    return std::unexpected(argument_error("todowrite allows at most one in_progress todo", "status"));

  snapshot.counts = count_todos(snapshot.todos);
  return snapshot;
}

bool json_bool_true(Json const& value)
{
  return value.is_boolean() && value.get<bool>();
}

std::optional<std::string> session_tool_result_text(ava::session::SessionEntry const& entry)
{
  if (entry.type != ava::session::EntryType::ToolResult)
    return std::nullopt;
  auto const name = ava::core::json::string_field(entry.data_json, "name");
  if (!name || *name != kTodoWriteToolName)
    return std::nullopt;
  auto const success_start = ava::core::json::field_value_start(entry.data_json, "success");
  if (!success_start)
    return std::nullopt;
  auto const rest = std::string_view(entry.data_json).substr(*success_start);
  if (!rest.starts_with("true"))
    return std::nullopt;
  return ava::core::json::string_field(entry.data_json, "result");
}

}  // namespace

std::string_view to_string(TodoStatus status) noexcept
{
  switch (status)
  {
    case TodoStatus::Pending:
      return "pending";
    case TodoStatus::InProgress:
      return "in_progress";
    case TodoStatus::Completed:
      return "completed";
  }
  return "pending";
}

std::optional<TodoStatus> parse_todo_status(std::string_view value) noexcept
{
  if (value == "pending")
    return TodoStatus::Pending;
  if (value == "in_progress")
    return TodoStatus::InProgress;
  if (value == "completed")
    return TodoStatus::Completed;
  return std::nullopt;
}

TodoCounts count_todos(std::vector<TodoItem> const& todos) noexcept
{
  TodoCounts counts;
  counts.total = todos.size();
  for (auto const& item : todos)
  {
    switch (item.status)
    {
      case TodoStatus::Pending:
        ++counts.pending;
        break;
      case TodoStatus::InProgress:
        ++counts.in_progress;
        break;
      case TodoStatus::Completed:
        ++counts.completed;
        break;
    }
  }
  return counts;
}

ava::core::Result<TodoSnapshot> parse_todowrite_arguments(std::string_view arguments_json)
{
  auto const strict = ava::core::validate_strict_json(arguments_json, ava::core::json::kMaxNestingDepth);
  if (strict == ava::core::StrictJsonStatus::DuplicateObjectKey)
    return std::unexpected(argument_error("todowrite arguments contain duplicate member names"));
  if (strict != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(argument_error("todowrite arguments must be one valid JSON object"));

  auto root = Json::parse(arguments_json.begin(), arguments_json.end(), nullptr, false, true);
  if (root.is_discarded() || !root.is_object())
    return std::unexpected(argument_error("todowrite arguments must be one valid JSON object"));

  for (auto const& [key, _] : root.items())
  {
    if (key != "todos")
      return std::unexpected(argument_error("todowrite arguments contain an unknown field", key));
  }
  if (!root.contains("todos"))
    return std::unexpected(argument_error("todowrite todos is required", "todos"));
  return parse_todos_array(root["todos"], true);
}

ava::core::Result<TodoSnapshot> parse_todowrite_result_text(std::string_view result_text)
{
  auto const strict = ava::core::validate_strict_json(result_text, ava::core::json::kMaxNestingDepth);
  if (strict != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(result_error("todowrite result must be one valid JSON object"));

  auto root = Json::parse(result_text.begin(), result_text.end(), nullptr, false, true);
  if (root.is_discarded() || !root.is_object())
    return std::unexpected(result_error("todowrite result must be one valid JSON object"));

  for (auto const& [key, _] : root.items())
  {
    if (key != "schema_version" && key != "tool" && key != "ok" && key != "todos" && key != "counts")
      return std::unexpected(result_error("todowrite result contains an unknown field"));
  }
  if (!root.contains("ok") || !json_bool_true(root["ok"]))
    return std::unexpected(result_error("todowrite result is not a successful snapshot"));
  if (!root.contains("tool") || !root["tool"].is_string() || root["tool"].get_ref<std::string const&>() != kTodoWriteToolName)
    return std::unexpected(result_error("todowrite result tool marker is invalid"));
  if (!root.contains("schema_version") || !root["schema_version"].is_number_integer() ||
      root["schema_version"].get<long long>() != static_cast<long long>(kTodoSchemaVersion))
    return std::unexpected(result_error("todowrite result schema_version is unsupported"));
  if (!root.contains("todos"))
    return std::unexpected(result_error("todowrite result todos is required"));

  auto snapshot = parse_todos_array(root["todos"], false);
  if (!snapshot)
    return snapshot;

  if (root.contains("counts"))
  {
    if (!root["counts"].is_object())
      return std::unexpected(result_error("todowrite result counts must be an object"));
    auto const& counts = root["counts"];
    for (auto const& [key, _] : counts.items())
    {
      if (key != "total" && key != "pending" && key != "in_progress" && key != "completed")
        return std::unexpected(result_error("todowrite result counts contain an unknown field"));
    }
    auto require_count = [&](char const* key, std::size_t expected) -> ava::core::VoidResult {
      if (!counts.contains(key) || !counts[key].is_number_integer() || counts[key].get<long long>() < 0)
        return std::unexpected(result_error("todowrite result counts are invalid"));
      if (static_cast<std::size_t>(counts[key].get<long long>()) != expected)
        return std::unexpected(result_error("todowrite result counts do not match todos"));
      return {};
    };
    if (auto ok = require_count("total", snapshot->counts.total); !ok)
      return std::unexpected(std::move(ok.error()));
    if (auto ok = require_count("pending", snapshot->counts.pending); !ok)
      return std::unexpected(std::move(ok.error()));
    if (auto ok = require_count("in_progress", snapshot->counts.in_progress); !ok)
      return std::unexpected(std::move(ok.error()));
    if (auto ok = require_count("completed", snapshot->counts.completed); !ok)
      return std::unexpected(std::move(ok.error()));
  }
  return snapshot;
}

std::string serialize_todowrite_success_json(TodoSnapshot const& snapshot)
{
  std::string out = "{\"schema_version\":";
  out += std::to_string(kTodoSchemaVersion);
  out += ",\"tool\":\"todowrite\",\"ok\":true,\"todos\":[";
  for (std::size_t index = 0; index < snapshot.todos.size(); ++index)
  {
    if (index != 0)
      out += ',';
    auto const& item = snapshot.todos[index];
    out += "{\"id\":\"";
    out += ava::core::json::escape(item.id);
    out += "\",\"content\":\"";
    out += ava::core::json::escape(item.content);
    out += "\",\"status\":\"";
    out += to_string(item.status);
    out += "\"}";
  }
  auto const counts = count_todos(snapshot.todos);
  out += "],\"counts\":{\"total\":";
  out += std::to_string(counts.total);
  out += ",\"pending\":";
  out += std::to_string(counts.pending);
  out += ",\"in_progress\":";
  out += std::to_string(counts.in_progress);
  out += ",\"completed\":";
  out += std::to_string(counts.completed);
  out += "}}";
  return out;
}

std::string serialize_todowrite_error_json(ava::core::Error const& error)
{
  std::string code = "invalid_argument";
  for (auto const& item : error.context())
  {
    if (item.key == "todo_error_code" && !item.value.empty())
    {
      code = item.value;
      break;
    }
  }
  std::string out = "{\"schema_version\":";
  out += std::to_string(kTodoSchemaVersion);
  out += ",\"tool\":\"todowrite\",\"ok\":false,\"error\":{\"category\":\"";
  out += ava::core::json::escape(ava::core::to_string(error.category()));
  out += "\",\"code\":\"";
  out += ava::core::json::escape(code);
  out += "\",\"message\":\"";
  out += ava::core::json::escape(error.message());
  out += "\"}}";
  return out;
}

TodoSnapshot latest_committed_todowrite_snapshot(std::vector<ava::session::SessionEntry> const& entries)
{
  TodoSnapshot latest;
  for (auto const& entry : entries)
  {
    auto result_text = session_tool_result_text(entry);
    if (!result_text)
      continue;
    auto parsed = parse_todowrite_result_text(*result_text);
    if (!parsed)
      continue;
    latest = std::move(*parsed);
  }
  return latest;
}

}  // namespace ava::agent
