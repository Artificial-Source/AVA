#include "sys.h"
#include "ava/agent/agent_turn_executor_internal.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_result.h"
#include "ava/agent/tool_scheduler.h"
#include "ava/agent/tool_summaries.h"
#include "ava/agent/tool_timeline.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace ava::agent::detail {
namespace {

std::string dispatch_error_result_json(ProviderToolCall const& call, ava::core::Error const& error)
{
  return "{\"tool\":\"" + ava::core::json::escape(call.name) + "\",\"ok\":false,\"error\":{\"category\":\"" +
         ava::core::json::escape(ava::core::to_string(error.category())) + "\",\"message\":\"" + ava::core::json::escape(error.message()) +
         "\",\"details\":\"" + ava::core::json::escape(error.format()) + "\"}}";
}

ToolDispatchResult synthetic_failed_dispatch_result(ProviderToolCall const& call, ava::core::Error const& error)
{
  return with_tool_result_payload(
      ToolDispatchResult{.call_id = call.id, .name = call.name, .success = false, .result_text = dispatch_error_result_json(call, error)});
}

ToolDispatchResult synthetic_truncated_provider_output_result(ProviderToolCall const& call)
{
  constexpr std::string_view message =
      "Provider output reached its token limit before this tool call was known to be complete. AVA did not execute it; reissue a complete call.";
  std::string result_text = "{\"tool\":\"" + ava::core::json::escape(call.name) +
                            "\",\"ok\":false,\"retryable\":false,\"error\":{\"category\":\"provider\","
                            "\"code\":\"provider_output_truncated\",\"message\":\"" +
                            ava::core::json::escape(message) + "\"}}";
  ToolResultPayload payload;
  payload.status = ToolResultStatus::Error;
  payload.summary = "Skipped incomplete tool call from truncated provider output";
  payload.content_type = "application/json";
  payload.error_category = "provider";
  payload.error_code = "provider_output_truncated";
  payload.error_message = message;
  payload.error_details = "The provider may reissue the complete call on the next turn.";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = false, .result_text = std::move(result_text), .payload = std::move(payload)};
}

bool is_run_command_call(ProviderToolCall const& call)
{
  return call.name == "bash";
}

std::string serialized_tool_arguments(ProviderToolCall const& call)
{
  if (is_run_command_call(call))
    return "{\"command\":\"" + ava::core::json::escape(kRedactedRunCommand) + "\"}";
  return call.arguments_json;
}

ParsedAssistantTurn persistable_turn(ParsedAssistantTurn const& turn)
{
  auto persisted = turn;
  for (auto& call : persisted.tool_calls) call.arguments_json = serialized_tool_arguments(call);
  for (auto& ordered : persisted.ordered_items)
  {
    if (auto* function = std::get_if<AssistantFunctionCallItem>(&ordered.item))
      function->tool_call.arguments_json = serialized_tool_arguments(function->tool_call);
  }
  return persisted;
}

bool error_has_context(ava::core::Error const& error, std::string_view key, std::string_view value)
{
  return std::ranges::any_of(error.context(), [&](ava::core::ErrorContext const& item) { return item.key == key && item.value == value; });
}

bool append_result_is_durable(ava::core::VoidResult const& appended)
{
  return appended || error_has_context(appended.error(), "append_commit_state", "committed_to_leased_inode");
}

bool permission_decision_cannot_ask(ava::tools::ToolContext const& context, ava::permissions::Operation operation, std::filesystem::path const& target_path)
{
  if (context.require_explicit_file_permissions && (operation == ava::permissions::Operation::ReadFile || operation == ava::permissions::Operation::EditFile))
    return false;
  auto const decision = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = operation,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = target_path,
      .command = "",
  });
  return decision.action != ava::permissions::PermissionAction::Ask;
}

bool preflight_read_file_parallel_ready(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return false;
  return permission_decision_cannot_ask(context, ava::permissions::Operation::ReadFile, tool_dispatch::workspace_path(context, *path));
}

bool preflight_list_directory_entries_cannot_ask(ava::tools::ToolContext const& context, std::filesystem::path const& path)
{
  std::error_code status_error;
  if (!std::filesystem::exists(path, status_error) || status_error)
    return true;
  if (!std::filesystem::is_directory(path, status_error) || status_error)
    return true;

  std::error_code iter_error;
  for (std::filesystem::directory_iterator it(path, iter_error), end; !iter_error && it != end; it.increment(iter_error))
  {
    if (!permission_decision_cannot_ask(context, ava::permissions::Operation::ReadFile, it->path()))
      return false;
  }
  return !iter_error;
}

bool preflight_list_directory_parallel_ready(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path_value = ava::core::json::string_field(call.arguments_json, "path");
  if (path_value)
  {
    if (auto safe = tool_dispatch::reject_control_arg(*path_value, "path", call.name); !safe)
      return false;
  }
  auto const path = tool_dispatch::workspace_path(context, path_value.value_or("."));
  if (!permission_decision_cannot_ask(context, ava::permissions::Operation::SearchFiles, path))
    return false;
  return preflight_list_directory_entries_cannot_ask(context, path);
}

bool preflight_glob_parallel_ready(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto pattern = tool_dispatch::required_safe_string_arg(call.arguments_json, "pattern", call.name);
  if (!pattern)
    return false;
  if (auto no_ignore_allowed = tool_dispatch::reject_provider_no_ignore(call.arguments_json, call.name); !no_ignore_allowed)
    return false;
  return permission_decision_cannot_ask(context, ava::permissions::Operation::SearchFiles, context.workspace_dir);
}

bool preflight_grep_parallel_ready(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto pattern = tool_dispatch::required_text_arg(call.arguments_json, "pattern", call.name);
  if (!pattern)
    return false;
  auto const include_value = ava::core::json::string_field(call.arguments_json, "include");
  if (include_value)
  {
    if (auto safe = tool_dispatch::reject_control_arg(*include_value, "include", call.name); !safe)
      return false;
  }
  if (auto no_ignore_allowed = tool_dispatch::reject_provider_no_ignore(call.arguments_json, call.name); !no_ignore_allowed)
    return false;
  if (auto literal = tool_dispatch::optional_bool_arg(call.arguments_json, "literal", true, call.name); !literal)
    return false;
  if (auto case_insensitive = tool_dispatch::optional_bool_arg(call.arguments_json, "case_insensitive", false, call.name); !case_insensitive)
    return false;
  return permission_decision_cannot_ask(context, ava::permissions::Operation::SearchFiles, context.workspace_dir);
}

bool preflight_parallel_ready(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  if (call.name == "read_file")
    return preflight_read_file_parallel_ready(context, call);
  if (call.name == "list_directory")
    return preflight_list_directory_parallel_ready(context, call);
  if (call.name == "glob")
    return preflight_glob_parallel_ready(context, call);
  if (call.name == "grep")
    return preflight_grep_parallel_ready(context, call);
  return false;
}

void mark_parallel_ready_slots(std::vector<ToolScheduleSlot>& schedule, ava::tools::ToolContext const& context)
{
  for (auto& slot : schedule)
  {
    if (slot.classification.eligibility != ToolScheduleEligibility::ReadOnlyCandidate)
      continue;
    if (preflight_parallel_ready(context, slot.call))
      slot.parallel_readiness = ToolScheduleParallelReadiness::PreflightProvenNonInteractive;
  }
}

bool is_parallel_ready_slot(ToolScheduleSlot const& slot) noexcept
{
  return slot.classification.eligibility == ToolScheduleEligibility::ReadOnlyCandidate &&
         slot.parallel_readiness == ToolScheduleParallelReadiness::PreflightProvenNonInteractive;
}

}  // namespace

bool is_scheduler_canceled_error(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

PendingCommittedToolResults::PendingCommittedToolResults(AgentTurnSession& session, ParsedAssistantTurn const& turn) noexcept : session_(session), turn_(turn)
{
}

PendingCommittedToolResults::~PendingCommittedToolResults() noexcept
{
  if (!armed_)
    return;

  auto terminal_status = ToolResultStatus::Error;
  try
  {
    if (session_.is_canceled())
      terminal_status = ToolResultStatus::Canceled;
  }
  catch (...)
  {
    // Cleanup remains best-effort and the original failure stays authoritative.
  }

  for (auto const& call : turn_.tool_calls)
  {
    auto const binding = unresolved_bindings_.find(call.id);
    if (binding == unresolved_bindings_.end())
      continue;
    try
    {
      auto synthetic = synthetic_terminal_tool_result(call, terminal_status);
      auto appended = session_.append_tool_result(synthetic, binding->second);
      if (append_result_is_durable(appended))
        unresolved_bindings_.erase(binding);
    }
    catch (...)
    {
      // Terminal audit persistence must never replace the original failure.
    }
  }
}

ava::core::VoidResult PendingCommittedToolResults::arm(PersistedAssistantTurn& persisted_turn)
{
  unresolved_bindings_.swap(persisted_turn.function_output_entry_ids_by_call_id);
  armed_ = true;
  for (auto const& call : turn_.tool_calls)
  {
    if (unresolved_bindings_.contains(call.id))
      continue;
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "committed assistant turn is missing a function output binding");
    error.with_context("call_id", call.id).with_context("tool_name", call.name);
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::Result<std::string_view> PendingCommittedToolResults::output_binding_for(ProviderToolCall const& call) const
{
  auto const tracked =
      std::ranges::find_if(turn_.tool_calls, [&](ProviderToolCall const& candidate) { return candidate.id == call.id && candidate.name == call.name; });
  auto const binding = unresolved_bindings_.find(call.id);
  if (tracked == turn_.tool_calls.end() || binding == unresolved_bindings_.end())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "committed assistant turn is missing a tracked function output binding");
    error.with_context("call_id", call.id).with_context("tool_name", call.name);
    return std::unexpected(std::move(error));
  }
  return binding->second;
}

void PendingCommittedToolResults::mark_result_durable(ProviderToolCall const& call, ava::core::VoidResult const& appended)
{
  if (!append_result_is_durable(appended))
    return;
  auto const tracked =
      std::ranges::find_if(turn_.tool_calls, [&](ProviderToolCall const& candidate) { return candidate.id == call.id && candidate.name == call.name; });
  if (tracked != turn_.tool_calls.end())
    unresolved_bindings_.erase(call.id);
}

ava::core::VoidResult AgentTurnExecutor::persist_assistant_turn(ProviderTurn const& provider_turn, PendingCommittedToolResults& pending_results)
{
  auto const& turn = provider_turn.assistant_turn;
  // Completing is the terminal arbitration boundary. Establish it before
  // persisting a terminal assistant so an accepted stop cannot label durable
  // success as canceled, while a later stop is a bounded no-op.
  if (turn.tool_calls.empty())
  {
    if (auto phase = publish_phase(RunPhase::Completing); !phase)
      return std::unexpected(std::move(phase.error()));
  }
  else
  {
    if (auto phase = publish_phase(RunPhase::PersistingAssistant); !phase)
      return std::unexpected(std::move(phase.error()));
  }

  // Permission prompts receive the transient provider call during dispatch.
  // Durable assistant output retains only a marker for raw bash arguments.
  auto persisted = session_.append_assistant_turn(persistable_turn(turn), provider_turn.usage, provider_turn.cost_usd);
  if (!persisted)
    return std::unexpected(std::move(persisted.error()));
  if (auto armed = pending_results.arm(*persisted); !armed)
    return std::unexpected(std::move(armed.error()));
  result_.committed_turn_id = persisted->committed_turn_id;

  // A tool call becomes finalized only after the complete v4 turn is durable.
  finalized_provider_tool_call_ids_.insert(provider_turn.iteration_tool_call_ids.begin(), provider_turn.iteration_tool_call_ids.end());
  return {};
}

ava::core::VoidResult AgentTurnExecutor::commit_truncated_provider_tool_results(ParsedAssistantTurn const& turn, PendingCommittedToolResults& pending_results)
{
  for (auto const& call : turn.tool_calls)
  {
    auto assistant_output_entry_id = pending_results.output_binding_for(call);
    if (!assistant_output_entry_id)
      return std::unexpected(std::move(assistant_output_entry_id.error()));
    auto appended = session_.append_tool_result(synthetic_truncated_provider_output_result(call), *assistant_output_entry_id);
    pending_results.mark_result_durable(call, appended);
    if (!appended)
      return std::unexpected(std::move(appended.error()));
  }
  return {};
}

ava::core::Result<ToolDispatchResult> AgentTurnExecutor::dispatch_and_commit_tool(ProviderToolCall const& call, PendingCommittedToolResults& pending_results)
{
  if (auto phase = publish_phase(RunPhase::ExecutingTools); !phase)
    return std::unexpected(std::move(phase.error()));
  if (auto not_canceled = session_.check_canceled("before_tool_dispatch"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));
  if (auto not_canceled = session_.check_canceled("before_tool_call_append"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));
  auto assistant_output_entry_id = pending_results.output_binding_for(call);
  if (!assistant_output_entry_id)
    return std::unexpected(std::move(assistant_output_entry_id.error()));

  ToolTimelineEntry timeline_entry{.status = ToolTimelineStatus::Running,
                                   .call_id = call.id,
                                   .name = call.name,
                                   .argument_summary = summarize_tool_arguments(call),
                                   .result_summary = "",
                                   .arguments_json = call.arguments_json};
  publish_tool_event(options_, timeline_entry);
  auto dispatch = dispatcher_storage_->dispatch(call);
  auto dispatch_result = dispatch ? *dispatch : synthetic_failed_dispatch_result(call, dispatch.error());
  dispatch_result.payload.summary = summarize_tool_result(dispatch_result);
  auto appended = session_.append_tool_result(dispatch_result, *assistant_output_entry_id);
  pending_results.mark_result_durable(call, appended);
  if (!appended)
    return std::unexpected(std::move(appended.error()));

  if (!dispatch)
    timeline_entry.status = ToolTimelineStatus::Error;
  else if (dispatch_result.payload.status == ToolResultStatus::Canceled)
    timeline_entry.status = ToolTimelineStatus::Canceled;
  else
    timeline_entry.status = dispatch_result.success ? ToolTimelineStatus::Success : ToolTimelineStatus::Error;
  timeline_entry.result_summary = dispatch_result.payload.summary;
  populate_tool_timeline_metadata(timeline_entry, dispatch_result);
  result_.tool_timeline.push_back(timeline_entry);
  publish_tool_event(options_, timeline_entry);
  ++result_.tool_calls;
  if (auto not_canceled = session_.check_canceled("after_tool_dispatch"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));
  return dispatch_result;
}

ava::core::VoidResult AgentTurnExecutor::commit_buffered_tool(ProviderToolCall const& call, ToolDispatchResult dispatch_result,
                                                              BufferedToolCallbacks const& callbacks, PendingCommittedToolResults& pending_results)
{
  dispatch_result.payload.summary = summarize_tool_result(dispatch_result);
  auto assistant_output_entry_id = pending_results.output_binding_for(call);
  if (!assistant_output_entry_id)
    return std::unexpected(std::move(assistant_output_entry_id.error()));
  ToolTimelineEntry timeline_entry{.status = ToolTimelineStatus::Running,
                                   .call_id = call.id,
                                   .name = call.name,
                                   .argument_summary = summarize_tool_arguments(call),
                                   .result_summary = "",
                                   .arguments_json = call.arguments_json};
  publish_tool_event(options_, timeline_entry);
  for (auto const& event : callbacks.permission_audits)
  {
    if (auto appended = session_.append_permission_decision(event); !appended)
      return std::unexpected(appended.error());
  }
  for (auto const& event : callbacks.progress_events)
  {
    auto published =
        publish_tool_progress(options_, ToolProgressEntry{.call_id = event.call_id, .name = event.tool_name, .text = event.text, .status = event.status});
    if (!published)
      return std::unexpected(std::move(published.error()));
  }
  auto appended = session_.append_tool_result(dispatch_result, *assistant_output_entry_id);
  pending_results.mark_result_durable(call, appended);
  if (!appended)
    return std::unexpected(std::move(appended.error()));

  timeline_entry.status = dispatch_result.payload.status == ToolResultStatus::Canceled
                              ? ToolTimelineStatus::Canceled
                              : (dispatch_result.success ? ToolTimelineStatus::Success : ToolTimelineStatus::Error);
  timeline_entry.result_summary = dispatch_result.payload.summary;
  populate_tool_timeline_metadata(timeline_entry, dispatch_result);
  result_.tool_timeline.push_back(timeline_entry);
  publish_tool_event(options_, timeline_entry);
  ++result_.tool_calls;
  return {};
}

ava::core::VoidResult AgentTurnExecutor::run_parallel_tool_epoch_and_commit(ParsedAssistantTurn const& turn, std::span<ToolScheduleSlot const> epoch,
                                                                            PendingCommittedToolResults& pending_results)
{
  // A parallel read/search epoch has one visible execution boundary; worker
  // scheduling itself must not publish duplicate phase changes.
  if (auto phase = publish_phase(RunPhase::ExecutingTools); !phase)
    return std::unexpected(std::move(phase.error()));
  if (auto not_canceled = session_.check_canceled("before_parallel_tool_epoch"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));

  auto const max_workers = std::max<std::size_t>(1, options_.parallel_read_search_max_workers);
  std::stop_source schedule_stop_source;
  std::vector<BufferedToolCallbacks> callbacks_by_provider_index(turn.tool_calls.size());
  std::vector<std::optional<ToolDispatchResult>> dispatch_results_by_provider_index(turn.tool_calls.size());
  std::mutex buffered_results_mutex;
  auto commit_recorded_prefix = [&]() -> ava::core::VoidResult {
    for (auto const& slot : epoch)
    {
      if (slot.provider_index >= dispatch_results_by_provider_index.size() || !dispatch_results_by_provider_index[slot.provider_index])
        break;
      auto const& callbacks = callbacks_by_provider_index[slot.provider_index];
      auto dispatch_result = *dispatch_results_by_provider_index[slot.provider_index];
      if (auto committed = commit_buffered_tool(slot.call, std::move(dispatch_result), callbacks, pending_results); !committed)
        return std::unexpected(std::move(committed.error()));
    }
    return {};
  };

  auto scheduled = run_parallel_tool_schedule(
      epoch,
      [&](ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ToolDispatchResult> {
        BufferedToolCallbacks callbacks;
        auto worker_context = *tool_context_storage_;
        // Preflight proved these slots non-interactive. Clear every live
        // ToolContext resolver that must not run from a worker thread, and pass
        // empty dispatch services so question/task/job cannot run off-thread.
        worker_context.permission_resolver = nullptr;
        worker_context.lsp_diagnostics_provider = nullptr;
        worker_context.permission_audit_sink = [&callbacks](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
          callbacks.permission_audits.push_back(event);
          return {};
        };
        worker_context.progress_sink = [&callbacks](ava::tools::ToolProgressEvent const& event) -> ava::core::VoidResult {
          callbacks.progress_events.push_back(event);
          return {};
        };
        worker_context.cancel_requested = [base_cancel = options_.cancel_requested, stop_token, &schedule_stop_source] {
          if (stop_token.stop_requested())
            return true;
          if (base_cancel && base_cancel())
          {
            schedule_stop_source.request_stop();
            return true;
          }
          return false;
        };

        auto dispatch = dispatcher_storage_->dispatch_with_context(std::move(worker_context), ToolDispatchServices{}, slot.call);
        auto dispatch_result = dispatch ? *dispatch : synthetic_failed_dispatch_result(slot.call, dispatch.error());
        dispatch_result.payload.summary = summarize_tool_result(dispatch_result);
        if (slot.provider_index < callbacks_by_provider_index.size())
        {
          std::lock_guard lock(buffered_results_mutex);
          callbacks_by_provider_index[slot.provider_index] = std::move(callbacks);
          dispatch_results_by_provider_index[slot.provider_index] = dispatch_result;
        }
        return dispatch_result;
      },
      ToolParallelScheduleOptions{.max_workers = max_workers, .stop_token = schedule_stop_source.get_token()});
  if (!scheduled)
  {
    auto schedule_error = std::move(scheduled.error());
    if (auto committed = commit_recorded_prefix(); !committed)
      return std::unexpected(std::move(committed.error()));
    if (is_scheduler_canceled_error(schedule_error))
    {
      if (auto not_canceled = session_.check_canceled("after_parallel_tool_epoch"); !not_canceled)
        return std::unexpected(std::move(not_canceled.error()));
    }
    return std::unexpected(std::move(schedule_error));
  }

  for (auto& outcome : *scheduled)
  {
    auto const& callbacks = callbacks_by_provider_index[outcome.slot.provider_index];
    if (auto committed = commit_buffered_tool(outcome.slot.call, std::move(outcome.result), callbacks, pending_results); !committed)
      return std::unexpected(std::move(committed.error()));
  }
  if (auto not_canceled = session_.check_canceled("after_parallel_tool_epoch"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));
  return {};
}

ava::core::VoidResult AgentTurnExecutor::execute_tools(ParsedAssistantTurn const& turn, PendingCommittedToolResults& pending_results)
{
  auto const registered_tool_metadata = dispatcher_storage_->registered_tool_metadata();
  auto schedule = build_sequential_tool_schedule(turn.tool_calls, registered_tool_metadata);
  if (!options_.parallel_read_search_tools)
  {
    auto scheduled = run_sequential_tool_schedule(
        schedule, [&](ToolScheduleSlot const& slot) -> ava::core::Result<ToolDispatchResult> { return dispatch_and_commit_tool(slot.call, pending_results); });
    if (!scheduled)
      return std::unexpected(std::move(scheduled.error()));
    return {};
  }

  mark_parallel_ready_slots(schedule, *tool_context_storage_);
  for (std::size_t index = 0; index < schedule.size();)
  {
    if (is_parallel_ready_slot(schedule[index]))
    {
      auto epoch_end = index + 1;
      while (epoch_end < schedule.size() && is_parallel_ready_slot(schedule[epoch_end])) ++epoch_end;
      auto epoch = std::span<ToolScheduleSlot const>(schedule).subspan(index, epoch_end - index);
      if (auto committed = run_parallel_tool_epoch_and_commit(turn, epoch, pending_results); !committed)
        return std::unexpected(std::move(committed.error()));
      index = epoch_end;
      continue;
    }

    auto dispatched = dispatch_and_commit_tool(schedule[index].call, pending_results);
    if (!dispatched)
      return std::unexpected(std::move(dispatched.error()));
    ++index;
  }
  return {};
}

}  // namespace ava::agent::detail
