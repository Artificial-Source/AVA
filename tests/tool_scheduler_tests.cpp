#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/tool_scheduler.h"
#include "ava/core/error.h"
#include "ava/core/mode.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using ava::agent::ToolScheduleEligibility;
using ava::agent::ToolScheduleParallelReadiness;

ava::agent::ProviderToolCall call(std::string_view id, std::string_view name)
{
  return ava::agent::ProviderToolCall{.id = std::string(id), .name = std::string(name), .arguments_json = "{}"};
}

ava::agent::ToolDispatchResult successful_result(ava::agent::ToolScheduleSlot const& slot)
{
  return ava::agent::ToolDispatchResult{.call_id = slot.call.id, .name = slot.call.name, .success = true, .result_text = "ok"};
}

void expect_classification(ava::agent::ToolScheduleClassification const& actual, ToolScheduleEligibility eligibility, std::string_view reason,
                           std::string_view message)
{
  ava::tests::expect_all(std::string(message), {{"eligibility", actual.eligibility == eligibility}, {"reason", actual.reason == reason}});
}

void expect_builtin_classification(std::string_view name, ToolScheduleEligibility eligibility, std::string_view reason)
{
  auto const actual = ava::agent::classify_tool_for_scheduling(call("call", name), ava::agent::builtin_tool_metadata());
  expect_classification(actual, eligibility, reason, std::string("scheduler classifies builtin ") + std::string(name) + " as " + std::string(reason));
}

ava::agent::ToolMetadata synthetic_metadata(std::string_view name, std::string_view permission_category)
{
  return ava::agent::ToolMetadata{.name = name,
                                  .description = "synthetic tool",
                                  .schema_json = "{}",
                                  .permission_category = permission_category,
                                  .output_bound_summary = "bounded",
                                  .execution_mode = "synchronous",
                                  .event_rendering_hint = "synthetic",
                                  .description_family = std::nullopt};
}

ava::agent::ToolScheduleSlot schedule_slot(std::size_t provider_index, std::string_view id, std::string_view name)
{
  return ava::agent::ToolScheduleSlot{
      .provider_index = provider_index,
      .call = call(id, name),
      .classification = ava::agent::ToolScheduleClassification{.eligibility = ToolScheduleEligibility::Barrier, .reason = "test"}};
}

ava::agent::ToolScheduleSlot parallel_read_slot(std::size_t provider_index, std::string_view id, std::string_view name = "read_file")
{
  return ava::agent::ToolScheduleSlot{
      .provider_index = provider_index,
      .call = call(id, name),
      .classification = ava::agent::ToolScheduleClassification{.eligibility = ToolScheduleEligibility::ReadOnlyCandidate, .reason = "read_search_candidate"},
      .parallel_readiness = ToolScheduleParallelReadiness::PreflightProvenNonInteractive};
}

ava::agent::ToolScheduleSlot non_ready_read_slot(std::size_t provider_index, std::string_view id, std::string_view name = "read_file")
{
  return ava::agent::ToolScheduleSlot{
      .provider_index = provider_index,
      .call = call(id, name),
      .classification = ava::agent::ToolScheduleClassification{.eligibility = ToolScheduleEligibility::ReadOnlyCandidate, .reason = "read_search_candidate"}};
}

ava::agent::ToolScheduleSlot barrier_slot(std::size_t provider_index, std::string_view id, std::string_view name = "write_file")
{
  return ava::agent::ToolScheduleSlot{
      .provider_index = provider_index,
      .call = call(id, name),
      .classification = ava::agent::ToolScheduleClassification{.eligibility = ToolScheduleEligibility::Barrier, .reason = "mutation"}};
}

ava::agent::ToolScheduleSlot deferred_slot(std::size_t provider_index, std::string_view id, std::string_view name = "webfetch")
{
  return ava::agent::ToolScheduleSlot{
      .provider_index = provider_index,
      .call = call(id, name),
      .classification = ava::agent::ToolScheduleClassification{.eligibility = ToolScheduleEligibility::Deferred, .reason = "network"}};
}

ava::core::Result<ava::agent::ToolDispatchResult> canceled_executor_result(ava::agent::ToolScheduleSlot const& slot)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "executor canceled");
  error.with_context("canceled", "true");
  error.with_context("provider_index", std::to_string(slot.provider_index));
  return std::unexpected(std::move(error));
}

ava::core::Result<ava::agent::ToolDispatchResult> executor_error(ava::agent::ToolScheduleSlot const& slot, std::string message)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Tool, std::move(message));
  error.with_context("provider_index", std::to_string(slot.provider_index));
  return std::unexpected(std::move(error));
}

bool has_error_context(ava::core::Error const& error, std::string_view key, std::string_view value)
{
  return std::ranges::any_of(error.context(), [key, value](ava::core::ErrorContext const& item) { return item.key == key && item.value == value; });
}

template <typename Predicate>
void wait_for_test(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, Predicate predicate, std::string const& message)
{
  bool const ready = changed.wait_until(lock, ava::tests::now_plus_seconds(5), predicate);
  expect(ready, message);
}

void test_classify_builtin_tools_for_scheduling()
{
  expect_builtin_classification("read_file", ToolScheduleEligibility::ReadOnlyCandidate, "read_search_candidate");
  expect_builtin_classification("list_directory", ToolScheduleEligibility::ReadOnlyCandidate, "read_search_candidate");
  expect_builtin_classification("glob", ToolScheduleEligibility::ReadOnlyCandidate, "read_search_candidate");
  expect_builtin_classification("grep", ToolScheduleEligibility::ReadOnlyCandidate, "read_search_candidate");

  expect_builtin_classification("write_file", ToolScheduleEligibility::Barrier, "mutation");
  expect_builtin_classification("edit_file", ToolScheduleEligibility::Barrier, "mutation");
  expect_builtin_classification("apply_patch", ToolScheduleEligibility::Barrier, "mutation");
  expect_builtin_classification("bash", ToolScheduleEligibility::Barrier, "shell");
  expect_builtin_classification("question", ToolScheduleEligibility::Barrier, "user_interaction");
  expect_builtin_classification("todowrite", ToolScheduleEligibility::Barrier, "user_interaction");
  expect_builtin_classification("task", ToolScheduleEligibility::Barrier, "subagent");
  expect_builtin_classification("skill", ToolScheduleEligibility::Barrier, "skill");

  expect_builtin_classification("lsp_diagnostics", ToolScheduleEligibility::Barrier, "lsp");
  expect_builtin_classification("lsp_document_symbols", ToolScheduleEligibility::Barrier, "lsp");
  expect_builtin_classification("lsp_workspace_symbols", ToolScheduleEligibility::Barrier, "lsp");
  expect_builtin_classification("lsp_definition", ToolScheduleEligibility::Barrier, "lsp");
  expect_builtin_classification("lsp_references", ToolScheduleEligibility::Barrier, "lsp");

  expect_builtin_classification("webfetch", ToolScheduleEligibility::Deferred, "network");
  expect_builtin_classification("websearch", ToolScheduleEligibility::Deferred, "network");
  expect_builtin_classification("not_registered", ToolScheduleEligibility::Deferred, "unknown_tool");
}

void test_classify_brokered_external_metadata_for_scheduling()
{
  std::array metadata{
      synthetic_metadata("plugin_tool", "plugin.tool.call"),
      synthetic_metadata("mcp_tool", "mcp.tool.call"),
  };

  expect_classification(ava::agent::classify_tool_for_scheduling(call("plugin_call", "plugin_tool"), metadata), ToolScheduleEligibility::Deferred,
                        "plugin_brokered_external", "scheduler defers plugin brokered external tools");
  expect_classification(ava::agent::classify_tool_for_scheduling(call("mcp_call", "mcp_tool"), metadata), ToolScheduleEligibility::Deferred,
                        "mcp_brokered_external", "scheduler defers MCP brokered external tools");
}

void test_build_sequential_tool_schedule_preserves_provider_order()
{
  std::vector calls{
      call("call_0", "grep"),
      call("call_1", "bash"),
      call("call_2", "webfetch"),
  };

  auto const schedule = ava::agent::build_sequential_tool_schedule(calls, ava::agent::builtin_tool_metadata());

  expect(schedule.size() == calls.size(), "sequential tool schedule keeps one slot per provider call");
  expect(schedule.size() == 3 && schedule[0].provider_index == 0 && schedule[0].call.id == "call_0" && schedule[0].call.name == "grep",
         "sequential tool schedule keeps the first provider call at index 0");
  expect(schedule.size() == 3 && schedule[1].provider_index == 1 && schedule[1].call.id == "call_1" && schedule[1].call.name == "bash",
         "sequential tool schedule keeps the second provider call at index 1");
  expect(schedule.size() == 3 && schedule[2].provider_index == 2 && schedule[2].call.id == "call_2" && schedule[2].call.name == "webfetch",
         "sequential tool schedule keeps the third provider call at index 2");
}

void test_run_sequential_tool_schedule_executes_in_order()
{
  std::vector schedule{
      schedule_slot(0, "call_0", "first"),
      schedule_slot(1, "call_1", "second"),
      schedule_slot(2, "call_2", "third"),
  };
  std::vector<std::size_t> execution_order;

  auto outcomes = ava::agent::run_sequential_tool_schedule(schedule, [&](ava::agent::ToolScheduleSlot const& slot) {
    execution_order.push_back(slot.provider_index);
    return successful_result(slot);
  });

  expect(outcomes.has_value(), outcomes ? "sequential scheduler executor succeeds" : outcomes.error().format());
  expect(execution_order == std::vector<std::size_t>{0, 1, 2}, "sequential scheduler invokes the executor in provider order");
  expect(outcomes && outcomes->size() == 3 && (*outcomes)[0].slot.provider_index == 0 && (*outcomes)[1].slot.provider_index == 1 &&
             (*outcomes)[2].slot.provider_index == 2,
         "sequential scheduler returns outcomes in provider order");
}

void test_run_sequential_tool_schedule_short_circuits_on_executor_error()
{
  std::vector schedule{
      schedule_slot(0, "call_0", "first"),
      schedule_slot(1, "call_1", "second"),
      schedule_slot(2, "call_2", "third"),
  };
  std::vector<std::size_t> execution_order;

  auto outcomes = ava::agent::run_sequential_tool_schedule(schedule, [&](ava::agent::ToolScheduleSlot const& slot) {
    execution_order.push_back(slot.provider_index);
    if (slot.provider_index == 1)
      return ava::core::Result<ava::agent::ToolDispatchResult>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "scheduler executor failed")));
    return ava::core::Result<ava::agent::ToolDispatchResult>(successful_result(slot));
  });

  expect(!outcomes && outcomes.error().message() == "scheduler executor failed", "sequential scheduler returns the first executor error");
  expect(execution_order == std::vector<std::size_t>{0, 1}, "sequential scheduler does not run slots after an executor error");
}

void test_run_sequential_tool_schedule_converts_executor_exception()
{
  std::vector schedule{schedule_slot(0, "call_throw", "read_file"), schedule_slot(1, "call_after", "read_file")};
  for (bool const standard_exception : {true, false})
  {
    std::size_t calls = 0;
    auto outcomes = ava::agent::run_sequential_tool_schedule(schedule, [&](auto const&) -> ava::core::Result<ava::agent::ToolDispatchResult> {
      ++calls;
      if (standard_exception)
        throw std::runtime_error("sequential executor failed");
      throw 7;
    });
    expect(!outcomes && outcomes.error().category() == ava::core::ErrorCategory::Tool && calls == 1,
           "sequential scheduler contains executor exceptions and stops before later slots");
    if (!outcomes)
    {
      auto const detail = outcomes.error().format();
      expect(detail.find("call_throw") != std::string::npos && detail.find("read_file") != std::string::npos &&
                 detail.find(standard_exception ? "sequential executor failed" : "unknown exception") != std::string::npos,
             "sequential scheduler preserves call, tool and exception context");
    }
  }
}

void test_run_sequential_tool_schedule_rejects_null_executor()
{
  std::vector schedule{schedule_slot(0, "call_0", "read_file")};
  ava::agent::ToolScheduleExecutor executor;

  auto outcomes = ava::agent::run_sequential_tool_schedule(schedule, executor);

  expect(!outcomes && outcomes.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             outcomes.error().message() == "tool schedule executor is required",
         "sequential scheduler rejects a null executor");
}

void test_run_parallel_tool_schedule_splits_epochs_at_barriers()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0", "read_file"),
      parallel_read_slot(1, "call_1", "grep"),
      barrier_slot(2, "call_2", "write_file"),
      parallel_read_slot(3, "call_3", "glob"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t first_epoch_started = 0;
    bool release_first_epoch = false;
    bool barrier_started = false;
    bool barrier_finished = false;
    bool read_after_barrier_started = false;
    bool read_observed_finished_barrier = false;
  } state;

  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          if (stop_token.stop_requested())
            return canceled_executor_result(slot);
          if (slot.provider_index <= 1)
          {
            std::unique_lock lock(state.mutex);
            ++state.first_epoch_started;
            state.changed.notify_all();
            std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
            state.changed.wait(lock, [&] { return state.release_first_epoch || stop_token.stop_requested(); });
            if (stop_token.stop_requested())
              return canceled_executor_result(slot);
            return successful_result(slot);
          }
          if (slot.provider_index == 2)
          {
            std::lock_guard lock(state.mutex);
            state.barrier_started = true;
            state.barrier_finished = true;
            state.changed.notify_all();
            return successful_result(slot);
          }

          std::lock_guard lock(state.mutex);
          state.read_after_barrier_started = true;
          state.read_observed_finished_barrier = state.barrier_finished;
          state.changed.notify_all();
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 4});
  });

  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.first_epoch_started == 2; }, "parallel scheduler starts the first read/search epoch");
    expect(!state.barrier_started && !state.read_after_barrier_started, "barrier and later read/search slot do not join the earlier read/search epoch");
    state.release_first_epoch = true;
    state.changed.notify_all();
  }
  runner.join();
  expect(state.read_observed_finished_barrier, "read after a barrier starts only after the barrier finished");

  expect(scheduled.has_value(), "parallel epoch splitting test stores a scheduler result");
  expect(scheduled && scheduled->has_value(), scheduled && !*scheduled ? scheduled->error().format() : "parallel scheduler succeeds across split epochs");
  expect(scheduled && *scheduled && (*scheduled)->size() == 4 && (*scheduled)->at(0).slot.provider_index == 0 && (*scheduled)->at(1).slot.provider_index == 1 &&
             (*scheduled)->at(2).slot.provider_index == 2 && (*scheduled)->at(3).slot.provider_index == 3,
         "parallel scheduler returns split epoch outcomes in provider order");
}

void test_run_parallel_tool_schedule_returns_provider_order_after_reverse_completion()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0", "read_file"),
      parallel_read_slot(1, "call_1", "list_directory"),
      parallel_read_slot(2, "call_2", "grep"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<bool> started = std::vector<bool>(3, false);
    std::vector<bool> released = std::vector<bool>(3, false);
    std::size_t started_count = 0;
    std::vector<std::size_t> completion_order;
  } state;

  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          auto const index = slot.provider_index;
          std::unique_lock lock(state.mutex);
          if (!state.started[index])
          {
            state.started[index] = true;
            ++state.started_count;
          }
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          state.changed.wait(lock, [&] { return state.released[index] || stop_token.stop_requested(); });
          if (stop_token.stop_requested())
            return canceled_executor_result(slot);
          state.completion_order.push_back(index);
          state.changed.notify_all();
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 3});
  });

  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.started_count == 3; }, "parallel scheduler starts all reverse-order test workers");
    state.released[2] = true;
    state.changed.notify_all();
    wait_for_test(
        state.changed, lock, [&] { return state.completion_order == std::vector<std::size_t>{2}; },
        "reverse-order test completes the last provider slot first");
    state.released[1] = true;
    state.changed.notify_all();
    wait_for_test(
        state.changed, lock, [&] { return state.completion_order == std::vector<std::size_t>{2, 1}; },
        "reverse-order test completes the middle provider slot second");
    state.released[0] = true;
    state.changed.notify_all();
  }
  runner.join();

  expect(scheduled && scheduled->has_value(), scheduled && !*scheduled ? scheduled->error().format() : "parallel scheduler handles reverse completion order");
  expect(scheduled && *scheduled && (*scheduled)->size() == 3 && (*scheduled)->at(0).slot.provider_index == 0 && (*scheduled)->at(1).slot.provider_index == 1 &&
             (*scheduled)->at(2).slot.provider_index == 2,
         "parallel scheduler collects reverse-completed workers in provider order");
}

void test_run_parallel_tool_schedule_respects_worker_cap()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"), parallel_read_slot(1, "call_1"), parallel_read_slot(2, "call_2"),
      parallel_read_slot(3, "call_3"), parallel_read_slot(4, "call_4"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<bool> released = std::vector<bool>(5, false);
    std::vector<std::size_t> start_order;
    std::size_t active = 0;
    std::size_t max_active = 0;
  } state;

  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          auto const index = slot.provider_index;
          std::unique_lock lock(state.mutex);
          state.start_order.push_back(index);
          ++state.active;
          state.max_active = std::max(state.max_active, state.active);
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          state.changed.wait(lock, [&] { return state.released[index] || stop_token.stop_requested(); });
          --state.active;
          state.changed.notify_all();
          if (stop_token.stop_requested())
            return canceled_executor_result(slot);
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 2});
  });

  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.start_order.size() == 2; }, "worker-cap test starts only the first capped batch");
    auto first_batch = state.start_order;
    std::ranges::sort(first_batch);
    expect(state.active == 2 && state.max_active == 2 && first_batch == std::vector<std::size_t>{0, 1},
           "parallel scheduler does not exceed the configured worker cap before release");
    state.released[0] = true;
    state.changed.notify_all();
    wait_for_test(state.changed, lock, [&] { return state.start_order.size() == 3; }, "worker-cap test starts one new worker after one completes");
    auto after_release = state.start_order;
    std::ranges::sort(after_release);
    expect(state.max_active <= 2 && after_release == std::vector<std::size_t>{0, 1, 2},
           "parallel scheduler keeps provider-order launch while honoring max_workers");
    for (auto&& released : state.released) released = true;
    state.changed.notify_all();
  }
  runner.join();

  expect(scheduled && scheduled->has_value(), scheduled && !*scheduled ? scheduled->error().format() : "parallel scheduler succeeds with a worker cap");
  expect(state.max_active <= 2, "parallel scheduler never exceeds max_workers");
}

void test_run_parallel_tool_schedule_replaces_immediately_completed_capped_workers()
{
  constexpr std::size_t slot_count = 256;
  std::vector<ava::agent::ToolScheduleSlot> schedule;
  schedule.reserve(slot_count);
  for (std::size_t index = 0; index < slot_count; ++index)
  {
    auto const id = "call_" + std::to_string(index);
    schedule.push_back(parallel_read_slot(index, id));
  }

  struct CompletionState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false;
  } completion;

  std::atomic_size_t executed = 0;
  std::stop_source cancellation;
  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          executed.fetch_add(1, std::memory_order_relaxed);
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 4, .stop_token = cancellation.get_token()});
    {
      std::lock_guard lock(completion.mutex);
      completion.finished = true;
    }
    completion.changed.notify_all();
  });

  bool finished = false;
  {
    std::unique_lock lock(completion.mutex);
    finished = completion.changed.wait_until(lock, ava::tests::now_plus_seconds(10), [&] { return completion.finished; });
  }
  if (!finished)
    static_cast<void>(cancellation.request_stop());
  runner.join();

  expect(finished, "parallel scheduler replaces immediately completed capped workers without losing a wakeup");
  if (!finished)
    return;

  expect(scheduled && scheduled->has_value(),
         scheduled && !scheduled->has_value() ? scheduled->error().format() : "immediate-completion capped schedule returns a result");
  expect(executed.load(std::memory_order_relaxed) == slot_count, "immediate-completion capped schedule executes every worker");

  bool provider_order = scheduled && scheduled->has_value() && (*scheduled)->size() == slot_count;
  if (provider_order)
  {
    for (std::size_t index = 0; index < slot_count; ++index) provider_order = provider_order && (*scheduled)->at(index).slot.provider_index == index;
  }
  expect(provider_order, "immediate-completion capped schedule returns every outcome in provider order");
}

void test_run_parallel_tool_schedule_refills_capacity_after_retiring_a_completed_worker()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"),
      parallel_read_slot(1, "call_1"),
      parallel_read_slot(2, "call_2"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool second_started = false;
    bool queued_started = false;
    bool finished = false;
  } state;

  std::stop_source cancellation;
  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          std::unique_lock lock(state.mutex);
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          if (slot.provider_index == 0)
          {
            state.changed.wait(lock, [&] { return state.second_started || stop_token.stop_requested(); });
          }
          else if (slot.provider_index == 1)
          {
            state.second_started = true;
            state.changed.notify_all();
            state.changed.wait(lock, [&] { return state.queued_started || stop_token.stop_requested(); });
          }
          else
          {
            state.queued_started = true;
            state.changed.notify_all();
          }
          if (stop_token.stop_requested())
            return canceled_executor_result(slot);
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 2, .stop_token = cancellation.get_token()});
    {
      std::lock_guard lock(state.mutex);
      state.finished = true;
    }
    state.changed.notify_all();
  });

  bool finished = false;
  {
    std::unique_lock lock(state.mutex);
    finished = state.changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return state.finished; });
  }
  if (!finished)
    static_cast<void>(cancellation.request_stop());
  runner.join();

  expect(finished, "parallel scheduler refills capacity after retiring a completed worker");
  if (!finished)
    return;

  expect(state.queued_started, "queued worker starts while the remaining capped worker is still active");
  expect(scheduled && scheduled->has_value() && (*scheduled)->size() == schedule.size(),
         scheduled && !scheduled->has_value() ? scheduled->error().format() : "capacity-refill schedule returns every outcome");
}

void test_run_parallel_tool_schedule_signals_stop_to_later_workers_on_error()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"),
      parallel_read_slot(1, "call_1"),
      parallel_read_slot(2, "call_2"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t started = 0;
    bool fail_first = false;
    std::vector<std::size_t> stopped_workers;
  } state;

  std::stop_source cancellation;
  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          std::unique_lock lock(state.mutex);
          ++state.started;
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          if (slot.provider_index == 0)
          {
            state.changed.wait(lock, [&] { return state.fail_first || stop_token.stop_requested(); });
            if (stop_token.stop_requested())
              return canceled_executor_result(slot);
            return executor_error(slot, "slot zero failed");
          }
          state.changed.wait(lock, [&] { return stop_token.stop_requested(); });
          state.stopped_workers.push_back(slot.provider_index);
          state.changed.notify_all();
          return canceled_executor_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 3, .stop_token = cancellation.get_token()});
  });

  bool later_workers_stopped = false;
  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.started == 3; }, "stop-signaling test starts all workers");
    state.fail_first = true;
    state.changed.notify_all();
    later_workers_stopped = state.changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return state.stopped_workers.size() == 2; });
  }
  if (!later_workers_stopped)
    static_cast<void>(cancellation.request_stop());
  runner.join();

  expect(later_workers_stopped, "scheduler stop signal reaches later active workers without losing the failure transition");
  if (!later_workers_stopped)
    return;

  expect(scheduled && !scheduled->has_value() && scheduled->error().message() == "slot zero failed",
         "parallel scheduler returns the first provider-order executor error after stopping later workers");
  auto stopped_workers = state.stopped_workers;
  std::ranges::sort(stopped_workers);
  expect(stopped_workers == std::vector<std::size_t>{1, 2}, "later workers observe stop in the stop-signaling test");
}

void test_run_parallel_tool_schedule_returns_first_error_in_provider_order()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"),
      parallel_read_slot(1, "call_1"),
      parallel_read_slot(2, "call_2"),
      barrier_slot(3, "call_3"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<bool> released = std::vector<bool>(3, false);
    std::size_t started = 0;
    std::vector<std::size_t> completion_order;
    bool barrier_started = false;
  } state;

  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          if (slot.provider_index == 3)
          {
            std::lock_guard lock(state.mutex);
            state.barrier_started = true;
            state.changed.notify_all();
            return successful_result(slot);
          }

          auto const index = slot.provider_index;
          std::unique_lock lock(state.mutex);
          ++state.started;
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          state.changed.wait(lock, [&] { return state.released[index] || stop_token.stop_requested(); });
          if (stop_token.stop_requested())
            return canceled_executor_result(slot);
          state.completion_order.push_back(index);
          state.changed.notify_all();
          if (index == 2)
            return executor_error(slot, "third failed first by completion");
          if (index == 1)
            return executor_error(slot, "second failed in provider order");
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 3});
  });

  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.started == 3; }, "first-error test starts all read/search workers");
    state.released[2] = true;
    state.changed.notify_all();
    wait_for_test(
        state.changed, lock, [&] { return state.completion_order == std::vector<std::size_t>{2}; }, "first-error test completes provider slot 2 before slot 1");
    state.released[1] = true;
    state.changed.notify_all();
    wait_for_test(
        state.changed, lock, [&] { return state.completion_order == std::vector<std::size_t>{2, 1}; },
        "first-error test completes provider slot 1 after slot 2");
    state.released[0] = true;
    state.changed.notify_all();
  }
  runner.join();

  expect(scheduled && !scheduled->has_value() && scheduled->error().message() == "second failed in provider order",
         "parallel scheduler returns the first executor error in provider order, not completion order");
  expect(!state.barrier_started, "parallel scheduler does not start later barriers after an epoch error");
}

void test_run_parallel_tool_schedule_cancels_active_epoch_from_external_stop_token()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"),
      parallel_read_slot(1, "call_1"),
      barrier_slot(2, "call_2"),
      parallel_read_slot(3, "call_3"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t started = 0;
    std::vector<std::size_t> stopped_workers;
    std::vector<std::size_t> later_launches;
    bool force_release = false;
  } state;

  std::stop_source source;
  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          if (slot.provider_index >= 2)
          {
            std::lock_guard lock(state.mutex);
            state.later_launches.push_back(slot.provider_index);
            state.changed.notify_all();
            return successful_result(slot);
          }

          std::unique_lock lock(state.mutex);
          ++state.started;
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          state.changed.wait(lock, [&] { return state.force_release || stop_token.stop_requested(); });
          if (stop_token.stop_requested())
            state.stopped_workers.push_back(slot.provider_index);
          state.changed.notify_all();
          return canceled_executor_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 2, .stop_token = source.get_token()});
  });

  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.started == 2; }, "external stop test starts the active parallel epoch workers");
    expect(state.later_launches.empty(), "external stop test does not launch later barrier/read slots before cancellation");
    expect(source.request_stop(), "external stop test requests cancellation through the supplied stop token");
    wait_for_test(state.changed, lock, [&] { return state.stopped_workers.size() == 2; }, "external stop reaches active worker jthread tokens");
    state.force_release = true;
    state.changed.notify_all();
  }
  runner.join();

  expect(scheduled && !scheduled->has_value() && scheduled->error().message() == "tool schedule canceled" &&
             has_error_context(scheduled->error(), "phase", "parallel_epoch"),
         "parallel scheduler returns a parallel_epoch canceled error for active external stop");
  auto stopped_workers = state.stopped_workers;
  std::ranges::sort(stopped_workers);
  expect(stopped_workers == std::vector<std::size_t>{0, 1}, "active workers observe jthread stop tokens after external cancellation");
  expect(state.later_launches.empty(), "parallel scheduler does not launch later barriers or slots after active external cancellation");
}

void test_run_parallel_tool_schedule_returns_before_slot_when_stop_token_is_pre_stopped()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"),
      barrier_slot(1, "call_1"),
  };
  std::stop_source source;
  expect(source.request_stop(), "pre-stopped token test requests stop before scheduling begins");
  std::vector<std::size_t> launched;

  auto outcomes = ava::agent::run_parallel_tool_schedule(
      schedule,
      [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
        launched.push_back(slot.provider_index);
        return successful_result(slot);
      },
      ava::agent::ToolParallelScheduleOptions{.max_workers = 2, .stop_token = source.get_token()});

  expect(!outcomes && outcomes.error().message() == "tool schedule canceled" && has_error_context(outcomes.error(), "phase", "before_slot"),
         "parallel scheduler returns before_slot cancellation for a pre-stopped external token");
  expect(launched.empty(), "parallel scheduler does not launch any slot when the external token is already stopped");
}

void test_run_parallel_tool_schedule_converts_executor_exception()
{
  std::vector schedule{
      parallel_read_slot(0, "call_0"),
      parallel_read_slot(1, "call_1"),
      barrier_slot(2, "call_2"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t started = 0;
    std::vector<std::size_t> stopped_workers;
    bool barrier_started = false;
    bool force_release = false;
  } state;

  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          if (slot.provider_index == 2)
          {
            std::lock_guard lock(state.mutex);
            state.barrier_started = true;
            state.changed.notify_all();
            return successful_result(slot);
          }

          std::unique_lock lock(state.mutex);
          ++state.started;
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          if (slot.provider_index == 0)
          {
            state.changed.wait(lock, [&] { return state.started == 2 || stop_token.stop_requested(); });
            throw std::runtime_error("boom from fake executor");
          }

          state.changed.wait(lock, [&] { return state.force_release || stop_token.stop_requested(); });
          if (stop_token.stop_requested())
            state.stopped_workers.push_back(slot.provider_index);
          state.changed.notify_all();
          return canceled_executor_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 2});
  });

  {
    std::unique_lock lock(state.mutex);
    wait_for_test(state.changed, lock, [&] { return state.started == 2; }, "exception conversion test starts both active workers");
    wait_for_test(
        state.changed, lock, [&] { return state.stopped_workers == std::vector<std::size_t>{1}; }, "exception conversion test stops the later active worker");
    state.force_release = true;
    state.changed.notify_all();
  }
  runner.join();

  expect(scheduled && !scheduled->has_value() && scheduled->error().category() == ava::core::ErrorCategory::Tool &&
             scheduled->error().message() == "tool schedule executor threw" && has_error_context(scheduled->error(), "provider_index", "0") &&
             has_error_context(scheduled->error(), "cause", "boom from fake executor"),
         "parallel scheduler converts fake executor exceptions into tool schedule executor errors with provider_index context");
  expect(!state.barrier_started, "parallel scheduler does not launch a later barrier after an executor exception");
}

void test_run_parallel_tool_schedule_rejects_null_executor()
{
  std::vector schedule{parallel_read_slot(0, "call_0")};
  ava::agent::ToolParallelScheduleExecutor executor;

  auto outcomes = ava::agent::run_parallel_tool_schedule(schedule, executor);

  expect(!outcomes && outcomes.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             outcomes.error().message() == "tool schedule executor is required",
         "parallel scheduler rejects a null executor");
}

void test_run_parallel_tool_schedule_rejects_zero_max_workers()
{
  std::vector schedule{parallel_read_slot(0, "call_0")};
  bool launched = false;

  auto outcomes = ava::agent::run_parallel_tool_schedule(
      schedule,
      [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
        launched = true;
        return successful_result(slot);
      },
      ava::agent::ToolParallelScheduleOptions{.max_workers = 0});

  expect(!outcomes && outcomes.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             outcomes.error().message() == "tool schedule max_workers must be greater than zero",
         "parallel scheduler rejects max_workers equal to zero");
  expect(!launched, "parallel scheduler does not launch a slot when max_workers is invalid");
}

void test_run_parallel_tool_schedule_keeps_non_eligible_slots_sequential()
{
  std::vector schedule{
      non_ready_read_slot(0, "call_0"),
      barrier_slot(1, "call_1"),
      deferred_slot(2, "call_2"),
      non_ready_read_slot(3, "call_3", "grep"),
  };

  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<bool> released = std::vector<bool>(4, false);
    std::vector<std::size_t> start_order;
    std::size_t active = 0;
    std::size_t max_active = 0;
  } state;

  std::optional<ava::core::Result<std::vector<ava::agent::ToolScheduleOutcome>>> scheduled;
  ava::core::JoinThread runner = ava::core::JoinThread::create("runner", [&] {
    scheduled = ava::agent::run_parallel_tool_schedule(
        schedule,
        [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
          auto const index = slot.provider_index;
          std::unique_lock lock(state.mutex);
          state.start_order.push_back(index);
          ++state.active;
          state.max_active = std::max(state.max_active, state.active);
          state.changed.notify_all();
          std::stop_callback notify_stop(stop_token, [&] { state.changed.notify_all(); });
          state.changed.wait(lock, [&] { return state.released[index] || stop_token.stop_requested(); });
          --state.active;
          state.changed.notify_all();
          if (stop_token.stop_requested())
            return canceled_executor_result(slot);
          return successful_result(slot);
        },
        ava::agent::ToolParallelScheduleOptions{.max_workers = 4});
  });

  {
    std::unique_lock lock(state.mutex);
    for (std::size_t index = 0; index < schedule.size(); ++index)
    {
      wait_for_test(
          state.changed, lock, [&] { return state.start_order.size() == index + 1; }, "non-eligible parallel scheduler test starts the next sequential slot");
      expect(state.active == 1 && !state.start_order.empty() && state.start_order.back() == index,
             "non-ready read candidates, barriers, and deferred slots run one-at-a-time in provider order");
      state.released[index] = true;
      state.changed.notify_all();
    }
  }
  runner.join();

  expect(scheduled && scheduled->has_value(), scheduled && !*scheduled ? scheduled->error().format() : "parallel scheduler succeeds for sequential-only slots");
  expect(state.max_active == 1 && state.start_order == std::vector<std::size_t>{0, 1, 2, 3},
         "parallel scheduler treats all non-eligible classifications and unpreflighted candidates as sequential barriers");
}

void test_run_parallel_tool_schedule_dispatches_real_read_search_tools()
{
  auto const root = create_empty_root("tool_scheduler_real_parallel");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace / "src");
  ava::tests::write_test_file(workspace / "note.txt", "alpha scheduler\nsecond line\n");
  ava::tests::write_test_file(workspace / "src" / "code.cpp", "int scheduler_symbol = 1;\n");

  std::vector calls{
      call("call_read", "read_file"),
      call("call_list", "list_directory"),
      call("call_glob", "glob"),
      call("call_grep", "grep"),
  };
  calls[0].arguments_json = "{\"path\":\"note.txt\"}";
  calls[1].arguments_json = "{\"path\":\".\",\"max_entries\":20}";
  calls[2].arguments_json = "{\"pattern\":\"**/*.cpp\",\"max_results\":20}";
  calls[3].arguments_json = "{\"pattern\":\"scheduler_symbol\",\"include\":\"**/*.cpp\"}";

  auto schedule = ava::agent::build_sequential_tool_schedule(calls, ava::agent::builtin_tool_metadata());
  for (auto& slot : schedule) slot.parallel_readiness = ToolScheduleParallelReadiness::PreflightProvenNonInteractive;

  auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::core::Mode::Build});
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto const& dispatcher = *dispatcher_result;
  auto outcomes = ava::agent::run_parallel_tool_schedule(
      schedule,
      [&](ava::agent::ToolScheduleSlot const& slot, std::stop_token stop_token) -> ava::core::Result<ava::agent::ToolDispatchResult> {
        if (stop_token.stop_requested())
          return canceled_executor_result(slot);
        return dispatcher.dispatch(slot.call);
      },
      ava::agent::ToolParallelScheduleOptions{.max_workers = 4});

  expect(outcomes.has_value(), outcomes ? "real read/search parallel dispatch succeeds" : outcomes.error().format());
  expect(outcomes && outcomes->size() == 4 && (*outcomes)[0].slot.call.name == "read_file" && (*outcomes)[1].slot.call.name == "list_directory" &&
             (*outcomes)[2].slot.call.name == "glob" && (*outcomes)[3].slot.call.name == "grep",
         "real read/search parallel dispatch keeps provider-order outcomes");
  if (!ava::tests::require(outcomes && outcomes->size() == 4, "real read/search dispatch returns all four outcomes"))
    return;
  expect(outcomes && (*outcomes)[0].result.success && (*outcomes)[0].result.result_text.find("alpha scheduler") != std::string::npos,
         "parallel scheduler can dispatch real read_file");
  expect(outcomes && (*outcomes)[1].result.success && (*outcomes)[1].result.result_text.find("note.txt") != std::string::npos,
         "parallel scheduler can dispatch real list_directory");
  expect(outcomes && (*outcomes)[2].result.success && (*outcomes)[2].result.result_text.find("src/code.cpp") != std::string::npos,
         "parallel scheduler can dispatch real glob");
  expect(outcomes && (*outcomes)[3].result.success && (*outcomes)[3].result.result_text.find("scheduler_symbol") != std::string::npos,
         "parallel scheduler can dispatch real grep");
}

}  // namespace

void run_tool_scheduler_tests()
{
  ava::tests::HermeticTestEnvironment environment("tool-scheduler-env-");
  test_classify_builtin_tools_for_scheduling();
  test_classify_brokered_external_metadata_for_scheduling();
  test_build_sequential_tool_schedule_preserves_provider_order();
  test_run_sequential_tool_schedule_executes_in_order();
  test_run_sequential_tool_schedule_short_circuits_on_executor_error();
  test_run_sequential_tool_schedule_rejects_null_executor();
  test_run_sequential_tool_schedule_converts_executor_exception();
  test_run_parallel_tool_schedule_splits_epochs_at_barriers();
  test_run_parallel_tool_schedule_returns_provider_order_after_reverse_completion();
  test_run_parallel_tool_schedule_respects_worker_cap();
  test_run_parallel_tool_schedule_replaces_immediately_completed_capped_workers();
  test_run_parallel_tool_schedule_refills_capacity_after_retiring_a_completed_worker();
  test_run_parallel_tool_schedule_signals_stop_to_later_workers_on_error();
  test_run_parallel_tool_schedule_returns_first_error_in_provider_order();
  test_run_parallel_tool_schedule_cancels_active_epoch_from_external_stop_token();
  test_run_parallel_tool_schedule_returns_before_slot_when_stop_token_is_pre_stopped();
  test_run_parallel_tool_schedule_converts_executor_exception();
  test_run_parallel_tool_schedule_rejects_null_executor();
  test_run_parallel_tool_schedule_rejects_zero_max_workers();
  test_run_parallel_tool_schedule_keeps_non_eligible_slots_sequential();
  test_run_parallel_tool_schedule_dispatches_real_read_search_tools();
}
