#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/runtime.h"
#include "ava/mcp/stdio_client.h"
#include "ava/permissions/permission.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

enum class UnifiedCommandSource
{
  Builtin,
  PromptProject,
  PromptGlobal,
  Skill,
  McpPrompt,
  PluginCommand,
};

enum class UnifiedCommandKind
{
  Backend,
  PromptTemplate,
  SkillPrompt,
  McpPrompt,
  PluginCommand,
};

struct CommandRegistryEntry
{
  std::string command = {};
  std::vector<std::string> aliases = {};
  std::string description = {};
  std::string hint = {};
  std::string category = {};
  bool enabled = true;
  std::string disabled_reason = {};
  UnifiedCommandSource source = UnifiedCommandSource::Builtin;
  UnifiedCommandKind kind = UnifiedCommandKind::Backend;
  std::string source_id = {};
  std::filesystem::path source_path = {};
  std::string source_scope = {};
  std::string template_text = {};
  std::string skill_name = {};
  std::string mcp_server_id = {};
  std::string mcp_prompt_name = {};
  std::vector<ava::mcp::McpPromptArgumentDescription> mcp_arguments = {};
  std::string plugin_id = {};
  std::string plugin_command_name = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandRegistryDiagnostic
{
  std::string command = {};
  std::string source = {};
  std::string source_id = {};
  std::filesystem::path path = {};
  std::string message = {};
  ava::core::ErrorCode error_code = ava::core::ErrorCode::Unspecified;
  std::string winner_source = {};
  std::string winner_source_id = {};
  std::filesystem::path winner_path = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandRegistry
{
  std::vector<CommandRegistryEntry> entries;
  std::vector<CommandRegistryDiagnostic> diagnostics;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PromptCommandSourceFile
{
  std::string command_name = {};
  std::string scope = {};
  std::filesystem::path path = {};
  std::size_t byte_count = 0;
  std::uint64_t content_fingerprint = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandRegistryOptions
{
  bool include_builtins = true;
  bool include_prompt_commands = true;
  bool include_skills = true;
  bool include_plugin_commands = true;
  bool include_mcp_prompts = false;
  ava::permissions::PermissionResolver permission_resolver = nullptr;
  std::function<bool()> cancel_requested = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

namespace runtime {
class Session;
} // namespace runtime

[[nodiscard]] std::string to_string(UnifiedCommandSource source);
[[nodiscard]] std::string to_string(UnifiedCommandKind kind);
[[nodiscard]] CommandRegistryEntry const* find_command_registry_entry(CommandRegistry const& registry, std::string_view line) noexcept;

// Passive command recognition. Never launches MCP servers or other external programs.
// Namespaced /skill:, /mcp:, and /plugin: tokens are treated as commands without discovery.
[[nodiscard]] bool command_registry_contains(runtime::session_ts& unlocked_session, std::string_view line);

// Resolve a slash command for submission.
//
// Loads builtins, prompt files, skills, and plugins without starting MCP. If those sources miss and the
// token is not /skill: or /plugin:, MCP prompt discovery runs once with permission_resolver and
// cancel_requested. Found entries are returned by value. Genuine misses return an empty optional.
// Cancellation, permission denial, and listing failures are errors rather than not-found.
[[nodiscard]] ava::core::Result<std::optional<CommandRegistryEntry>> resolve_command_registry_entry(
    runtime::session_ts& unlocked_session, std::string_view line, ava::permissions::PermissionResolver permission_resolver = nullptr,
    std::function<bool()> cancel_requested = nullptr);

[[nodiscard]] ava::core::Result<std::string> expand_prompt_command_template(std::string_view template_text, std::string_view argument_text);

// Build a command registry from `unlocked_session` according to the source categories and callbacks in `options`.
//
// Returns discovered entries plus non-fatal diagnostics. Loading can inspect files and plugins, and MCP prompt
// discovery can request permission, append an audit entry, launch configured servers, and block on protocol I/O.
[[nodiscard]] CommandRegistry load_command_registry(runtime::session_ts& unlocked_session, CommandRegistryOptions options = {});

}  // namespace ava::app
