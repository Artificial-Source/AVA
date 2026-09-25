#include "sys.h"
#include "ava/core/thread.h"
#include "ava/agent/tool_scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace ava::agent {
namespace {

ToolScheduleClassification classification(ToolScheduleEligibility eligibility, std::string reason)
{
  return ToolScheduleClassification{.eligibility = eligibility, .reason = std::move(reason)};
}

ToolMetadata const* find_metadata(std::string_view name, std::span<ToolMetadata const> tool_metadata) noexcept
{
  auto const match = std::find_if(tool_metadata.begin(), tool_metadata.end(), [name](ToolMetadata const& metadata) { return metadata.name == name; });
  if (match == tool_metadata.end())
    return nullptr;
  return &*match;
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept
{
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool contains(std::string_view value, std::string_view needle) noexcept
{
  return value.find(needle) != std::string_view::npos;
}

bool has_description_family(ToolMetadata const& metadata, std::string_view family) noexcept
{
  return metadata.description_family && *metadata.description_family == family;
}

bool is_builtin_read_search_candidate(ToolMetadata const& metadata) noexcept
{
  if (metadata.execution_mode != std::string_view("synchronous"))
    return false;
  if (metadata.permission_category != std::string_view("read") && metadata.permission_category != std::string_view("search"))
    return false;
  return metadata.name == std::string_view("read_file") || metadata.name == std::string_view("list_directory") || metadata.name == std::string_view("glob") ||
         metadata.name == std::string_view("grep");
}

bool can_run_in_parallel_epoch(ToolScheduleSlot const& slot) noexcept
{
  return slot.classification.eligibility == ToolScheduleEligibility::ReadOnlyCandidate &&
         slot.parallel_readiness == ToolScheduleParallelReadiness::PreflightProvenNonInteractive;
}

bool is_plugin_metadata(ToolMetadata const& metadata) noexcept
{
  return starts_with(metadata.permission_category, "plugin.") || contains(metadata.execution_mode, "plugin") || has_description_family(metadata, "plugin");
}

bool is_mcp_metadata(ToolMetadata const& metadata) noexcept
{
  return starts_with(metadata.permission_category, "mcp.") || contains(metadata.execution_mode, "mcp") || has_description_family(metadata, "mcp");
}

bool is_lsp_metadata(ToolMetadata const& metadata) noexcept
{
  return starts_with(metadata.permission_category, "lsp.") || has_description_family(metadata, "lsp");
}

ava::core::Error scheduler_canceled_error(std::string_view phase)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "tool schedule canceled", ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  error.with_context("phase", std::string(phase));
  return error;
}

ava::core::Error worker_exception_error(ToolScheduleSlot const& slot, std::string cause)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "tool schedule executor threw");
  error.with_context("tool", slot.call.name);
  error.with_context("call_id", slot.call.id);
  error.with_context("provider_index", std::to_string(slot.provider_index));
  error.with_context("cause", std::move(cause));
  return error;
}

struct ParallelEpochState
{
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::optional<ava::core::Result<ToolDispatchResult>>> results;
  std::atomic_size_t completed = 0;
  std::atomic_size_t stop_after = std::numeric_limits<std::size_t>::max();
  std::atomic_bool cancel_requested = false;
};

struct ActiveEpochWorker
{
  std::size_t epoch_offset = 0;
  ava::core::JoinThread thread;
};

void request_epoch_cancellation(ParallelEpochState& state)
{
  {
    std::lock_guard lock(state.mutex);
    state.cancel_requested.store(true);
  }
  state.changed.notify_all();
}

bool has_completed_active_worker(ParallelEpochState const& state, std::vector<ActiveEpochWorker> const& active)
{
  return std::any_of(active.begin(), active.end(), [&state](ActiveEpochWorker const& worker) { return state.results[worker.epoch_offset].has_value(); });
}

void request_stop_for_active_workers(std::vector<ActiveEpochWorker>& active, std::size_t after_offset)
{
  for (auto& worker : active)
  {
    if (worker.epoch_offset > after_offset)
      worker.thread.request_stop();
  }
}

void request_stop_for_all_active_workers(std::vector<ActiveEpochWorker>& active)
{
  for (auto& worker : active) worker.thread.request_stop();
}

ava::core::Result<ToolDispatchResult> execute_parallel_worker(ToolParallelScheduleExecutor const& executor, ToolScheduleSlot const& slot,
                                                              std::stop_token stop_token)
{
  if (stop_token.stop_requested())
    return std::unexpected(scheduler_canceled_error("worker_start"));
  try
  {
    return executor(slot, stop_token);
  }
  catch (std::exception const& error)
  {
    return std::unexpected(worker_exception_error(slot, error.what()));
  }
  catch (...)
  {
    return std::unexpected(worker_exception_error(slot, "unknown exception"));
  }
}

std::optional<ava::core::Error> launch_epoch_worker(std::span<ToolScheduleSlot const> epoch, ToolParallelScheduleExecutor const& executor,
                                                    ParallelEpochState& state, std::vector<ActiveEpochWorker>& active, std::size_t offset)
{
  try
  {
    auto thread = ava::core::JoinThread::create("tool_scheduler", [&executor, &state, epoch, offset](std::stop_token stop_token) {
      auto result = execute_parallel_worker(executor, epoch[offset], stop_token);
      bool const failed = !result;
      {
        std::lock_guard lock(state.mutex);
        state.results[offset] = std::move(result);
        if (failed)
        {
          auto current = state.stop_after.load();
          while (offset < current && !state.stop_after.compare_exchange_weak(current, offset))
          {
          }
        }
        state.completed.fetch_add(1);
      }
      state.changed.notify_all();
    });
    active.push_back(ActiveEpochWorker{.epoch_offset = offset, .thread = std::move(thread)});
    return std::nullopt;
  }
  catch (std::system_error const& error)
  {
    auto result_error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to start tool schedule worker");
    result_error.with_context("provider_index", std::to_string(epoch[offset].provider_index));
    result_error.with_context("cause", error.what());
    return result_error;
  }
  catch (std::exception const& error)
  {
    auto result_error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to start tool schedule worker");
    result_error.with_context("provider_index", std::to_string(epoch[offset].provider_index));
    result_error.with_context("cause", error.what());
    return result_error;
  }
}

void retire_completed_workers(ParallelEpochState& state, std::vector<ActiveEpochWorker>& active)
{
  std::erase_if(active, [&state](ActiveEpochWorker const& worker) {
    std::lock_guard lock(state.mutex);
    return state.results[worker.epoch_offset].has_value();
  });
}

ava::core::Result<std::vector<ToolScheduleOutcome>> collect_epoch_outcomes(std::span<ToolScheduleSlot const> epoch, ParallelEpochState& state)
{
  std::vector<ToolScheduleOutcome> outcomes;
  outcomes.reserve(epoch.size());
  for (std::size_t offset = 0; offset < epoch.size(); ++offset)
  {
    if (!state.results[offset])
      return std::unexpected(scheduler_canceled_error("parallel_epoch"));
    auto result = std::move(*state.results[offset]);
    if (!result)
      return std::unexpected(std::move(result.error()));
    outcomes.push_back(ToolScheduleOutcome{.slot = epoch[offset], .result = std::move(*result)});
  }
  return outcomes;
}

ava::core::Result<std::vector<ToolScheduleOutcome>> run_parallel_epoch(std::span<ToolScheduleSlot const> epoch, ToolParallelScheduleExecutor const& executor,
                                                                       ToolParallelScheduleOptions const& options)
{
  ParallelEpochState state;
  state.results.resize(epoch.size());
  std::vector<ActiveEpochWorker> active;
  active.reserve(std::min(options.max_workers, epoch.size()));

  std::stop_callback notify_external_stop(options.stop_token, [&state] { request_epoch_cancellation(state); });

  std::size_t next_to_launch = 0;
  std::optional<ava::core::Error> launch_error;
  while (state.completed.load() < epoch.size() && !launch_error)
  {
    retire_completed_workers(state, active);
    if (options.stop_token.stop_requested())
      request_epoch_cancellation(state);
    if (state.cancel_requested.load())
      request_stop_for_all_active_workers(active);
    request_stop_for_active_workers(active, state.stop_after.load());

    while (!state.cancel_requested.load() && active.size() < options.max_workers && next_to_launch < epoch.size() && next_to_launch <= state.stop_after.load())
    {
      if (auto error = launch_epoch_worker(epoch, executor, state, active, next_to_launch))
      {
        launch_error = std::move(*error);
        break;
      }
      ++next_to_launch;
    }
    if (launch_error)
    {
      request_stop_for_all_active_workers(active);
      break;
    }
    retire_completed_workers(state, active);
    // A worker may publish a lower stop_after and finish between the earlier
    // stop scan and retirement. Apply that transition before waiting even when
    // the completed worker has already been removed from active.
    request_stop_for_active_workers(active, state.stop_after.load());
    if (state.completed.load() >= epoch.size())
      break;
    if (!state.cancel_requested.load() && active.size() < options.max_workers && next_to_launch < epoch.size() && next_to_launch <= state.stop_after.load())
    {
      continue;
    }
    if (active.empty())
    {
      if (!state.cancel_requested.load() && next_to_launch < epoch.size() && next_to_launch <= state.stop_after.load())
        continue;
      break;
    }

    std::unique_lock lock(state.mutex);
    auto const observed_completed = state.completed.load();
    auto const observed_stop_after = state.stop_after.load();
    state.changed.wait(lock, [&] {
      return state.completed.load() != observed_completed || state.stop_after.load() != observed_stop_after || state.cancel_requested.load() ||
             has_completed_active_worker(state, active);
    });
  }

  request_stop_for_all_active_workers(active);
  active.clear();

  if (launch_error)
    return std::unexpected(std::move(*launch_error));
  if (state.cancel_requested.load() || options.stop_token.stop_requested())
    return std::unexpected(scheduler_canceled_error("parallel_epoch"));
  return collect_epoch_outcomes(epoch, state);
}

}  // namespace

ToolScheduleClassification classify_tool_for_scheduling(ProviderToolCall const& call, std::span<ToolMetadata const> tool_metadata)
{
  auto const* metadata = find_metadata(call.name, tool_metadata);
  if (metadata == nullptr)
    return classification(ToolScheduleEligibility::Deferred, "unknown_tool");

  if (is_plugin_metadata(*metadata))
    return classification(ToolScheduleEligibility::Deferred, "plugin_brokered_external");
  if (is_mcp_metadata(*metadata))
    return classification(ToolScheduleEligibility::Deferred, "mcp_brokered_external");
  if (is_lsp_metadata(*metadata))
    return classification(ToolScheduleEligibility::Barrier, "lsp");
  if (metadata->permission_category == std::string_view("edit") || metadata->name == std::string_view("write_file") ||
      metadata->name == std::string_view("edit_file") || metadata->name == std::string_view("apply_patch"))
  {
    return classification(ToolScheduleEligibility::Barrier, "mutation");
  }
  if (metadata->permission_category == std::string_view("bash") || metadata->name == std::string_view("bash"))
    return classification(ToolScheduleEligibility::Barrier, "shell");
  if (metadata->permission_category == std::string_view("user") || metadata->execution_mode == std::string_view("synchronous_user_interaction") ||
      metadata->name == std::string_view("question") || metadata->name == std::string_view("todowrite"))
  {
    return classification(ToolScheduleEligibility::Barrier, "user_interaction");
  }
  if (metadata->permission_category == std::string_view("task") || metadata->execution_mode == std::string_view("subagent") ||
      metadata->name == std::string_view("task") || metadata->name == std::string_view("job"))
  {
    return classification(ToolScheduleEligibility::Barrier, "subagent");
  }
  if (metadata->permission_category == std::string_view("skill") || metadata->name == std::string_view("skill") || has_description_family(*metadata, "skills"))
    return classification(ToolScheduleEligibility::Barrier, "skill");
  if (metadata->execution_mode == std::string_view("synchronous_network") || starts_with(metadata->permission_category, "network."))
    return classification(ToolScheduleEligibility::Deferred, "network");
  if (metadata->execution_mode == std::string_view("synchronous_process"))
    return classification(ToolScheduleEligibility::Barrier, "process");
  if (is_builtin_read_search_candidate(*metadata))
    return classification(ToolScheduleEligibility::ReadOnlyCandidate, "read_search_candidate");
  if (metadata->execution_mode != std::string_view("synchronous"))
    return classification(ToolScheduleEligibility::Barrier, "unreviewed_execution_mode");
  return classification(ToolScheduleEligibility::Barrier, "unreviewed_synchronous_tool");
}

std::vector<ToolScheduleSlot> build_sequential_tool_schedule(std::span<ProviderToolCall const> calls, std::span<ToolMetadata const> tool_metadata)
{
  std::vector<ToolScheduleSlot> schedule;
  schedule.reserve(calls.size());
  for (std::size_t index = 0; index < calls.size(); ++index)
  {
    schedule.push_back(
        ToolScheduleSlot{.provider_index = index, .call = calls[index], .classification = classify_tool_for_scheduling(calls[index], tool_metadata)});
  }
  return schedule;
}

ava::core::Result<std::vector<ToolScheduleOutcome>> run_sequential_tool_schedule(std::span<ToolScheduleSlot const> schedule,
                                                                                 ToolScheduleExecutor const& executor)
{
  if (!executor)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "tool schedule executor is required"));
  }

  std::vector<ToolScheduleOutcome> outcomes;
  outcomes.reserve(schedule.size());
  for (auto const& slot : schedule)
  {
    auto executed = executor(slot);
    if (!executed)
      return std::unexpected(std::move(executed.error()));
    auto result = std::move(*executed);
    outcomes.push_back(ToolScheduleOutcome{.slot = slot, .result = std::move(result)});
  }
  return outcomes;
}

ava::core::Result<std::vector<ToolScheduleOutcome>> run_parallel_tool_schedule(std::span<ToolScheduleSlot const> schedule,
                                                                               ToolParallelScheduleExecutor const& executor,
                                                                               ToolParallelScheduleOptions options)
{
  if (!executor)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "tool schedule executor is required"));
  }
  if (options.max_workers == 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "tool schedule max_workers must be greater than zero"));
  }

  std::vector<ToolScheduleOutcome> outcomes;
  outcomes.reserve(schedule.size());
  for (std::size_t index = 0; index < schedule.size();)
  {
    if (options.stop_token.stop_requested())
      return std::unexpected(scheduler_canceled_error("before_slot"));

    if (!can_run_in_parallel_epoch(schedule[index]))
    {
      auto executed = execute_parallel_worker(executor, schedule[index], options.stop_token);
      if (!executed)
        return std::unexpected(std::move(executed.error()));
      outcomes.push_back(ToolScheduleOutcome{.slot = schedule[index], .result = std::move(*executed)});
      ++index;
      continue;
    }

    auto epoch_end = index + 1;
    while (epoch_end < schedule.size() && can_run_in_parallel_epoch(schedule[epoch_end])) ++epoch_end;
    auto epoch = run_parallel_epoch(schedule.subspan(index, epoch_end - index), executor, options);
    if (!epoch)
      return std::unexpected(std::move(epoch.error()));
    outcomes.insert(outcomes.end(), std::make_move_iterator(epoch->begin()), std::make_move_iterator(epoch->end()));
    index = epoch_end;
  }
  return outcomes;
}

}  // namespace ava::agent
