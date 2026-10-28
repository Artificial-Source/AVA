#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace ava::agent {

inline constexpr std::string_view kTodoWriteToolName = "todowrite";
inline constexpr std::size_t kTodoSchemaVersion = 1;
inline constexpr std::size_t kMaxTodoItems = 50;
inline constexpr std::size_t kMinTodoIdBytes = 1;
inline constexpr std::size_t kMaxTodoIdBytes = 32;
inline constexpr std::size_t kMaxTodoContentBytes = 512;

enum class TodoStatus
{
  Pending,
  InProgress,
  Completed,
};

[[nodiscard]] std::string_view to_string(TodoStatus status) noexcept;
[[nodiscard]] std::optional<TodoStatus> parse_todo_status(std::string_view value) noexcept;

struct TodoItem
{
  std::string id;
  std::string content;
  TodoStatus status = TodoStatus::Pending;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TodoCounts
{
  std::size_t total = 0;
  std::size_t pending = 0;
  std::size_t in_progress = 0;
  std::size_t completed = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TodoSnapshot
{
  std::size_t schema_version = kTodoSchemaVersion;
  std::vector<TodoItem> todos = {};
  TodoCounts counts = {};

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] TodoCounts count_todos(std::vector<TodoItem> const& todos) noexcept;

// Strict full-list replacement parse of tool arguments: {"todos":[...]}.
// Rejects unknown fields, duplicate members, invalid UTF-8/control text, bounds
// violations, duplicate ids, and more than one in_progress item.
[[nodiscard]] ava::core::Result<TodoSnapshot> parse_todowrite_arguments(std::string_view arguments_json);

// Strict parse of a previously normalized successful todowrite result_text.
// Fail-closed for presentation hydration: malformed input returns nullopt-style
// error rather than a partial trust of the payload.
[[nodiscard]] ava::core::Result<TodoSnapshot> parse_todowrite_result_text(std::string_view result_text);

[[nodiscard]] std::string serialize_todowrite_success_json(TodoSnapshot const& snapshot);
[[nodiscard]] std::string serialize_todowrite_error_json(ava::core::Error const& error);

// Latest successful committed todowrite ToolResult wins. Failed/malformed
// results are ignored. An empty successful list clears. Uses only already-loaded
// SessionEntry values; does not touch session authority or read limits.
[[nodiscard]] std::optional<TodoSnapshot> latest_committed_todowrite_snapshot(std::vector<ava::session::SessionEntry> const& entries);

}  // namespace ava::agent
