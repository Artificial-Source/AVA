#pragma once

#include "ava/event/RuntimeEvent.h"
#include "ava/app/command_catalog.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/agent/agent_loop.h"
#include "ava/session/attachments.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/result.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::app {
class PluginUiInvocationCapability;
}

namespace ava::app::interactive_internal {

struct InteractiveState
{
 public:
  // Lifetime contract: the borrowed session must outlive each run loop invocation.
  runtime::session_ts& unlocked_session;

  // Runtime sessions can contain provider credentials and must not be debug-printed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct InteractiveResult
{
 public:
  bool quit = false;
  bool session_tree_changed = false;
  bool ordinary_turn_committed = false;
  std::vector<std::string> output;
  std::vector<ava::agent::ToolTimelineEntry> tool_timeline;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<std::optional<std::string>> edit_text_with_external_editor(std::string_view initial_text);
[[nodiscard]] bool is_display_settings_command(std::string_view line) noexcept;
void add_output(InteractiveResult& result, std::string text);
// Persist a reusable permission rule and return its backend rule id. Presentation
// layers may wrap the id but never receive rule-store authority.
[[nodiscard]] ava::core::Result<std::string> remember_permission_rule(runtime::session_ts const& unlocked_session,
                                                                      ava::permissions::PermissionPrompt const& prompt,
                                                                      ava::permissions::PermissionAction action, std::string actor);
[[nodiscard]] InteractiveResult handle_interactive_submission(
    InteractiveState& state, std::string const& line, ava::permissions::PermissionResolver permission_resolver = nullptr,
    ava::agent::QuestionResolver question_resolver = nullptr, std::vector<CommandHotkey> const& hotkeys = {}, ava::event::RuntimeEventSink event_sink = nullptr,
    std::function<bool()> cancel_requested = nullptr, std::function<ava::core::Result<std::vector<std::string>>()> take_steering_messages = nullptr,
    std::vector<ava::session::ImageAttachmentRef> image_attachments = {}, std::string request_id = {},
    ava::agent::SubagentLaunchSink on_subagent_launch = nullptr, std::shared_ptr<PluginUiInvocationCapability> plugin_ui_capability = nullptr);

}  // namespace ava::app::interactive_internal
