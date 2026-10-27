#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/observability/run_observer.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/question.h"
#include "ava/agent/run_phase.h"
#include "ava/tools/tool_io.h"
#include "ava/session/attachments.h"
#include "ava/permissions/permission.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ava::app::runtime {

// Carry per-invocation run controls for run_prompt and compaction summary generation: credentials, streaming/retry toggles, callbacks for events, permissions,
// questions, cancellation and steering, plus an optional session lock and image attachments.
//
// All callback members default to null; run_prompt treats a null permission or question resolver as an error and a null event sink as a no-op.
struct RunOptions
{
  // Caller-supplied request identity. run_prompt generates one when absent.
  std::optional<std::string> request_id = std::nullopt;
  ava::core::NoLogString access_token;
  std::string credential_type = "bearer";
  bool openai_oauth = false;
  ava::core::NoLogString openai_account_id;
  bool stream = true;
  bool enable_transport_retries = false;
  std::optional<std::vector<std::string>> exact_builtin_tool_names = std::nullopt;
  // Suppress ambient plugins, skills, LSP, plugin hooks, and subagent catalogs;
  // preserve explicit session MCP unless disable_session_mcp is set.
  bool isolate_ambient_extensions = false;
  // Integration-only runs may explicitly suppress even a session-local
  // immutable MCP composition; ordinary ACP isolation retains its approved MCP.
  bool disable_session_mcp = false;
  bool require_descriptor_secure_workspace = false;
  bool announce_execution_after_permission = false;
  bool redact_permission_audit_arguments = false;
  bool require_explicit_file_permissions = false;
  std::shared_ptr<ava::tools::ExactFileAccess const> exact_file_access = nullptr;
  std::shared_ptr<ava::tools::CommandExecutor const> command_executor = nullptr;
  ava::event::RuntimeEventSink event_sink = nullptr;
  // Private process-local task launch metadata; never enters RuntimeEvent/RPC.
  ava::agent::SubagentLaunchSink on_subagent_launch = nullptr;
  ava::permissions::PermissionResolver permission_resolver = nullptr;
  ava::agent::QuestionResolver question_resolver = nullptr;
  std::function<bool()> cancel_requested = nullptr;
  std::function<ava::core::Result<std::vector<std::string>>()> take_steering_messages = nullptr;
  ava::agent::SessionAppendSink active_append_route = nullptr;
  ava::agent::SessionAppendBatchSink active_append_batch_route = nullptr;
  ava::session::SessionCompactionAppendSink active_compaction_append_route = nullptr;
  std::vector<ava::session::ImageAttachmentRef> image_attachments;
  // Disabled by default; observation data never enters events, sessions, or RPC output.
  std::shared_ptr<ava::observability::RunObservation> observation = nullptr;
  ava::observability::TraceContext trace_context = {};
  // Strict adapters use this callback to arbitrate immediately before committing terminal output.
  std::function<ava::core::VoidResult()> on_terminal_commit = nullptr;
  std::function<ava::core::VoidResult(ava::agent::RunPhase)> on_phase = nullptr;
  bool offline = false;
  bool expand_prompt_file_references = true;
  // Public headless opt-in for the existing builtin read/search scheduler.
  // Disabled by default. max_workers must stay in 1..4; invalid values are rejected.
  bool parallel_read_search_tools = false;
  std::size_t parallel_read_search_max_workers = 4;
  // Internal application marker: automatic parent delivery must not refresh
  // or recursively retain its own detached capsule.
  bool synthetic_subagent_delivery = false;
  std::optional<ava::session::SyntheticDeliveryProvenance> synthetic_user_message_provenance = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::app::runtime
