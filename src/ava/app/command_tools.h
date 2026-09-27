#pragma once

#include "ava/app/commands.h"
#include "ava/tools/tool_context.h"

#include <string>
#include <vector>

namespace ava::app {

[[nodiscard]] ava::tools::ToolContext make_tool_context(runtime::session_ts& unlocked_session, ava::permissions::PermissionResolver permission_resolver);

[[nodiscard]] ava::core::VoidResult record_tool_start(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink, CommandResult& result,
                                                      std::string const& call_id, std::string name, std::string argument_summary);

[[nodiscard]] ava::core::VoidResult record_tool_result(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink, CommandResult& result,
                                                       std::string const& call_id, std::string name, ava::agent::ToolTimelineStatus status,
                                                       std::string result_summary, std::string result_content = {},
                                                       std::vector<std::string> permission_request_ids = {});

[[nodiscard]] ava::core::Result<CommandResult> run_tool_command(runtime::session_ts& unlocked_session, CommandRequest& request);

}  // namespace ava::app
