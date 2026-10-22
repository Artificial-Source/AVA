#pragma once

#include "ava/event/RuntimeEvent.h"
#include "ava/app/command_catalog.h"
#include "ava/app/runtime.h"
#include "ava/agent/agent_loop.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

class PluginUiInvocationCapability;

struct CommandRequest
{
  std::string command;
  ava::event::RuntimeEventSink event_sink = nullptr;
  ava::permissions::PermissionResolver permission_resolver = nullptr;
  ava::agent::QuestionResolver question_resolver = nullptr;
  CompactionSummaryGenerator compaction_summary_generator = nullptr;
  std::function<bool()> cancel_requested = nullptr;
  std::shared_ptr<PluginUiInvocationCapability> plugin_ui_capability = nullptr;
  bool propagate_compaction_errors = false;
  std::vector<CommandHotkey> hotkeys = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandResult
{
  bool handled = false;
  bool quit = false;
  bool session_tree_changed = false;
  std::vector<std::string> output;
  std::vector<ava::agent::ToolTimelineEntry> tool_timeline;
  std::optional<std::string> prompt_message = std::nullopt;
  std::string prompt_command = {};
  std::string prompt_source = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandRegistryEntry;

[[nodiscard]] bool is_backend_command(std::string_view line) noexcept;
// Session-aware recognition for catalog, aliases, skills, plugins, and prompt files.
// Does not launch MCP servers; MCP prompt aliases are resolved only at submission time.
[[nodiscard]] bool is_backend_command_1(std::string_view line, runtime::session_ts& unlocked_session);
[[nodiscard]] std::string command_help_text(std::vector<CommandHotkey> const& hotkeys = {});
[[nodiscard]] std::string command_hotkeys_text(std::vector<CommandHotkey> const& hotkeys = {});
[[nodiscard]] ava::core::Result<CommandResult> run_command(runtime::session_ts& unlocked_session, CommandRequest request);
[[nodiscard]] ava::core::Result<CommandResult> run_registry_command(runtime::session_ts& unlocked_session, CommandRequest request,
                                                                    CommandRegistryEntry const& entry);

}  // namespace ava::app
