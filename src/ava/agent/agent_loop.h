#pragma once
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/context_compaction.h"
#include "ava/agent/message_builder.h"
#include "ava/agent/mode.h"
#include "ava/agent/model_invocation_options.h"
#include "ava/agent/question.h"
#include "ava/agent/run_phase.h"
#include "ava/agent/subagent_config.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/agent/subagent_launch.h"
#include "ava/agent/tool_execution_options.h"
#include "ava/agent/tool_resources.h"
#include "ava/agent/tool_visibility.h"
#include "ava/config/model_config.h"
#include "ava/session/attachments.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/provider/provider.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/result.h"
#include "ava/core/runtime_outcome.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ava::agent {

enum class ToolTimelineStatus
{
  Running,
  Success,
  Canceled,
  Error,
};

struct ToolTimelineEntry
{
  ToolTimelineStatus status = ToolTimelineStatus::Running;
  std::string call_id = {};
  std::string name = {};
  std::string argument_summary = {};
  std::string result_summary = {};
  std::string arguments_json = {};
  std::string result_json = {};
  std::string structured_result_json = {};
  std::string content_type = {};
  std::string error_category = {};
  std::string error_code = {};
  std::string error_message = {};
  std::string error_details = {};
  std::string diff = {};
  bool diff_truncated = false;
  std::vector<std::string> changed_paths = {};
  std::vector<std::string> permission_request_ids = {};
  bool truncated = false;
  bool byte_limited = false;
  bool line_limited = false;
  std::optional<std::size_t> output_bytes = std::nullopt;
  std::optional<std::size_t> total_bytes = std::nullopt;
  std::optional<std::size_t> output_lines = std::nullopt;
  std::optional<std::size_t> total_lines = std::nullopt;
  std::optional<std::size_t> start_line = std::nullopt;
  std::optional<std::size_t> end_line = std::nullopt;
  std::optional<std::size_t> next_offset_line = std::nullopt;
  std::optional<std::size_t> omitted_bytes = std::nullopt;
  std::optional<std::size_t> omitted_lines = std::nullopt;
  std::optional<std::size_t> visible_matches = std::nullopt;
  std::optional<std::size_t> total_matches = std::nullopt;
  std::string spill_path = {};
  bool spill_truncated = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ToolProgressEntry
{
  std::string call_id = {};
  std::string name = {};
  std::string text = {};
  std::string status = "running";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string to_string(ToolTimelineStatus status);

struct AgentLoopOptions
{
  std::filesystem::path workspace_dir;
  std::filesystem::path current_dir = {};
  // Additional writable directories beyond workspace_dir (e.g., spill_dir,
  // session storage). These are opened as anchor descriptors at startup and
  // made available to tools via ToolContext::anchor_set.
  std::vector<std::filesystem::path> additional_writable_dirs = {};
  // Pre-opened anchor descriptors for all writable directories. Opened once
  // at startup and shared across all turns and subagent loops.
  std::shared_ptr<ava::core::AnchorSet> anchor_set = nullptr;
  Mode mode = Mode::Build;
  ModelInvocationOptions model = {};
  std::string access_token;
  std::string credential_type = "bearer";
  bool openai_oauth = false;
  std::string openai_account_id = "";
  std::size_t max_tool_iterations = 10;
  // Child loops opt into one bounded, tool-free wrap-up request after this
  // tool-round ceiling. Parent loops preserve their existing hard stop.
  bool child_execution = false;
  std::size_t max_provider_events = 4096;
  std::size_t max_assistant_text_bytes = 256 * 1024;
  std::size_t max_tool_argument_bytes = 256 * 1024;
  std::size_t max_tool_result_context_bytes = 8 * 1024;
  ToolResourceOptions tool_resources = {};
  ToolExecutionOptions tool_execution = {};
  std::vector<SubagentDefinition> subagents = {};
  ToolVisibilityOptions tool_visibility = {};
  std::function<void(ToolTimelineEntry const&)> on_tool_event = nullptr;
  std::function<ava::core::VoidResult(ToolProgressEntry const&)> on_tool_progress = nullptr;
  std::function<ava::core::VoidResult(ava::provider::StreamEvent const&)> on_stream_event = nullptr;
  // Private process-local task-card association. The display is normalized by
  // the runtime before loop construction; request/correlation ids are captured
  // for this run. This callback is never part of RuntimeEvent publication.
  SubagentLaunchObserver subagent_launch = {};
  ava::permissions::PermissionResolver permission_resolver = nullptr;
  // Deny-only, non-interactive policy check used before model-initiated
  // backend auto-Allow decisions. Mirrors ToolContext::auto_allow_deny_preflight
  // so sealed commands and prompt-free task launches still honor persistent
  // denies. It must never prompt or return reusable authority.
  ava::permissions::PermissionResolver auto_allow_deny_preflight = nullptr;
  QuestionResolver question_resolver = nullptr;
  std::function<bool()> cancel_requested = nullptr;
  std::function<ava::core::Result<std::vector<std::string>>()> take_steering_messages = nullptr;
  std::function<ava::core::Result<bool>(ava::session::SessionReadAuthority, std::string_view, std::vector<std::string> const& replayed_user_messages)>
      compact_context = nullptr;
  // Value-only launch snapshot inherited by children. A child binds this to
  // its own exact leased append target; parent callbacks are never retained.
  std::optional<ChildContextCompactionBlueprint> child_compaction_blueprint = std::nullopt;
  std::optional<ChildContextCompactionBinding> child_compaction_binding = std::nullopt;
  std::function<ava::core::Result<std::unique_ptr<ava::provider::Provider>>()> background_provider_factory = nullptr;
  // Retained by child loops so web tools continue to use the originating
  // runtime session authority even after the scheduling run has ended.
  ava::http::TransportFactory transport_factory = nullptr;
  ava::http::TransportFactory background_transport_factory = nullptr;
  // Production and tests use one application-scoped coordinator as the sole
  // task-subagent owner. BackgroundJobRegistry remains an internal engine.
  std::shared_ptr<SubagentCoordinator> subagent_coordinator = nullptr;
  // Immutable generation routes for records produced by this run. Persistent
  // provider assistant turns require the batch route so v4 staging and its
  // commit are appended through one guarded authority.
  SessionAppendSink append_entry = nullptr;
  SessionAppendBatchSink append_batch = nullptr;
  // Copyable exact-lease (or in-memory) authority used for every history read.
  std::optional<ava::session::SessionReadAuthority> session_read_authority = std::nullopt;
  // Must match the policy established when the runtime session was opened.
  // Direct unit construction retains historical unbounded behavior.
  ava::session::SessionReadLimits session_read_limits = ava::session::legacy_unbounded_session_read_limits();
  // Backend-only provenance for an application-generated synthetic delivery
  // user entry. Ordinary frontend text never derives or populates this field.
  std::optional<ava::session::SyntheticDeliveryProvenance> synthetic_user_message_provenance = std::nullopt;
  // Called at real loop boundaries; errors abort the loop rather than being
  // swallowed as observer-only state.
  std::function<ava::core::VoidResult(RunPhase)> on_phase = nullptr;
  bool parallel_read_search_tools = false;
  std::size_t parallel_read_search_max_workers = 4;
  // Disabled by default. This is independent from typed runtime-event/RPC output.
  std::shared_ptr<ava::observability::RunObservation> observation = nullptr;
  // Runtime may pre-establish this so retries, compaction, and the agent share
  // one run/turn identity.
  ava::observability::TraceContext trace_context = {};

  // Includes provider credentials and callback/runtime ownership state; never
  // stream this aggregate through generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct AgentLoopResult
{
  std::string final_text;
  // Exact id of the final durably committed v4 assistant transaction.
  std::optional<std::string> committed_turn_id = std::nullopt;
  std::optional<ava::provider::TokenUsage> usage = std::nullopt;
  std::optional<long double> cost_usd = std::nullopt;
  std::size_t provider_iterations = 0;
  std::size_t tool_calls = 0;
  std::size_t initial_context_messages = 0;
  bool used_compacted_context = false;
  std::size_t tool_iterations = 0;
  ava::core::RuntimeTerminalOutcome outcome = ava::core::RuntimeTerminalOutcome::Error;
  std::vector<ToolTimelineEntry> tool_timeline;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class AgentLoop
{
 public:
  explicit AgentLoop(AgentLoopOptions options);

  [[nodiscard]] ava::core::Result<AgentLoopResult> run_turn(std::string const& user_message, ava::session::SessionStore& store,
                                                            ava::provider::Provider const& provider, ava::http::Transport& transport);
  [[nodiscard]] ava::core::Result<AgentLoopResult> run_turn(std::string const& user_message,
                                                            std::vector<ava::session::ImageAttachmentRef> const& image_attachments,
                                                            ava::session::SessionStore& store, ava::provider::Provider const& provider,
                                                            ava::http::Transport& transport);
  // Owns AgentLoopOptions, which contains provider credentials.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] ava::core::Result<AgentLoopResult> run_turn_impl(std::string const& user_message,
                                                                 std::vector<ava::session::ImageAttachmentRef> const& image_attachments,
                                                                 ava::session::SessionStore& store, ava::provider::Provider const& provider,
                                                                 ava::http::Transport& transport, ava::observability::TraceContext const& trace_context);

  AgentLoopOptions options_;
  bool ava_authority_roots_over_limit_ = false;
};

}  // namespace ava::agent
