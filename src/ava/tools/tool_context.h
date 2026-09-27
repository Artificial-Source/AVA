#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/process/scope.h"
#include "ava/tools/tool_io.h"
#include "ava/permissions/permission.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/mode.h"
#include "ava/core/result.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ava::lsp {
class DiagnosticsProvider;
}  // namespace ava::lsp

namespace ava::mcp {
struct McpConfig;
}  // namespace ava::mcp

namespace ava::tools {

class MutationQueue;
class SecureWorkspace;

struct PermissionAuditEvent
{
  std::string permission_request_id = {};
  ava::permissions::Operation operation;
  ava::core::Mode mode = ava::core::Mode::Build;
  std::string tool_name;
  ava::permissions::PermissionAction action = ava::permissions::PermissionAction::Deny;
  std::string reason;
  ava::permissions::PermissionRisk risk = ava::permissions::PermissionRisk::Low;
  std::filesystem::path target_path = {};
  std::string command;
  std::string resolution;
  std::string resolution_source;
  std::string resolution_reason;
  std::string actor = "agent";
  std::string rule_id;
  // Durable audit serialization must suppress command and recipe-display
  // fields derived from arguments when a strict frontend supplied redacted args.
  bool command_arguments_redacted = false;
  std::optional<ava::permissions::CommandPermissionMetadata> command_metadata = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using PermissionAuditSink = std::function<ava::core::VoidResult(PermissionAuditEvent const&)>;

struct ToolProgressEvent
{
  std::string text;
  std::string call_id;
  std::string tool_name;
  std::string status = "running";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using ToolProgressSink = std::function<ava::core::VoidResult(ToolProgressEvent const&)>;

// Per-dispatch capture for validated one-shot denial guidance. Owned through
// ToolContext so parallel dispatch_with_context calls remain independent and
// race-free. Direct app command contexts leave this null and discard guidance.
struct PermissionDenialGuidanceCapture
{
  std::string provider_user_guidance = {};

  // Guidance text must never appear in debug/log representations.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ToolContext
{
  std::filesystem::path workspace_dir;
  std::filesystem::path spill_dir = {};
  ava::core::Mode mode = ava::core::Mode::Build;
  // Local sealed command execution is enabled by default. Legacy and
  // PromptOnly contexts remain explicit non-executing compatibility modes.
  ava::command::CommandRuntimeOptions command_runtime{.mode = ava::command::CommandRuntimeMode::Enabled};
  ava::permissions::PermissionResolver permission_resolver = nullptr;
  // Deny-only, non-interactive policy check used before backend auto-Allow
  // decisions. Explicit persistent denies still win over prompt-free task
  // launches; this check must never prompt or return reusable authority.
  ava::permissions::PermissionResolver auto_allow_deny_preflight = nullptr;
  PermissionAuditSink permission_audit_sink = nullptr;
  ToolProgressSink progress_sink = nullptr;
  // Strict adapters may expose a distinct pending -> in_progress boundary.
  // The shared flag keeps multi-permission tools to one execution-start event.
  bool announce_execution_after_permission = false;
  std::shared_ptr<std::atomic_bool> execution_started = nullptr;
  std::function<bool()> cancel_requested = nullptr;
  std::string permission_tool_name = {};
  std::string permission_actor = {};
  std::string current_tool_name = {};
  std::string current_call_id = {};
  // Strict adapters can keep exact arguments in memory for matching while
  // preventing them from entering durable permission audit records.
  bool redact_permission_audit_arguments = false;
  bool require_explicit_file_permissions = false;
  // Strict adapters share one descriptor-anchored root across permission
  // identity resolution and the actual built-in file operation.
  std::shared_ptr<SecureWorkspace> secure_workspace = nullptr;
  // Pre-opened anchor descriptors for all writable directories (workspace,
  // spill, session storage, synthetic command roots, and any user-configured
  // additional dirs). Path authority is selected lexically and resolved
  // descriptor-relative; configured anchor roots may themselves contain
  // symlink components.
  std::shared_ptr<ava::core::AnchorSet> anchor_set = nullptr;
  // Actual AVA config/state/sessions/auth authority directories supplied from
  // the runtime session. These are passed to command sealing so workspace
  // overlap with authority roots is rejected, and to containment so authority
  // roots are never made writable through a broader workspace rule. Direct
  // test contexts may leave this empty.
  std::vector<std::filesystem::path> ava_authority_roots = {};
  // ToolContext is copied into dispatchers/workers, so immutable adapters share
  // session ownership rather than storing lifetime-sensitive references.
  std::shared_ptr<ExactFileAccess const> exact_file_access = nullptr;
  std::shared_ptr<CommandExecutor const> command_executor = nullptr;
  // Explicit parent authority for one-shot tool-owned process operations.
  // Plugin execution derives a fresh operation scope before reservation.
  std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt;
  // Session-capturing process authority for web tools. Explicit per-call fake
  // transports bypass this factory and require no process authority.
  ava::http::TransportFactory transport_factory = nullptr;
  // An observer-only correlation ID. Provider call IDs remain product/session
  // data and must not cross the trace boundary.
  std::string trace_call_id = {};
  std::shared_ptr<std::vector<std::string>> permission_request_ids = nullptr;
  // Independent per-dispatch capture installed by ToolDispatcher. shared_ptr
  // debug printing only emits the pointer identity, never the guidance text.
  std::shared_ptr<PermissionDenialGuidanceCapture> permission_denial_guidance_capture = nullptr;
  std::shared_ptr<MutationQueue> mutation_queue = nullptr;
  std::shared_ptr<ava::lsp::DiagnosticsProvider> lsp_diagnostics_provider = nullptr;
  std::filesystem::path plugin_global_plugins_dir = {};
  std::filesystem::path plugin_project_plugins_dir = {};
  std::filesystem::path plugin_enablement_file = {};
  bool include_project_plugins = true;
  bool include_plugin_tools = true;
  std::filesystem::path mcp_global_config_file = {};
  std::filesystem::path mcp_project_config_file = {};
  bool include_global_mcp_config = true;
  bool include_project_mcp_config = true;
  std::shared_ptr<ava::mcp::McpConfig const> session_mcp_config = nullptr;
  // Present means compose exactly these built-ins with immutable session MCP
  // and fail closed on unavailable names, discovery failures, or collisions.
  std::optional<std::vector<std::string>> exact_builtin_tool_names = std::nullopt;
  bool require_descriptor_secure_workspace = false;
  std::vector<std::filesystem::path> skill_global_dirs = {};
  std::vector<std::filesystem::path> skill_project_dirs = {};
  bool include_global_skills = true;
  bool include_project_skills = true;
  std::string session_id = {};
  std::string provider_id = {};
  std::string model_id = {};
  std::filesystem::path current_dir = {};
  std::shared_ptr<ava::observability::RunObservation> observation = nullptr;
  ava::observability::TraceContext trace_context = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::tools
