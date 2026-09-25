#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/agent/job_control.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/agent/subagent_inspector.h"
#include "ava/agent/subagent_inspector_source.h"
#include "ava/session/session_store.h"
#include "ava/session/subagent_job_history.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>

namespace {

struct BlockingWorker
{
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false;
  bool release = false;
  bool canceled = false;

  ava::agent::BackgroundJobCompletion run(ava::agent::BackgroundJobContext const& context, std::string text = "done")
  {
    std::stop_callback wake_on_stop(context.stop_token, [&] { changed.notify_all(); });
    std::unique_lock lock(mutex);
    started = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release || context.stop_token.stop_requested(); });
    if (context.stop_token.stop_requested())
    {
      canceled = true;
      changed.notify_all();
      return {.state = ava::agent::BackgroundJobState::Canceled, .final_text = {}, .stop_reason = "canceled"};
    }
    return {.state = ava::agent::BackgroundJobState::Completed, .final_text = std::move(text), .stop_reason = "completed"};
  }

  bool wait_started()
  {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2), [&] { return started; });
  }

  void finish()
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
};

std::shared_ptr<ava::agent::SubagentCoordinator> coordinator_with(ava::agent::SubagentCoordinatorOptions options = {})
{
  auto created = ava::agent::SubagentCoordinator::create(std::move(options));
  expect(created.has_value(), created ? "subagent coordinator creates" : "subagent coordinator creates: " + created.error().format());
  return created ? *created : nullptr;
}

ava::agent::SubagentCoordinatorJobSnapshot complete_background(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator, std::string const& parent,
                                                               std::string child, std::string summary)
{
  auto started = coordinator->start_background(parent, {.child_session_id = std::move(child)}, [summary = std::move(summary)](auto const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = summary, .stop_reason = "completed"};
  });
  expect(started.has_value(), "background fixture starts");
  if (!started)
    return {};
  auto terminal = coordinator->wait(parent, started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out, "background fixture reaches terminal state");
  return terminal.value_or(ava::agent::SubagentCoordinatorJobSnapshot{});
}

void acknowledge(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator, std::string const& parent,
                 ava::agent::SubagentCoordinatorJobSnapshot const& job, std::string suffix)
{
  auto attempt_id = "attempt_" + suffix;
  auto attempted = coordinator->record_delivery_attempt(parent, job.job.identity.job_id, attempt_id, "fingerprint_" + suffix);
  auto acknowledged = attempted ? coordinator->acknowledge_delivery(parent, job.job.identity.job_id, attempt_id, "turn_" + suffix)
                                : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(attempted.error()));
  expect(attempted && acknowledged && acknowledged->job.delivery == ava::agent::SubagentDeliveryState::Acknowledged,
         "delivery fixture acknowledges latest attempt");
}

void test_hidden_immediate_completion_publishes_once_after_visibility()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.wait_for_terminal_before_start_returns = true;
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t notifications = 0;
  bool visible_from_sink = false;
  coordinator->set_terminal_sink([&](ava::agent::SubagentCoordinatorJobSnapshot const& notification) {
    auto listed = coordinator->list("parent_immediate");
    auto visible = coordinator->snapshot("parent_immediate", notification.job.identity.job_id);
    std::lock_guard lock(mutex);
    ++notifications;
    visible_from_sink = listed.size() == 1 && listed.front().job.identity.job_id == notification.job.identity.job_id && visible.has_value();
    changed.notify_all();
  });
  auto started = coordinator->start_background("parent_immediate", {.child_session_id = "child_immediate"}, [](auto const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "immediate", .stop_reason = "completed"};
  });
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return notifications == 1; }) && visible_from_sink,
           "immediate completion stays hidden until list and snapshot can observe it, then emits once");
  }
  expect(started && started->job.execution == ava::agent::SubagentExecutionState::Completed &&
             coordinator->snapshot("parent_immediate", started->job.identity.job_id).has_value() && notifications == 1,
         "forced immediate completion occurs while hidden, then start returns the latest process-local snapshot without duplicate notification");
}

void test_process_locality_after_prior_coordinator_shutdown()
{
  auto first = coordinator_with();
  if (!first)
    return;
  auto completed = complete_background(first, "parent_process_a", "child_process_a", "process-local result");
  expect(!completed.job.identity.job_id.empty() && first->result("parent_process_a", completed.job.identity.job_id).has_value(),
         "first coordinator retains its completed process-local result until shutdown");
  first->shutdown();
  first.reset();

  auto second = coordinator_with();
  if (!second)
    return;
  auto listed = second->list("parent_process_a");
  auto recovered = second->result("parent_process_a", completed.job.identity.job_id);
  expect(listed.empty() && !recovered && recovered.error().category() == ava::core::ErrorCategory::NotFound,
         "a coordinator constructed after prior coordinator shutdown cannot recover its completed job");
}

void test_owner_filter_wait_result_cancel_and_wire_contract()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background("parent_owner", {.title = "title", .child_session_id = "child_owner"},
                                               [worker](auto const& context) { return worker->run(context, "terminal summary"); });
  expect(started && worker->wait_started(), "owner fixture starts one worker");
  if (!started)
    return;
  auto timed = coordinator->wait("parent_owner", started->job.identity.job_id, std::chrono::milliseconds(1));
  auto hidden = coordinator->snapshot("other_parent", started->job.identity.job_id);
  auto hidden_cancel = coordinator->cancel("other_parent", started->job.identity.job_id);
  expect(timed && timed->timed_out && !hidden && !hidden_cancel && hidden.error().category() == ava::core::ErrorCategory::NotFound &&
             hidden_cancel.error().category() == ava::core::ErrorCategory::NotFound && !coordinator->result("parent_owner", started->job.identity.job_id),
         "wait timeout and parent ownership remain process-local and non-enumerating");
  worker->finish();
  auto completed = coordinator->wait("parent_owner", started->job.identity.job_id, std::chrono::seconds(2));
  auto result = completed ? coordinator->result("parent_owner", started->job.identity.job_id)
                          : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(completed.error()));
  auto status_json = completed ? ava::agent::public_job_snapshot_json(*completed) : std::string{};
  auto result_json = result ? ava::agent::public_job_snapshot_json(*result, ava::agent::PublicJobContent::IncludeTerminalResult) : std::string{};
  expect(completed && result && completed->job.execution == ava::agent::SubagentExecutionState::Completed &&
             completed->job.delivery == ava::agent::SubagentDeliveryState::Pending && completed->job.summary == "terminal summary" &&
             status_json.find("terminal summary") == std::string::npos && result_json.find("terminal summary") != std::string::npos &&
             result_json.find("\"schema_version\":1") != std::string::npos && result_json.find("coordinator_error") == std::string::npos,
         "schema-version-1 status/result wire fields and terminal-content policy remain unchanged");
}

void test_launch_display_survives_coordinator_lifecycle_and_owner_checks()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  auto const display = ava::agent::SubagentLaunchDisplay::normalized("COORDINATOR_MODEL_SENTINEL", std::string_view("COORD_REASONING_SENTINEL"));
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start(ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "launch_owner",
                                                                                .mode = ava::agent::SubagentJobMode::Foreground,
                                                                                .job = {.title = "launch", .child_session_id = "launch_child"},
                                                                                .launch_display = display},
                                    [worker](auto const& context) { return worker->run(context, "launch terminal"); });
  expect(started && worker->wait_started() && started->job.launch_display == display, "coordinator stores launch display before foreground publication");
  if (!started)
    return;
  auto const job_id = started->job.identity.job_id;
  auto listed = coordinator->list("launch_owner");
  auto status = coordinator->snapshot("launch_owner", job_id);
  auto timed = coordinator->wait("launch_owner", job_id, std::chrono::milliseconds(1));
  auto hidden = coordinator->snapshot("wrong_owner", job_id);
  auto promoted = coordinator->promote("launch_owner", job_id);
  expect(listed.size() == 1 && listed.front().job.launch_display == display && status && status->job.launch_display == display && timed && timed->timed_out &&
             timed->job.launch_display == display && !hidden && hidden.error().category() == ava::core::ErrorCategory::NotFound && promoted &&
             promoted->job.launch_display == display,
         "list/status/wait/promotion preserve immutable launch display while wrong-owner authority remains non-enumerating");
  worker->finish();
  auto terminal = coordinator->wait("launch_owner", job_id, std::chrono::seconds(2));
  auto retained = coordinator->result("launch_owner", job_id);
  auto const public_status = terminal ? ava::agent::public_job_snapshot_json(*terminal) : std::string{};
  auto const public_result = retained ? ava::agent::public_job_snapshot_json(*retained, ava::agent::PublicJobContent::IncludeTerminalResult) : std::string{};
  auto const public_list = ava::agent::public_job_list_json(coordinator->list("launch_owner"));
  expect(terminal && retained && terminal->job.was_promoted && terminal->job.launch_display == display && retained->job.launch_display == display &&
             public_status.find("COORDINATOR_MODEL_SENTINEL") == std::string::npos && public_result.find("COORDINATOR_MODEL_SENTINEL") == std::string::npos &&
             public_list.find("COORDINATOR_MODEL_SENTINEL") == std::string::npos && public_result.find("COORD_REASONING_SENTINEL") == std::string::npos,
         "promotion, terminal completion, retained result, and public status/list/result keep private launch display in coordinator custody only");

  auto canceled_worker = std::make_shared<BlockingWorker>();
  auto cancel_start = coordinator->start(ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "cancel_owner",
                                                                                     .mode = ava::agent::SubagentJobMode::Background,
                                                                                     .job = {.child_session_id = "cancel_child"},
                                                                                     .launch_display = display},
                                         [canceled_worker](auto const& context) { return canceled_worker->run(context); });
  if (cancel_start && canceled_worker->wait_started())
  {
    auto canceled = coordinator->cancel("cancel_owner", cancel_start->job.identity.job_id);
    auto canceled_terminal = coordinator->wait("cancel_owner", cancel_start->job.identity.job_id, std::chrono::seconds(2));
    expect(canceled && canceled->job.launch_display == display && canceled_terminal && canceled_terminal->job.launch_display == display,
           "cancel request and canceled terminal snapshot preserve launch display");
  }

  auto rejected = coordinator->start(ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "../invalid",
                                                                                 .mode = ava::agent::SubagentJobMode::Background,
                                                                                 .job = {.child_session_id = "rejected_child"},
                                                                                 .launch_display = display},
                                     [](auto const&) { return ava::agent::BackgroundJobCompletion{}; });
  expect(!rejected && coordinator->list("../invalid").empty() &&
             ava::agent::subagent_publication_commit_state(rejected.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "coordinator start failure publishes neither job state nor private launch metadata");
}

void test_exact_v1_job_snapshot_and_enum_strings()
{
  ava::agent::SubagentCoordinatorJobSnapshot snapshot{
      .job = {.schema_version = 1,
              .identity = {.job_id = "job_fixed",
                           .task_id = "task_fixed",
                           .parent_session_id = "parent_fixed",
                           .child_session_id = "child_fixed",
                           .delivery_id = "delivery_fixed"},
              .mode = ava::agent::SubagentJobMode::Background,
              .execution = ava::agent::SubagentExecutionState::Completed,
              .delivery = ava::agent::SubagentDeliveryState::Acknowledged,
              .was_promoted = true,
              .cancel_requested = true,
              .delivery_attempts = 7,
              .started_at = "started",
              .updated_at = "updated",
              .promoted_at = std::nullopt,
              .cancel_requested_at = "cancel-requested",
              .terminal_at = std::nullopt,
              .delivery_pending_at = "delivery-pending",
              .last_delivery_attempt_at = std::nullopt,
              .delivery_acknowledged_at = "delivery-acknowledged",
              .summary = "fixed summary",
              .summary_truncated = true,
              .error_truncated = false,
              .stop_reason_truncated = true,
              .provider_iterations = 11,
              .tool_calls = 12,
              .tool_iterations = 13,
              .display_title = "internal display only",
              .display_subagent_type = "explore",
              .launch_display = ava::agent::SubagentLaunchDisplay::normalized("MODEL_DISPLAY_SENTINEL", std::string_view("REASONING_SENTINEL"))},
      .timed_out = true};
  auto const expected =
      R"({"schema_version":1,"job_id":"job_fixed","task_id":"task_fixed","parent_session_id":"parent_fixed","child_session_id":"child_fixed","delivery_id":"delivery_fixed","mode":"background","state":"completed","delivery_state":"acknowledged","was_promoted":true,"cancel_requested":true,"timed_out":true,"started_at":"started","updated_at":"updated","promoted_at":null,"cancel_requested_at":"cancel-requested","terminal_at":null,"delivery_pending_at":"delivery-pending","last_delivery_attempt_at":null,"delivery_acknowledged_at":"delivery-acknowledged","delivery_attempts":7,"summary_truncated":true,"error_truncated":false,"stop_reason_truncated":true,"provider_iterations":11,"tool_calls":12,"tool_iterations":13,"result":{"status":"completed","summary":"fixed summary"}})";
  auto const encoded = ava::agent::public_job_snapshot_json(snapshot, ava::agent::PublicJobContent::IncludeTerminalResult);
  auto const listed = ava::agent::public_job_list_json({snapshot});
  expect(encoded == expected && encoded.find("display_title") == std::string::npos && encoded.find("internal display only") == std::string::npos &&
             encoded.find("\"title\"") == std::string::npos && encoded.find("explore") == std::string::npos &&
             encoded.find("MODEL_DISPLAY_SENTINEL") == std::string::npos && encoded.find("REASONING_SENTINEL") == std::string::npos &&
             listed.find("MODEL_DISPLAY_SENTINEL") == std::string::npos && listed.find("REASONING_SENTINEL") == std::string::npos,
         "exact schema-version-1 public snapshot JSON retains every field, nullable value, counter, truncation flag, and terminal result shape while omitting "
         "internal display fields");

  std::array const modes{ava::agent::SubagentJobMode::Foreground, ava::agent::SubagentJobMode::Background};
  std::array const mode_strings{std::string_view("foreground"), std::string_view("background")};
  std::array const executions{ava::agent::SubagentExecutionState::Starting,  ava::agent::SubagentExecutionState::Running,
                              ava::agent::SubagentExecutionState::Completed, ava::agent::SubagentExecutionState::Failed,
                              ava::agent::SubagentExecutionState::Canceled,  ava::agent::SubagentExecutionState::Interrupted};
  std::array const execution_strings{std::string_view("starting"), std::string_view("running"),  std::string_view("completed"),
                                     std::string_view("failed"),   std::string_view("canceled"), std::string_view("interrupted")};
  std::array const deliveries{ava::agent::SubagentDeliveryState::Direct, ava::agent::SubagentDeliveryState::Pending,
                              ava::agent::SubagentDeliveryState::Attempting, ava::agent::SubagentDeliveryState::Acknowledged};
  std::array const delivery_strings{std::string_view("direct"), std::string_view("pending"), std::string_view("attempting"), std::string_view("acknowledged")};
  bool exact = true;
  for (std::size_t index = 0; index < modes.size(); ++index)
    exact = exact && ava::agent::to_string(modes[index]) == mode_strings[index];
  for (std::size_t index = 0; index < executions.size(); ++index)
    exact = exact && ava::agent::to_string(executions[index]) == execution_strings[index];
  for (std::size_t index = 0; index < deliveries.size(); ++index)
    exact = exact && ava::agent::to_string(deliveries[index]) == delivery_strings[index];
  expect(exact, "exact public mode, retained execution, and delivery enum strings remain schema-version-1 compatible");
}

void test_foreground_promotion_cancel_and_interaction_gate()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;

  auto direct = complete_background(coordinator, "unused", "unused", "unused");
  if (!direct.job.identity.job_id.empty())
    acknowledge(coordinator, "unused", direct, "unused");

  auto foreground_worker = std::make_shared<BlockingWorker>();
  auto gate = ava::agent::SubagentInteractionGate::create(ava::agent::SubagentJobMode::Foreground, nullptr, nullptr);
  auto foreground = coordinator->start(
      "parent_foreground", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_foreground"},
      [foreground_worker](auto const& context) { return foreground_worker->run(context, "promoted summary"); }, gate);
  expect(foreground && foreground_worker->wait_started(), "foreground promotion fixture starts");
  if (foreground)
  {
    auto promoted = coordinator->promote("parent_foreground", foreground->job.identity.job_id);
    auto repeated = coordinator->promote("parent_foreground", foreground->job.identity.job_id);
    expect(promoted && repeated && promoted->job.was_promoted && repeated->job.was_promoted, "promotion is idempotent and preserves one job identity");
    foreground_worker->finish();
    auto terminal = coordinator->wait("parent_foreground", foreground->job.identity.job_id, std::chrono::seconds(2));
    expect(terminal && terminal->job.delivery == ava::agent::SubagentDeliveryState::Pending && terminal->job.summary == "promoted summary",
           "promotion winner converts the same worker's terminal result to pending delivery");
  }

  auto direct_worker = std::make_shared<BlockingWorker>();
  auto direct_start = coordinator->start("parent_direct", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_direct"},
                                         [direct_worker](auto const& context) { return direct_worker->run(context, "direct summary"); });
  expect(direct_start && direct_worker->wait_started(), "direct foreground fixture starts");
  if (direct_start)
  {
    direct_worker->finish();
    auto terminal = coordinator->wait("parent_direct", direct_start->job.identity.job_id, std::chrono::seconds(2));
    auto late = coordinator->promote("parent_direct", direct_start->job.identity.job_id);
    expect(terminal && late && terminal->job.delivery == ava::agent::SubagentDeliveryState::Direct && !late->job.was_promoted,
           "completion winner remains direct and causes late promotion to abort");
  }

  auto canceled_worker = std::make_shared<BlockingWorker>();
  auto canceled = coordinator->start("parent_cancel", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_cancel"},
                                     [canceled_worker](auto const& context) { return canceled_worker->run(context); });
  expect(canceled && canceled_worker->wait_started(), "cancel fixture starts");
  if (canceled)
  {
    auto requested = coordinator->cancel("parent_cancel", canceled->job.identity.job_id);
    auto promotion = coordinator->promote("parent_cancel", canceled->job.identity.job_id);
    auto terminal = coordinator->wait("parent_cancel", canceled->job.identity.job_id, std::chrono::seconds(2));
    expect(requested && terminal && terminal->job.execution == ava::agent::SubagentExecutionState::Canceled && (!promotion || !promotion->job.was_promoted),
           "cancel latches before the registry call and prevents a later promotion winner");
  }
}

void test_concurrent_state_transition_matrices()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;

  auto promotion_worker = std::make_shared<BlockingWorker>();
  auto promotion_start = coordinator->start("parent_promotion_race", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_promotion_race"},
                                            [promotion_worker](auto const& context) { return promotion_worker->run(context, "promotion race"); });
  expect(promotion_start && promotion_worker->wait_started(), "promotion/completion race fixture starts");
  if (promotion_start)
  {
    auto const job_id = promotion_start->job.identity.job_id;
    std::barrier start_line(3);
    auto promotion = std::async(std::launch::async, [&] {
      start_line.arrive_and_wait();
      return coordinator->promote("parent_promotion_race", job_id);
    });
    auto completion = std::async(std::launch::async, [&] {
      start_line.arrive_and_wait();
      promotion_worker->finish();
    });
    start_line.arrive_and_wait();
    auto const promotion_ready = promotion.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto const completion_ready = completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto promoted = promotion_ready ? promotion.get()
                                    : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(
                                          std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "promotion race timed out")));
    if (completion_ready)
      completion.get();
    auto terminal = coordinator->wait("parent_promotion_race", job_id, std::chrono::seconds(2));
    bool const promotion_won = terminal && terminal->job.was_promoted;
    bool const legal = terminal && terminal->job.identity.job_id == job_id &&
                       ((promotion_won && terminal->job.mode == ava::agent::SubagentJobMode::Background &&
                         terminal->job.delivery == ava::agent::SubagentDeliveryState::Pending && promoted && promoted->job.was_promoted) ||
                        (!promotion_won && terminal->job.mode == ava::agent::SubagentJobMode::Foreground &&
                         terminal->job.delivery == ava::agent::SubagentDeliveryState::Direct && promoted && !promoted->job.was_promoted));
    expect(promotion_ready && completion_ready && legal,
           "concurrent promotion and completion choose exactly one legal stable-identity linearization without deadlock");
  }

  auto cancellation_worker = std::make_shared<BlockingWorker>();
  auto cancellation_start = coordinator->start("parent_cancel_race", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_cancel_race"},
                                               [cancellation_worker](auto const& context) { return cancellation_worker->run(context, "cancel race"); });
  expect(cancellation_start && cancellation_worker->wait_started(), "cancellation/completion race fixture starts");
  if (cancellation_start)
  {
    auto const job_id = cancellation_start->job.identity.job_id;
    std::barrier start_line(3);
    auto cancellation = std::async(std::launch::async, [&] {
      start_line.arrive_and_wait();
      return coordinator->cancel("parent_cancel_race", job_id);
    });
    auto completion = std::async(std::launch::async, [&] {
      start_line.arrive_and_wait();
      cancellation_worker->finish();
    });
    start_line.arrive_and_wait();
    auto const cancellation_ready = cancellation.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto const completion_ready = completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto canceled = cancellation_ready ? cancellation.get()
                                       : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(
                                             std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "cancellation race timed out")));
    if (completion_ready)
      completion.get();
    auto terminal = coordinator->wait("parent_cancel_race", job_id, std::chrono::seconds(2));
    bool const legal_terminal = terminal && (terminal->job.execution == ava::agent::SubagentExecutionState::Completed ||
                                             terminal->job.execution == ava::agent::SubagentExecutionState::Canceled);
    expect(cancellation_ready && completion_ready && canceled && legal_terminal && terminal->job.identity.job_id == job_id,
           "concurrent cancellation and completion settle to a legal terminal state with stable identity and no deadlock");
  }

  auto delivery = complete_background(coordinator, "parent_delivery_race", "child_delivery_race", "delivery race");
  auto attempted = coordinator->record_delivery_attempt("parent_delivery_race", delivery.job.identity.job_id, "attempt_race", "fingerprint_race");
  expect(attempted.has_value(), "acknowledgement/exhaustion race fixture records the latest attempt");
  if (attempted)
  {
    std::barrier start_line(3);
    auto acknowledgement = std::async(std::launch::async, [&] {
      start_line.arrive_and_wait();
      return coordinator->acknowledge_delivery("parent_delivery_race", delivery.job.identity.job_id, "attempt_race", "turn_race");
    });
    auto exhaustion = std::async(std::launch::async, [&] {
      start_line.arrive_and_wait();
      return coordinator->exhaust_delivery("parent_delivery_race", delivery.job.identity.job_id, "attempt_race");
    });
    start_line.arrive_and_wait();
    auto const acknowledgement_ready = acknowledgement.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto const exhaustion_ready = exhaustion.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto acknowledged = acknowledgement_ready ? acknowledgement.get()
                                              : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(
                                                    std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "acknowledgement race timed out")));
    auto exhausted = exhaustion_ready ? exhaustion.get()
                                      : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(
                                            std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "exhaustion race timed out")));
    auto final = coordinator->result("parent_delivery_race", delivery.job.identity.job_id);
    auto pending = coordinator->pending_deliveries("parent_delivery_race");
    bool const one_winner = acknowledged.has_value() != exhausted.has_value();
    bool const legal_public_state = final && (final->job.delivery == ava::agent::SubagentDeliveryState::Acknowledged ||
                                              final->job.delivery == ava::agent::SubagentDeliveryState::Attempting);
    expect(acknowledgement_ready && exhaustion_ready && one_winner && legal_public_state && pending && pending->empty() &&
               final->job.identity.job_id == delivery.job.identity.job_id,
           "concurrent acknowledgement and exhaustion have one winner, settle pending discovery, and retain an existing public delivery state");
  }
}

void test_real_foreground_interaction_gate_blocks_promotion()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool release = false;
  std::size_t callback_calls = 0;
  auto gate = ava::agent::SubagentInteractionGate::create(
      ava::agent::SubagentJobMode::Foreground,
      [&](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        std::unique_lock lock(mutex);
        ++callback_calls;
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return ava::permissions::PermissionResolutionDecision(ava::permissions::PermissionResolution::Allow);
      },
      nullptr);
  auto permission = gate->permission_resolver();
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start(
      "parent_interaction", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_interaction"},
      [worker](auto const& context) { return worker->run(context); }, gate);
  expect(started && worker->wait_started(), "foreground interaction-gate fixture starts");
  if (!started)
    return;

  auto interaction = std::async(std::launch::async, [&] { return permission(ava::permissions::PermissionPrompt{}); });
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }), "foreground permission callback becomes outstanding deterministically");
  }
  auto blocked = coordinator->promote("parent_interaction", started->job.identity.job_id);
  expect(!blocked && blocked.error().format().find("foreground_interaction_outstanding") != std::string::npos,
         "promotion fails with foreground_interaction_outstanding while the real permission callback is outstanding");
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  auto const interaction_ready = interaction.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  auto interaction_result = interaction_ready ? interaction.get()
                                              : ava::core::Result<ava::permissions::PermissionResolutionDecision>(
                                                    std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "interaction timed out")));
  auto promoted = coordinator->promote("parent_interaction", started->job.identity.job_id);
  auto blocked_in_background = permission(ava::permissions::PermissionPrompt{});
  {
    std::lock_guard lock(mutex);
    expect(interaction_ready && interaction_result && promoted && promoted->job.was_promoted && !blocked_in_background && callback_calls == 1,
           "after release promotion succeeds and no foreground callback leaks into background mode");
  }
  worker->finish();
  static_cast<void>(coordinator->wait("parent_interaction", started->job.identity.job_id, std::chrono::seconds(2)));
}

void test_retention_registry_independence_and_hard_cap()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.max_running_jobs = 1;
  options.registry_options.max_retained_finished_jobs = 1;
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  auto first = complete_background(coordinator, "parent_limits", "child_one", "one");
  auto second = complete_background(coordinator, "parent_limits", "child_two", "two");
  auto listed = coordinator->list("parent_limits");
  expect(listed.size() == 2 && coordinator->pending_deliveries("parent_limits")->size() == 2,
         "pending coordinator states survive low-level registry max-retained pruning");

  auto rejected =
      coordinator->start_background("parent_limits", {.child_session_id = "child_rejected"}, [](auto const&) { return ava::agent::BackgroundJobCompletion{}; });
  expect(!rejected && rejected.error().message().find("state limit") != std::string::npos &&
             ava::agent::subagent_publication_commit_state(rejected.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "start rejects actionably when the hard cap is entirely protected by pending states");

  acknowledge(coordinator, "parent_limits", first, "one");
  auto third = complete_background(coordinator, "parent_limits", "child_three", "three");
  auto after_prune = coordinator->list("parent_limits");
  expect(!third.job.identity.job_id.empty() && after_prune.size() == 2 &&
             std::ranges::none_of(after_prune, [&](auto const& snapshot) { return snapshot.job.identity.job_id == first.job.identity.job_id; }),
         "a new admission prunes the oldest eligible acknowledged result while protecting pending work");
  acknowledge(coordinator, "parent_limits", second, "two");
  acknowledge(coordinator, "parent_limits", third, "three");
  auto retained = coordinator->list("parent_limits");
  expect(retained.size() == 1 && retained.front().job.identity.job_id == third.job.identity.job_id,
         "eligible results retain newest-first within max_retained_finished_jobs");
}

void test_retry_exhaustion_is_internal_and_settled()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.max_running_jobs = 1;
  options.registry_options.max_retained_finished_jobs = 1;
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  auto terminal = complete_background(coordinator, "parent_exhaust", "child_exhaust", "bounded result");
  auto attempted = coordinator->record_delivery_attempt("parent_exhaust", terminal.job.identity.job_id, "attempt_exhaust", "fingerprint_exhaust");
  auto exhausted = attempted ? coordinator->exhaust_delivery("parent_exhaust", terminal.job.identity.job_id, "attempt_exhaust")
                             : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(attempted.error()));
  auto repeated = coordinator->exhaust_delivery("parent_exhaust", terminal.job.identity.job_id, "attempt_exhaust");
  auto pending = coordinator->pending_deliveries("parent_exhaust");
  auto result = coordinator->result("parent_exhaust", terminal.job.identity.job_id);
  expect(exhausted && repeated && pending && pending->empty() && result && result->job.delivery == ava::agent::SubagentDeliveryState::Attempting &&
             result->job.summary == "bounded result" && ava::agent::public_job_snapshot_json(*result).find("delivery_exhausted") == std::string::npos,
         "retry exhaustion is internally settled, no longer rediscovered, queryable, and absent from the public contract");
}

void test_bounded_identity_collision_preserves_existing_job()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.id_generator = [](std::string_view prefix) { return prefix == "job" ? "job_fixed" : "delivery_fixed"; };
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  auto first = coordinator->start_background("parent_collision", {.child_session_id = "child_first"}, [](auto const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "first result", .stop_reason = "completed"};
  });
  auto first_terminal = first ? coordinator->wait("parent_collision", first->job.identity.job_id, std::chrono::seconds(2))
                              : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(first.error()));
  std::atomic<bool> second_worker_called = false;
  auto second = coordinator->start_background("parent_collision", {.child_session_id = "child_second"}, [&](auto const&) {
    second_worker_called.store(true, std::memory_order_release);
    return ava::agent::BackgroundJobCompletion{};
  });
  auto retained = first ? coordinator->result("parent_collision", first->job.identity.job_id)
                        : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(first.error()));
  auto listed = coordinator->list("parent_collision");
  expect(first_terminal && !second && !second_worker_called.load(std::memory_order_acquire) && listed.size() == 1 && retained &&
             retained->job.identity.job_id == "job_fixed" && retained->job.identity.delivery_id == "delivery_fixed" &&
             retained->job.summary == "first result" && second.error().message().find("unique subagent job and delivery identities") != std::string::npos &&
             ava::agent::subagent_publication_commit_state(second.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "eight deterministic identity collisions start no invisible worker and preserve the first visible job and result");
}

void test_identity_generator_faults_are_proven_unpublished()
{
  ava::agent::SubagentCoordinatorOptions invalid_options;
  invalid_options.id_generator = [](std::string_view) { return std::string("../invalid"); };
  auto invalid_coordinator = coordinator_with(std::move(invalid_options));
  std::atomic<bool> invalid_worker_called = false;
  auto invalid = invalid_coordinator->start_background("parent_invalid_generator", {.child_session_id = "child_invalid_generator"}, [&](auto const&) {
    invalid_worker_called.store(true, std::memory_order_release);
    return ava::agent::BackgroundJobCompletion{};
  });

  ava::agent::SubagentCoordinatorOptions throwing_options;
  throwing_options.id_generator = [](std::string_view) -> std::string { throw std::runtime_error("injected identity generator failure"); };
  auto throwing_coordinator = coordinator_with(std::move(throwing_options));
  std::atomic<bool> throwing_worker_called = false;
  auto throwing = throwing_coordinator->start_background("parent_throwing_generator", {.child_session_id = "child_throwing_generator"}, [&](auto const&) {
    throwing_worker_called.store(true, std::memory_order_release);
    return ava::agent::BackgroundJobCompletion{};
  });

  expect(!invalid && !throwing && !invalid_worker_called.load(std::memory_order_acquire) && !throwing_worker_called.load(std::memory_order_acquire) &&
             invalid_coordinator->list("parent_invalid_generator").empty() && throwing_coordinator->list("parent_throwing_generator").empty() &&
             ava::agent::subagent_publication_commit_state(invalid.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished &&
             ava::agent::subagent_publication_commit_state(throwing.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "invalid and throwing identity generators return proven-unpublished errors without leaking exceptions or starting workers");
}

void test_all_start_error_families_are_proven_unpublished()
{
  struct Observation
  {
    ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> result;
    std::size_t visible_before = 0;
    std::size_t visible_after = 0;
    bool rejected_worker_called = false;
  };
  using Case = std::pair<std::string_view, std::function<Observation()>>;
  std::vector<Case> const cases{
      {"null worker",
       [] {
         auto coordinator = coordinator_with();
         auto result = coordinator->start_background("parent_null_worker", {.child_session_id = "child_null_worker"}, {});
         return Observation{.result = std::move(result), .visible_after = coordinator->list("parent_null_worker").size()};
       }},
      {"invalid parent id",
       [] {
         auto coordinator = coordinator_with();
         bool called = false;
         auto result = coordinator->start_background("../invalid", {.child_session_id = "child_invalid_parent"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         return Observation{.result = std::move(result), .visible_after = coordinator->list("../invalid").size(), .rejected_worker_called = called};
       }},
      {"invalid child id",
       [] {
         auto coordinator = coordinator_with();
         bool called = false;
         auto result = coordinator->start_background("parent_invalid_child", {.child_session_id = "../invalid"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         return Observation{.result = std::move(result), .visible_after = coordinator->list("parent_invalid_child").size(), .rejected_worker_called = called};
       }},
      {"coordinator shutdown",
       [] {
         auto coordinator = coordinator_with();
         coordinator->shutdown();
         bool called = false;
         auto result = coordinator->start_background("parent_shutdown_error", {.child_session_id = "child_shutdown_error"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         return Observation{.result = std::move(result), .visible_after = coordinator->list("parent_shutdown_error").size(), .rejected_worker_called = called};
       }},
      {"hard cap",
       [] {
         ava::agent::SubagentCoordinatorOptions options;
         options.registry_options.max_running_jobs = 1;
         options.registry_options.max_retained_finished_jobs = 1;
         auto coordinator = coordinator_with(std::move(options));
         static_cast<void>(complete_background(coordinator, "parent_hard_cap", "child_hard_cap_one", "one"));
         static_cast<void>(complete_background(coordinator, "parent_hard_cap", "child_hard_cap_two", "two"));
         auto before = coordinator->list("parent_hard_cap").size();
         bool called = false;
         auto result = coordinator->start_background("parent_hard_cap", {.child_session_id = "child_hard_cap_rejected"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         return Observation{.result = std::move(result),
                            .visible_before = before,
                            .visible_after = coordinator->list("parent_hard_cap").size(),
                            .rejected_worker_called = called};
       }},
      {"registry running cap",
       [] {
         ava::agent::SubagentCoordinatorOptions options;
         options.registry_options.max_running_jobs = 1;
         options.registry_options.max_retained_finished_jobs = 1;
         auto coordinator = coordinator_with(std::move(options));
         auto worker = std::make_shared<BlockingWorker>();
         auto first = coordinator->start_background("parent_running_cap", {.child_session_id = "child_running_cap_one"},
                                                    [worker](auto const& context) { return worker->run(context); });
         static_cast<void>(worker->wait_started());
         auto before = coordinator->list("parent_running_cap").size();
         bool called = false;
         auto result = coordinator->start_background("parent_running_cap", {.child_session_id = "child_running_cap_two"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         worker->finish();
         if (first)
           static_cast<void>(coordinator->wait("parent_running_cap", first->job.identity.job_id, std::chrono::seconds(2)));
         return Observation{.result = std::move(result),
                            .visible_before = before,
                            .visible_after = coordinator->list("parent_running_cap").size(),
                            .rejected_worker_called = called};
       }},
      {"identity collision exhaustion",
       [] {
         ava::agent::SubagentCoordinatorOptions options;
         options.id_generator = [](std::string_view prefix) { return prefix == "job" ? "job_table" : "delivery_table"; };
         auto coordinator = coordinator_with(std::move(options));
         static_cast<void>(complete_background(coordinator, "parent_collision_table", "child_collision_table_one", "one"));
         auto before = coordinator->list("parent_collision_table").size();
         bool called = false;
         auto result = coordinator->start_background("parent_collision_table", {.child_session_id = "child_collision_table_two"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         return Observation{.result = std::move(result),
                            .visible_before = before,
                            .visible_after = coordinator->list("parent_collision_table").size(),
                            .rejected_worker_called = called};
       }},
      {"registry thread preflight",
       [] {
         ava::agent::SubagentCoordinatorOptions options;
         options.registry_options.thread_start_preflight = []() -> ava::core::VoidResult {
           return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "injected thread start failure"));
         };
         auto coordinator = coordinator_with(std::move(options));
         bool called = false;
         auto result = coordinator->start_background("parent_preflight", {.child_session_id = "child_preflight"}, [&](auto const&) {
           called = true;
           return ava::agent::BackgroundJobCompletion{};
         });
         return Observation{.result = std::move(result), .visible_after = coordinator->list("parent_preflight").size(), .rejected_worker_called = called};
       }},
  };

  for (auto const& [name, run] : cases)
  {
    auto observation = run();
    expect(!observation.result && !observation.rejected_worker_called && observation.visible_after == observation.visible_before &&
               ava::agent::subagent_publication_commit_state(observation.result.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
           std::string(name) + " start failure is proven unpublished and starts or exposes no rejected worker");
  }
}

ScopedTestDirectory coordinator_temp_root()
{
  return ScopedTestDirectory::create_unique(temp_root(), "ava-subagent-inspect-");
}

struct PersistentChildFixture
{
  ScopedTestDirectory owned_root;
  std::filesystem::path root;
  ava::session::SessionStore store;
  ava::session::SessionLease lease;
  std::shared_ptr<ava::session::SessionAppendTarget> append_target;
  ava::session::SessionReadAuthority authority;
  std::shared_ptr<ava::agent::SubagentLiveInspectionSource> source;
};

// Build a persistent inspectable child and retain its cached append authority for later fixture mutations.
//
// Returns no fixture when lease, append-target, seed append, read-authority, or inspection-source construction fails.
std::optional<PersistentChildFixture> make_child_fixture(std::string session_id, std::string seed_text = "hello child")
{
  auto owned_root = coordinator_temp_root();
  auto root = owned_root.path();
  auto workspace = root / "workspace";
  auto sessions = root / "sessions";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = std::move(session_id)});
  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  if (!lease)
    return std::nullopt;
  auto append_target = ava::session::SessionAppendTarget::create_persistent(store, *lease, ava::session::SessionReadLimits{});
  if (!append_target)
    return std::nullopt;
  auto entry = ava::session::SessionEntry{.id = "u1",
                                          .parent_id = "",
                                          .type = ava::session::EntryType::UserMessage,
                                          .timestamp = ava::session::now_timestamp(),
                                          .data_json = "{\"text\":\"" + seed_text + "\"}"};
  if (!(*append_target)->append(entry))
    return std::nullopt;
  auto authority = ava::session::SessionReadAuthority::create_persistent(store, *lease);
  if (!authority)
    return std::nullopt;
  auto source = ava::agent::SubagentLiveInspectionSource::create(*authority);
  if (!source)
    return std::nullopt;
  return PersistentChildFixture{
      .owned_root = std::move(owned_root),
      .root = std::move(root),
      .store = std::move(store),
      .lease = std::move(*lease),
      .append_target = std::move(*append_target),
      .authority = std::move(*authority),
      .source = std::move(*source),
  };
}

void test_live_inspection_missing_mismatch_owner_and_prepublication()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;

  auto missing = coordinator->start_background("parent_inspect", {.child_session_id = "child_missing_source"}, [](auto const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "done", .stop_reason = "completed"};
  });
  expect(missing.has_value(), "legacy start without source still publishes");
  if (!missing)
    return;
  auto unavailable = coordinator->inspect("parent_inspect", missing->job.identity.job_id);
  expect(unavailable && (*unavailable)->unavailable && !(*unavailable)->not_modified, "missing inspection source returns a stable unavailable frame");

  auto child = make_child_fixture("child_match");
  expect(static_cast<bool>(child), "inspection mismatch fixture builds child session");
  if (!child)
    return;
  auto mismatched = coordinator->start_background(
      "parent_inspect", {.child_session_id = "child_other_id"},
      [](auto const&) {
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "x", .stop_reason = "completed"};
      },
      child->source);
  expect(!mismatched && ava::agent::subagent_publication_commit_state(mismatched.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "source/session_id mismatch is proven unpublished before worker launch");

  auto ephemeral = ava::session::SessionStore::create_ephemeral(child->root / "workspace");
  expect(ephemeral.has_value(), "ephemeral rejection fixture creates store");
  if (ephemeral)
  {
    auto ephemeral_authority = ava::session::SessionReadAuthority::create_ephemeral(*ephemeral);
    expect(ephemeral_authority.has_value(), "ephemeral rejection fixture creates authority");
    if (ephemeral_authority)
    {
      auto ephemeral_source = ava::agent::SubagentLiveInspectionSource::create(*ephemeral_authority);
      expect(ephemeral_source.has_value(), "ephemeral rejection fixture creates source");
      if (ephemeral_source)
      {
        auto rejected = coordinator->start_background(
            "parent_inspect", {.child_session_id = ephemeral_authority->session_id()},
            [](auto const&) {
              return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "x", .stop_reason = "completed"};
            },
            *ephemeral_source);
        expect(!rejected && ava::agent::subagent_publication_commit_state(rejected.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
               "ephemeral inspection sources are rejected as proven unpublished");
      }
    }
  }

  auto worker = std::make_shared<BlockingWorker>();
  auto matched_child = make_child_fixture("child_owner_a");
  expect(static_cast<bool>(matched_child), "owner isolation fixture builds child");
  if (!matched_child)
    return;
  auto started = coordinator->start_background(
      "parent_a", {.child_session_id = matched_child->store.session_id()}, [worker](auto const& context) { return worker->run(context, "owner"); },
      matched_child->source);
  expect(started && worker->wait_started(), "owner isolation fixture starts");
  if (!started)
    return;
  auto wrong_owner = coordinator->inspect("parent_b", started->job.identity.job_id);
  expect(!wrong_owner, "inspect is owner-bound and rejects foreign parents");
  auto live = coordinator->inspect("parent_a", started->job.identity.job_id);
  expect(live && !(*live)->unavailable && !(*live)->terminal && (*live)->messages.size() == 1 && (*live)->messages.front().text == "hello child",
         "pre-publication source is inspectable after start with committed user prefix");
  worker->finish();
  auto terminal = coordinator->wait("parent_a", started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out, "owner isolation fixture reaches terminal");
}

void test_live_inspection_generation_freeze_and_lifecycle()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;

  auto child = make_child_fixture("child_freeze", "prefix");
  expect(static_cast<bool>(child), "freeze fixture builds child");
  if (!child)
    return;

  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background(
      "parent_freeze", {.child_session_id = child->store.session_id()}, [worker](auto const& context) { return worker->run(context, "final summary"); },
      child->source);
  expect(started && worker->wait_started(), "freeze fixture starts running job");
  if (!started)
    return;

  auto first = coordinator->inspect("parent_freeze", started->job.identity.job_id);
  expect(first && (*first)->generation == 1 && !(*first)->refresh_unavailable && (*first)->messages.size() == 1 && (*first)->messages.front().text == "prefix",
         "live inspect projects committed prefix under content generation 1");
  if (!first)
    return;
  auto not_modified = coordinator->inspect("parent_freeze", started->job.identity.job_id, (*first)->generation);
  expect(not_modified && (*not_modified)->not_modified && (*not_modified)->generation == (*first)->generation,
         "known_generation skips reload when fingerprint is unchanged");

  auto assistant = ava::session::SessionEntry{.id = "a1",
                                              .parent_id = "",
                                              .type = ava::session::EntryType::AssistantMessage,
                                              .timestamp = ava::session::now_timestamp(),
                                              .data_json = "{\"text\":\"live update\"}"};
  expect(child->append_target->append(assistant).has_value(), "freeze fixture appends assistant message");
  auto updated = coordinator->inspect("parent_freeze", started->job.identity.job_id, (*first)->generation);
  expect(updated && !(*updated)->not_modified && (*updated)->generation == 2 && (*updated)->messages.size() == 2 &&
             (*updated)->messages.back().text == "live update",
         "fingerprint change advances content generation and forces reload");

  worker->finish();
  auto terminal = coordinator->wait("parent_freeze", started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out && terminal->job.execution == ava::agent::SubagentExecutionState::Completed, "freeze fixture completes");

  auto frozen = coordinator->inspect("parent_freeze", started->job.identity.job_id);
  expect(frozen && (*frozen)->terminal && !(*frozen)->unavailable && !(*frozen)->freeze_pending && !(*frozen)->refresh_unavailable &&
             (*frozen)->generation == 3 && (*frozen)->messages.size() == 2,
         "terminal freeze stores a path-free frame and advances content generation for terminal metadata");
  if (!frozen)
    return;
  auto frozen_not_modified = coordinator->inspect("parent_freeze", started->job.identity.job_id, (*frozen)->generation);
  expect(frozen_not_modified && (*frozen_not_modified)->not_modified, "frozen generation returns not_modified");
  auto stale_known = coordinator->inspect("parent_freeze", started->job.identity.job_id, (*updated)->generation);
  expect(stale_known && !(*stale_known)->not_modified && (*stale_known)->generation == (*frozen)->generation,
         "stale known_generation after terminal publish returns the newer frozen frame");

  // Immediate terminal completion still freezes when a source is present.
  auto immediate_child = make_child_fixture("child_immediate", "immediate");
  expect(static_cast<bool>(immediate_child), "immediate terminal fixture builds child");
  if (!immediate_child)
    return;
  ava::agent::SubagentCoordinatorOptions immediate_options;
  immediate_options.registry_options.wait_for_terminal_before_start_returns = true;
  auto immediate_coordinator = coordinator_with(std::move(immediate_options));
  if (!immediate_coordinator)
    return;
  auto immediate = immediate_coordinator->start_background(
      "parent_immediate", {.child_session_id = immediate_child->store.session_id()},
      [](auto const&) {
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "done", .stop_reason = "completed"};
      },
      immediate_child->source);
  expect(immediate.has_value(), "immediate terminal start publishes");
  if (!immediate)
    return;
  auto immediate_frame = immediate_coordinator->inspect("parent_immediate", immediate->job.identity.job_id);
  expect(immediate_frame && (*immediate_frame)->terminal && (*immediate_frame)->generation >= 1 && (*immediate_frame)->messages.size() == 1 &&
             (*immediate_frame)->messages.front().text == "immediate",
         "immediate terminal completion freezes the committed prefix");

  // Promote/cancel retain owner inspect semantics while running.
  auto promote_child = make_child_fixture("child_promote");
  expect(static_cast<bool>(promote_child), "promote inspect fixture builds child");
  if (!promote_child)
    return;
  auto promote_worker = std::make_shared<BlockingWorker>();
  auto gate = ava::agent::SubagentInteractionGate::create(ava::agent::SubagentJobMode::Foreground, nullptr, nullptr);
  auto promoted_start = coordinator->start(
      "parent_promote", ava::agent::SubagentJobMode::Foreground, {.child_session_id = promote_child->store.session_id()},
      [promote_worker](auto const& context) { return promote_worker->run(context); }, gate, promote_child->source);
  expect(promoted_start && promote_worker->wait_started(), "promote inspect fixture starts");
  if (!promoted_start)
    return;
  auto promoted = coordinator->promote("parent_promote", promoted_start->job.identity.job_id);
  auto promote_frame = coordinator->inspect("parent_promote", promoted_start->job.identity.job_id);
  expect(promoted && promote_frame && !(*promote_frame)->unavailable, "promote leaves inspection available for the owner");
  auto canceled = coordinator->cancel("parent_promote", promoted_start->job.identity.job_id);
  promote_worker->finish();
  auto canceled_wait = coordinator->wait("parent_promote", promoted_start->job.identity.job_id, std::chrono::seconds(2));
  auto canceled_frame = coordinator->inspect("parent_promote", promoted_start->job.identity.job_id);
  expect(canceled && canceled_wait && canceled_frame && (*canceled_frame)->terminal, "cancel reaches terminal frozen/unavailable inspection");

  // Shutdown drops residual sources and owner visibility.
  auto shutdown_child = make_child_fixture("child_shutdown_src");
  expect(static_cast<bool>(shutdown_child), "shutdown source fixture builds child");
  if (!shutdown_child)
    return;
  auto shutdown_worker = std::make_shared<BlockingWorker>();
  auto shutdown_start = coordinator->start_background(
      "parent_shutdown_src", {.child_session_id = shutdown_child->store.session_id()},
      [shutdown_worker](auto const& context) { return shutdown_worker->run(context); }, shutdown_child->source);
  expect(shutdown_start && shutdown_worker->wait_started(), "shutdown source fixture starts");
  if (!shutdown_start)
    return;
  coordinator->shutdown();
  auto after_shutdown = coordinator->inspect("parent_shutdown_src", shutdown_start->job.identity.job_id);
  expect(!after_shutdown, "shutdown removes jobs from owner inspection");
}

void test_live_inspection_deterministic_stale_inflight_race()
{
  std::mutex hook_mutex;
  std::condition_variable hook_cv;
  bool arm_hook = false;
  bool hook_entered = false;
  bool release_hook = false;

  ava::agent::SubagentCoordinatorOptions options;
  options.inspect_after_source_capture_for_test = [&] {
    std::unique_lock lock(hook_mutex);
    if (!arm_hook)
      return;
    hook_entered = true;
    hook_cv.notify_all();
    expect(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_hook; }), "inspect_after_source_capture_for_test waits with a finite deadline");
  };
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;

  auto child = make_child_fixture("child_race", "race-prefix");
  expect(static_cast<bool>(child), "deterministic race fixture builds child");
  if (!child)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background(
      "parent_race", {.child_session_id = child->store.session_id()}, [worker](auto const& context) { return worker->run(context, "race-done"); },
      child->source);
  expect(started && worker->wait_started(), "deterministic race fixture starts");
  if (!started)
    return;

  auto live = coordinator->inspect("parent_race", started->job.identity.job_id);
  expect(live && (*live)->generation == 1 && !(*live)->terminal, "race fixture publishes an initial live generation");
  if (!live)
    return;

  auto assistant = ava::session::SessionEntry{.id = "a_race",
                                              .parent_id = "",
                                              .type = ava::session::EntryType::AssistantMessage,
                                              .timestamp = ava::session::now_timestamp(),
                                              .data_json = "{\"text\":\"stale-live\"}"};
  expect(child->append_target->append(assistant).has_value(), "race fixture appends before paused inspect");

  {
    std::lock_guard lock(hook_mutex);
    arm_hook = true;
  }
  auto in_flight = std::async(std::launch::async, [&] { return coordinator->inspect("parent_race", started->job.identity.job_id, (*live)->generation); });
  {
    std::unique_lock lock(hook_mutex);
    expect(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] { return hook_entered; }), "paused inspect reaches source-capture seam");
  }

  worker->finish();
  auto terminal = coordinator->wait("parent_race", started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out && terminal->job.execution == ava::agent::SubagentExecutionState::Completed,
         "complete/freeze runs while inspect is paused at the source-capture seam");

  // Terminal freeze inspect must not re-enter the armed live-source seam.
  auto frozen = coordinator->inspect("parent_race", started->job.identity.job_id);
  expect(frozen && (*frozen)->terminal && !(*frozen)->freeze_pending && (*frozen)->generation >= 2,
         "terminal freeze publishes before the paused inspect resumes");

  {
    std::lock_guard lock(hook_mutex);
    release_hook = true;
    hook_cv.notify_all();
  }
  auto in_flight_result = in_flight.get();
  expect(in_flight_result && (*in_flight_result)->terminal && !(*in_flight_result)->freeze_pending &&
             (*in_flight_result)->generation == (*frozen)->generation && !(*in_flight_result)->not_modified,
         "stale in-flight inspect returns the current terminal generation, not a stale live frame");
}

void test_live_inspection_monotonic_concurrent_publish_race()
{
  // ARCH-INS-09: inspector A projects C1 then pauses before publish; inspector B
  // publishes C2; A must return current C2 without advancing generation or
  // regressing content. Only A is blocked: remaining_blocks starts at 1.
  std::mutex hook_mutex;
  std::condition_variable hook_cv;
  int remaining_blocks = 0;
  bool hook_entered = false;
  bool release_hook = false;

  ava::agent::SubagentCoordinatorOptions options;
  options.inspect_before_publish_for_test = [&] {
    std::unique_lock lock(hook_mutex);
    if (remaining_blocks <= 0)
      return;
    --remaining_blocks;
    hook_entered = true;
    hook_cv.notify_all();
    expect(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_hook; }), "inspect_before_publish_for_test waits with a finite deadline");
  };
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;

  auto child = make_child_fixture("child_mono_race", "seed");
  expect(static_cast<bool>(child), "monotonic race fixture builds child");
  if (!child)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background(
      "parent_mono_race", {.child_session_id = child->store.session_id()}, [worker](auto const& context) { return worker->run(context, "mono-done"); },
      child->source);
  expect(started && worker->wait_started(), "monotonic race fixture starts");
  if (!started)
    return;

  auto initial = coordinator->inspect("parent_mono_race", started->job.identity.job_id);
  expect(initial && (*initial)->generation == 1 && (*initial)->messages.size() == 1 && (*initial)->messages.front().text == "seed",
         "monotonic race starts at generation 1 with seed content");
  if (!initial)
    return;

  auto c1 = ava::session::SessionEntry{.id = "a_c1",
                                       .parent_id = "",
                                       .type = ava::session::EntryType::AssistantMessage,
                                       .timestamp = ava::session::now_timestamp(),
                                       .data_json = "{\"text\":\"C1\"}"};
  expect(child->append_target->append(c1).has_value(), "monotonic race appends C1");

  {
    std::lock_guard lock(hook_mutex);
    remaining_blocks = 1;
    hook_entered = false;
    release_hook = false;
  }
  auto inspector_a =
      std::async(std::launch::async, [&] { return coordinator->inspect("parent_mono_race", started->job.identity.job_id, (*initial)->generation); });
  {
    std::unique_lock lock(hook_mutex);
    expect(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] { return hook_entered; }), "inspector A reaches before-publish seam with C1 projected");
  }

  auto c2 = ava::session::SessionEntry{.id = "a_c2",
                                       .parent_id = "",
                                       .type = ava::session::EntryType::AssistantMessage,
                                       .timestamp = ava::session::now_timestamp(),
                                       .data_json = "{\"text\":\"C2\"}"};
  expect(child->append_target->append(c2).has_value(), "monotonic race appends C2 while A is paused");

  // remaining_blocks is already 0, so B must not block on the before-publish seam.
  auto inspector_b = coordinator->inspect("parent_mono_race", started->job.identity.job_id, (*initial)->generation);
  expect(inspector_b && !(*inspector_b)->not_modified && (*inspector_b)->generation == 2 && (*inspector_b)->messages.size() == 3 &&
             (*inspector_b)->messages.back().text == "C2",
         "inspector B publishes C2 at generation 2 without blocking");
  if (!inspector_b)
  {
    std::lock_guard lock(hook_mutex);
    release_hook = true;
    hook_cv.notify_all();
    inspector_a.wait();
    return;
  }

  {
    std::lock_guard lock(hook_mutex);
    release_hook = true;
    hook_cv.notify_all();
  }
  auto a_result = inspector_a.get();
  expect(
      a_result && !(*a_result)->not_modified && (*a_result)->generation == 2 && (*a_result)->messages.size() == 3 && (*a_result)->messages.back().text == "C2",
      "inspector A returns current C2 generation/content without regressing to C1 or minting a new generation");

  auto current = coordinator->inspect("parent_mono_race", started->job.identity.job_id, (*inspector_b)->generation);
  expect(current && (*current)->not_modified && (*current)->generation == 2, "current known generation returns not_modified at C2");
  auto stale = coordinator->inspect("parent_mono_race", started->job.identity.job_id, (*initial)->generation);
  expect(stale && !(*stale)->not_modified && (*stale)->generation == 2 && (*stale)->messages.size() == 3 && (*stale)->messages.back().text == "C2",
         "stale known generation returns current C2 frame");

  worker->finish();
  auto terminal = coordinator->wait("parent_mono_race", started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out, "monotonic race fixture reaches terminal");
}

void test_live_inspection_two_client_lost_response_and_sink()
{
  std::mutex sink_mutex;
  std::condition_variable sink_cv;
  bool sink_seen = false;
  bool sink_frame_stable = false;
  std::uint64_t sink_generation = 0;

  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  coordinator->set_terminal_sink([&](ava::agent::SubagentCoordinatorJobSnapshot const& snapshot) {
    auto frame = coordinator->inspect(snapshot.job.identity.parent_session_id, snapshot.job.identity.job_id);
    std::lock_guard lock(sink_mutex);
    sink_seen = true;
    sink_frame_stable = frame && (*frame)->terminal && !(*frame)->freeze_pending && !(*frame)->unavailable;
    if (frame)
      sink_generation = (*frame)->generation;
    sink_cv.notify_all();
  });

  auto child = make_child_fixture("child_two_client", "c1");
  expect(static_cast<bool>(child), "two-client fixture builds child");
  if (!child)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background(
      "parent_two_client", {.child_session_id = child->store.session_id()}, [worker](auto const& context) { return worker->run(context, "done"); },
      child->source);
  expect(started && worker->wait_started(), "two-client fixture starts");
  if (!started)
    return;

  auto client_a = coordinator->inspect("parent_two_client", started->job.identity.job_id);
  expect(client_a && (*client_a)->generation == 1, "client A observes generation 1");
  if (!client_a)
    return;

  auto assistant = ava::session::SessionEntry{.id = "a_two",
                                              .parent_id = "",
                                              .type = ava::session::EntryType::AssistantMessage,
                                              .timestamp = ava::session::now_timestamp(),
                                              .data_json = "{\"text\":\"second\"}"};
  expect(child->append_target->append(assistant).has_value(), "two-client fixture appends");

  auto client_b = coordinator->inspect("parent_two_client", started->job.identity.job_id);
  expect(client_b && (*client_b)->generation == 2 && (*client_b)->messages.size() == 2, "client B observes generation 2");
  if (!client_b)
    return;

  // Lost response: client A retries with stale known_generation and must receive
  // the cached newer frame rather than not_modified.
  auto lost = coordinator->inspect("parent_two_client", started->job.identity.job_id, (*client_a)->generation);
  expect(lost && !(*lost)->not_modified && (*lost)->generation == 2 && (*lost)->messages.size() == 2,
         "lost-response retry with stale generation returns the newer cached frame");
  auto lost_again = coordinator->inspect("parent_two_client", started->job.identity.job_id, (*client_a)->generation);
  expect(lost_again && !(*lost_again)->not_modified && (*lost_again)->generation == 2, "second lost-response retry still returns the newer cached frame");
  auto current = coordinator->inspect("parent_two_client", started->job.identity.job_id, (*client_b)->generation);
  expect(current && (*current)->not_modified && (*current)->generation == 2, "current generation still short-circuits");

  worker->finish();
  auto terminal = coordinator->wait("parent_two_client", started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out, "two-client fixture completes");
  {
    std::unique_lock lock(sink_mutex);
    expect(sink_cv.wait_for(lock, std::chrono::seconds(2), [&] { return sink_seen; }), "terminal sink fires after freeze");
    expect(sink_frame_stable && sink_generation >= 3, "terminal sink observes a stable freeze, not freeze_pending/gap");
  }
}

void test_live_inspection_path_free_refresh_failures()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;

  auto child = make_child_fixture("child_path_free", "seed");
  expect(static_cast<bool>(child), "path-free fixture builds child");
  if (!child)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background(
      "parent_path_free", {.child_session_id = child->store.session_id()}, [worker](auto const& context) { return worker->run(context, "x"); }, child->source);
  expect(started && worker->wait_started(), "path-free fixture starts");
  if (!started)
    return;

  auto first = coordinator->inspect("parent_path_free", started->job.identity.job_id);
  expect(first && (*first)->generation == 1 && (*first)->messages.size() == 1, "path-free fixture publishes live frame");
  if (!first)
    return;

  {
    std::ofstream corrupt(child->store.session_path(), std::ios::app | std::ios::binary);
    corrupt << "{this is not a session record}\n";
  }
  auto corrupted = coordinator->inspect("parent_path_free", started->job.identity.job_id, (*first)->generation);
  expect(corrupted.has_value(), "corrupt live inspect returns a path-free frame, not a raw error");
  if (corrupted)
  {
    expect(
        (*corrupted)->refresh_unavailable && !(*corrupted)->not_modified && (*corrupted)->messages.size() == 1 && (*corrupted)->messages.front().text == "seed",
        "corrupt live inspect retains prior messages and sets refresh_unavailable");
    auto const rendered = (*corrupted)->messages.front().text;
    expect(rendered.find(child->store.session_path().string()) == std::string::npos, "path-free frame omits session path text");
    expect(!(*corrupted)->unavailable || !(*corrupted)->messages.empty() || (*corrupted)->refresh_unavailable,
           "refresh failure is flagged without leaking load diagnostics");
  }

  // Over-cap: append enough valid entries to exceed the inspector max_entries.
  auto over_child = make_child_fixture("child_over_cap", "cap-seed");
  expect(static_cast<bool>(over_child), "over-cap fixture builds child");
  if (!over_child)
    return;
  auto over_worker = std::make_shared<BlockingWorker>();
  auto over_started = coordinator->start_background(
      "parent_over_cap", {.child_session_id = over_child->store.session_id()}, [over_worker](auto const& context) { return over_worker->run(context, "x"); },
      over_child->source);
  expect(over_started && over_worker->wait_started(), "over-cap fixture starts");
  if (!over_started)
    return;
  auto over_first = coordinator->inspect("parent_over_cap", over_started->job.identity.job_id);
  expect(over_first && (*over_first)->generation == 1, "over-cap fixture has an initial frame");
  for (std::size_t index = 0; index < ava::agent::kSubagentInspectorMaxEntries + 2; ++index)
  {
    auto entry = ava::session::SessionEntry{.id = "u_over_" + std::to_string(index),
                                            .parent_id = "",
                                            .type = ava::session::EntryType::UserMessage,
                                            .timestamp = ava::session::now_timestamp(),
                                            .data_json = "{\"text\":\"x\"}"};
    expect(over_child->append_target->append(entry).has_value(), "over-cap fixture appends");
  }
  auto over = coordinator->inspect("parent_over_cap", over_started->job.identity.job_id, (*over_first)->generation);
  expect(over && (*over)->refresh_unavailable && (*over)->messages.size() == 1, "over-cap live inspect is path-free refresh_unavailable with prior messages");
  if (over)
  {
    expect(!(*over)->not_modified, "over-cap does not claim not_modified");
    // Ensure no raw session error strings leaked into message text.
    for (auto const& message : (*over)->messages)
    {
      expect(message.text.find("exceeds bounded read limit") == std::string::npos, "path-free frame omits raw load error text");
      expect(message.text.find(over_child->store.session_path().string()) == std::string::npos, "path-free frame omits path context");
    }
  }

  worker->finish();
  over_worker->finish();
  static_cast<void>(coordinator->wait("parent_path_free", started->job.identity.job_id, std::chrono::seconds(2)));
  static_cast<void>(coordinator->wait("parent_over_cap", over_started->job.identity.job_id, std::chrono::seconds(2)));
}

void test_live_inspection_eviction_and_freeze_failure()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.max_running_jobs = 1;
  options.registry_options.max_retained_finished_jobs = 1;
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;

  auto first_child = make_child_fixture("child_evict_1");
  auto second_child = make_child_fixture("child_evict_2");
  auto third_child = make_child_fixture("child_evict_3");
  expect(first_child && second_child && third_child, "eviction fixtures build children");
  if (!first_child || !second_child || !third_child)
    return;

  auto finish = [&](std::string parent, PersistentChildFixture& child, std::string suffix) {
    // Foreground/direct terminal jobs are retention-eligible immediately, so the
    // hard cap can drop older sources without delivery-ack choreography.
    auto started = coordinator->start(
        parent, ava::agent::SubagentJobMode::Foreground, {.child_session_id = child.store.session_id()},
        [](auto const&) {
          return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "done", .stop_reason = "completed"};
        },
        nullptr, child.source);
    expect(started.has_value(), "eviction fixture starts: " + suffix);
    if (!started)
      return std::string{};
    auto terminal = coordinator->wait(parent, started->job.identity.job_id, std::chrono::seconds(2));
    expect(terminal && !terminal->timed_out, "eviction fixture completes: " + suffix);
    return started->job.identity.job_id;
  };
  auto id1 = finish("parent_evict", *first_child, "1");
  auto id2 = finish("parent_evict", *second_child, "2");
  auto id3 = finish("parent_evict", *third_child, "3");
  expect(!id1.empty() && !id2.empty() && !id3.empty(), "eviction fixture publishes three jobs");
  auto listed = coordinator->list("parent_evict");
  expect(listed.size() <= 1, "hard retention cap evicts older terminal jobs and their sources");
  if (!listed.empty())
  {
    auto kept = coordinator->inspect("parent_evict", listed.front().job.identity.job_id);
    expect(kept.has_value(), "retained job remains inspectable");
  }
  auto gone = coordinator->inspect("parent_evict", id1);
  expect(!gone, "evicted job is no longer owner-visible to inspect");

  // Freeze failure with a prior live frame: retain messages and mark refresh_unavailable.
  auto fail_child = make_child_fixture("child_freeze_fail", "keep-me");
  expect(static_cast<bool>(fail_child), "freeze-failure fixture builds child");
  if (!fail_child)
    return;
  auto failure_coordinator = coordinator_with();
  if (!failure_coordinator)
    return;
  auto fail_worker = std::make_shared<BlockingWorker>();
  auto fail_started = failure_coordinator->start_background(
      "parent_fail", {.child_session_id = fail_child->store.session_id()}, [fail_worker](auto const& context) { return fail_worker->run(context, "x"); },
      fail_child->source);
  expect(fail_started && fail_worker->wait_started(), "freeze-failure fixture starts");
  if (!fail_started)
    return;
  auto prior = failure_coordinator->inspect("parent_fail", fail_started->job.identity.job_id);
  expect(prior && (*prior)->generation == 1 && (*prior)->messages.front().text == "keep-me", "freeze-failure fixture caches a live frame");
  {
    std::ofstream corrupt(fail_child->store.session_path(), std::ios::app | std::ios::binary);
    corrupt << "{this is not a session record}\n";
  }
  fail_worker->finish();
  auto fail_terminal = failure_coordinator->wait("parent_fail", fail_started->job.identity.job_id, std::chrono::seconds(2));
  expect(fail_terminal && !fail_terminal->timed_out, "freeze-failure fixture reaches terminal");
  auto fail_frame = failure_coordinator->inspect("parent_fail", fail_started->job.identity.job_id);
  expect(fail_frame && (*fail_frame)->terminal && !(*fail_frame)->freeze_pending && (*fail_frame)->refresh_unavailable && !(*fail_frame)->unavailable &&
             (*fail_frame)->generation > (*prior)->generation && (*fail_frame)->messages.size() == 1 && (*fail_frame)->messages.front().text == "keep-me",
         "failed terminal freeze retains the last live frame and marks refresh_unavailable");

  // Freeze failure with no prior frame: stable unavailable.
  auto bare_child = make_child_fixture("child_freeze_bare", "ignored");
  expect(static_cast<bool>(bare_child), "bare freeze-failure fixture builds child");
  if (!bare_child)
    return;
  auto bare_coordinator = coordinator_with();
  if (!bare_coordinator)
    return;
  {
    std::ofstream corrupt(bare_child->store.session_path(), std::ios::app | std::ios::binary);
    corrupt << "{this is not a session record}\n";
  }
  auto bare_started = bare_coordinator->start_background(
      "parent_bare", {.child_session_id = bare_child->store.session_id()},
      [](auto const&) {
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "x", .stop_reason = "completed"};
      },
      bare_child->source);
  expect(bare_started.has_value(), "bare freeze-failure starts");
  if (!bare_started)
    return;
  auto bare_wait = bare_coordinator->wait("parent_bare", bare_started->job.identity.job_id, std::chrono::seconds(2));
  expect(bare_wait && !bare_wait->timed_out, "bare freeze-failure reaches terminal");
  auto bare_frame = bare_coordinator->inspect("parent_bare", bare_started->job.identity.job_id);
  expect(bare_frame && (*bare_frame)->terminal && (*bare_frame)->unavailable && !(*bare_frame)->freeze_pending,
         "failed terminal freeze with no prior frame surfaces stable unavailable");
}

void test_parent_maintenance_serializes_start_and_live_jobs()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;

  {
    auto maintenance = coordinator->reserve_parent_maintenance({"parent_maintenance", "parent_maintenance"});
    std::atomic<bool> worker_called = false;
    auto rejected = coordinator->start_background("parent_maintenance", {.child_session_id = "child_blocked"}, [&](auto const&) {
      worker_called.store(true, std::memory_order_release);
      return ava::agent::BackgroundJobCompletion{};
    });
    expect(maintenance && maintenance->active() && !rejected && !worker_called.load(std::memory_order_acquire) &&
               ava::agent::subagent_publication_commit_state(rejected.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished &&
               rejected.error().format().find("parent_maintenance") == std::string::npos,
           "parent maintenance deduplicates private identities and rejects a racing start without worker publication or identity disclosure");
  }

  struct StartBarrier
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool release = false;
  };
  auto start_barrier = std::make_shared<StartBarrier>();
  ava::agent::SubagentCoordinatorOptions starting_options;
  starting_options.id_generator = [start_barrier](std::string_view prefix) {
    if (prefix == "job")
    {
      std::unique_lock lock(start_barrier->mutex);
      start_barrier->reached = true;
      start_barrier->changed.notify_all();
      start_barrier->changed.wait(lock, [&] { return start_barrier->release; });
    }
    return prefix == "job" ? std::string("job_start_barrier") : std::string("delivery_start_barrier");
  };
  auto starting_coordinator = coordinator_with(std::move(starting_options));
  std::optional<ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>> starting_result;
  std::jthread starter([&] {
    starting_result.emplace(starting_coordinator->start_background("parent_starting_maintenance", {.child_session_id = "child_starting"}, [](auto const&) {
      return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "started", .stop_reason = "completed"};
    }));
  });
  {
    std::unique_lock lock(start_barrier->mutex);
    expect(start_barrier->changed.wait_for(lock, std::chrono::seconds(3), [&] { return start_barrier->reached; }),
           "starting-job fixture pauses before registry publication");
  }
  auto during_start = starting_coordinator->reserve_parent_maintenance({"parent_starting_maintenance"});
  {
    std::lock_guard lock(start_barrier->mutex);
    start_barrier->release = true;
  }
  start_barrier->changed.notify_all();
  starter.join();
  expect(!during_start && starting_result && starting_result->has_value(),
         "a start already in progress wins maintenance arbitration and publishes normally after maintenance rejection");

  auto worker = std::make_shared<BlockingWorker>();
  auto running = coordinator->start_background("parent_maintenance", {.child_session_id = "child_running"},
                                               [worker](auto const& context) { return worker->run(context); });
  expect(running && worker->wait_started() && !coordinator->reserve_parent_maintenance({"parent_maintenance"}),
         "a running background job prevents parent maintenance acquisition");
  worker->finish();
  if (running)
    static_cast<void>(coordinator->wait("parent_maintenance", running->job.identity.job_id, std::chrono::seconds(2)));
  auto after = coordinator->reserve_parent_maintenance({"parent_maintenance"});
  expect(after && after->active(), "parent maintenance becomes available after the running job reaches terminal state");
}

void test_owner_checked_bounded_steering_fifo()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  auto queue = ava::agent::SubagentSteeringQueue::create();
  auto started = coordinator->start(ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "steer_owner",
                                                                                .mode = ava::agent::SubagentJobMode::Background,
                                                                                .job = {.child_session_id = "steer_child"},
                                                                                .steering_queue = queue},
                                    [worker](auto const& context) { return worker->run(context); });
  expect(started && worker->wait_started(), "steering fixture starts with a child-owned queue");
  if (!started)
    return;
  auto const job_id = started->job.identity.job_id;
  auto hidden = coordinator->steer("wrong_owner", job_id, "hidden");
  auto first = coordinator->steer("steer_owner", job_id, "first");
  auto second = coordinator->steer("steer_owner", job_id, "second");
  auto drained = queue->take();
  auto replay = queue->take();
  bool filled = true;
  for (std::size_t index = 0; index < 16; ++index)
    filled = filled && coordinator->steer("steer_owner", job_id, "queued-" + std::to_string(index)).has_value();
  auto full = coordinator->steer("steer_owner", job_id, "overflow");
  auto canceled = coordinator->cancel("steer_owner", job_id);
  auto after_cancel = coordinator->steer("steer_owner", job_id, "too late");
  auto terminal = coordinator->wait("steer_owner", job_id, std::chrono::seconds(2));
  auto after_terminal = coordinator->steer("steer_owner", job_id, "terminal");
  expect(!hidden && hidden.error().category() == ava::core::ErrorCategory::NotFound && first && second && drained &&
             *drained == std::vector<std::string>({"first", "second"}) && replay && replay->empty() && filled && !full && canceled && !after_cancel &&
             terminal && terminal->job.execution == ava::agent::SubagentExecutionState::Canceled && !after_terminal,
         "steering is owner-checked FIFO, drains exactly once, rejects overflow, and closes on cancellation or terminal state");
}

struct CapturingHistorySink
{
  std::mutex mutex;
  std::vector<ava::session::SessionEntry> entries;
  bool fail_start = false;
  bool fail_terminal = false;
  std::size_t calls = 0;

  ava::core::VoidResult operator()(ava::session::SessionEntry entry)
  {
    std::lock_guard lock(mutex);
    ++calls;
    auto parsed = ava::session::parse_subagent_job_history_entry(entry);
    bool const start = parsed && *parsed && (*parsed)->phase == ava::session::SubagentJobHistoryPhase::Start;
    if (start && fail_start)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "forced start history failure"));
    if (!start && fail_terminal)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "forced terminal history failure"));
    entries.push_back(std::move(entry));
    return {};
  }
};

void test_job_history_start_before_worker_and_immediate_completion_order()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.wait_for_terminal_before_start_returns = true;
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  auto sink = std::make_shared<CapturingHistorySink>();
  std::atomic<int> worker_runs{0};
  auto started = coordinator->start(
      ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "parent_history",
                                                  .mode = ava::agent::SubagentJobMode::Background,
                                                  .job = {.child_session_id = "child_history"},
                                                  .history_append = [sink](ava::session::SessionEntry entry) { return (*sink)(std::move(entry)); }},
      [&](auto const&) {
        ++worker_runs;
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "done", .stop_reason = "completed"};
      });
  expect(started && worker_runs == 1 && sink->entries.size() == 2, "immediate completion still persists start then terminal");
  if (sink->entries.size() != 2)
    return;
  auto first = ava::session::parse_subagent_job_history_entry(sink->entries[0]);
  auto second = ava::session::parse_subagent_job_history_entry(sink->entries[1]);
  expect(first && *first && (*first)->phase == ava::session::SubagentJobHistoryPhase::Start && second && *second &&
             (*second)->phase == ava::session::SubagentJobHistoryPhase::Terminal && (*second)->summary == "done",
         "start and terminal history cannot reorder for immediate completion");
}

void test_job_history_start_failure_does_not_launch_worker()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  auto sink = std::make_shared<CapturingHistorySink>();
  sink->fail_start = true;
  std::atomic<int> worker_runs{0};
  auto started = coordinator->start(
      ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "parent_fail_start",
                                                  .mode = ava::agent::SubagentJobMode::Background,
                                                  .job = {.child_session_id = "child_fail_start"},
                                                  .history_append = [sink](ava::session::SessionEntry entry) { return (*sink)(std::move(entry)); }},
      [&](auto const&) {
        ++worker_runs;
        return ava::agent::BackgroundJobCompletion{};
      });
  expect(!started && worker_runs == 0 && sink->entries.empty() &&
             ava::agent::subagent_publication_commit_state(started.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "start history failure does not launch a worker and stays unpublished");
}

void test_job_history_terminal_failure_keeps_worker_outcome()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.wait_for_terminal_before_start_returns = true;
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  auto sink = std::make_shared<CapturingHistorySink>();
  sink->fail_terminal = true;
  auto started = coordinator->start(
      ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "parent_fail_terminal",
                                                  .mode = ava::agent::SubagentJobMode::Background,
                                                  .job = {.child_session_id = "child_fail_terminal"},
                                                  .history_append = [sink](ava::session::SessionEntry entry) { return (*sink)(std::move(entry)); }},
      [](auto const&) {
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "kept", .stop_reason = "completed"};
      });
  expect(started && started->job.execution == ava::agent::SubagentExecutionState::Completed && started->job.summary == "kept" && sink->entries.size() == 1,
         "terminal history failure does not mislabel the worker outcome and leaves unmatched start history");
  auto projected = ava::session::project_subagent_job_history("parent_fail_terminal", sink->entries);
  expect(projected.size() == 1 && projected[0].unmatched_start && projected[0].record.execution == ava::session::SubagentJobHistoryExecution::Interrupted,
         "post-start terminal persistence failure yields conservative unknown history rather than false success");
}

void test_job_history_registry_start_failure_leaves_unmatched_start()
{
  ava::agent::SubagentCoordinatorOptions options;
  options.registry_options.thread_start_preflight = [] {
    return ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "forced thread start failure")));
  };
  auto coordinator = coordinator_with(std::move(options));
  if (!coordinator)
    return;
  auto sink = std::make_shared<CapturingHistorySink>();
  std::atomic<int> worker_runs{0};
  auto started = coordinator->start(
      ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "parent_thread_fail",
                                                  .mode = ava::agent::SubagentJobMode::Background,
                                                  .job = {.child_session_id = "child_thread_fail"},
                                                  .history_append = [sink](ava::session::SessionEntry entry) { return (*sink)(std::move(entry)); }},
      [&](auto const&) {
        ++worker_runs;
        return ava::agent::BackgroundJobCompletion{};
      });
  expect(!started && worker_runs == 0 && sink->entries.size() == 1 &&
             ava::agent::subagent_publication_commit_state(started.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "registry start failure after start history leaves unmatched start and does not launch the worker");
  auto projected = ava::session::project_subagent_job_history("parent_thread_fail", sink->entries);
  expect(projected.size() == 1 && projected[0].unmatched_start, "unmatched start history is interrupted/unknown after a failed launch");
}

struct BlockingTerminalHistorySink
{
  std::mutex mutex;
  std::condition_variable changed;
  bool entered_terminal = false;
  bool release_terminal = false;
  std::vector<ava::session::SessionEntry> entries;

  ava::core::VoidResult operator()(ava::session::SessionEntry entry)
  {
    auto parsed = ava::session::parse_subagent_job_history_entry(entry);
    bool const start = parsed && *parsed && (*parsed)->phase == ava::session::SubagentJobHistoryPhase::Start;
    if (start)
    {
      std::lock_guard lock(mutex);
      entries.push_back(std::move(entry));
      return {};
    }
    std::unique_lock lock(mutex);
    entered_terminal = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release_terminal; });
    entries.push_back(std::move(entry));
    return {};
  }

  bool wait_entered_terminal()
  {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered_terminal; });
  }

  void release()
  {
    std::lock_guard lock(mutex);
    release_terminal = true;
    changed.notify_all();
  }
};

void test_job_history_terminal_append_gates_automatic_delivery()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  auto history = std::make_shared<BlockingTerminalHistorySink>();
  auto worker = std::make_shared<BlockingWorker>();
  std::mutex notify_mutex;
  std::condition_variable notify_changed;
  std::size_t notifications = 0;
  coordinator->set_terminal_sink([&](ava::agent::SubagentCoordinatorJobSnapshot const&) {
    std::lock_guard lock(notify_mutex);
    ++notifications;
    notify_changed.notify_all();
  });
  auto started = coordinator->start(
      ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "parent_gate_delivery",
                                                  .mode = ava::agent::SubagentJobMode::Background,
                                                  .job = {.child_session_id = "child_gate_delivery"},
                                                  .history_append = [history](ava::session::SessionEntry entry) { return (*history)(std::move(entry)); }},
      [worker](auto const& context) { return worker->run(context, "gated"); });
  expect(started && worker->wait_started(), "delivery-gate fixture starts a live worker");
  if (!started)
    return;
  worker->finish();
  expect(history->wait_entered_terminal(), "terminal history append is entered before automatic delivery");
  auto pending_blocked = coordinator->pending_deliveries("parent_gate_delivery");
  auto snap_blocked = coordinator->snapshot("parent_gate_delivery", started->job.identity.job_id);
  auto raced_attempt = coordinator->record_delivery_attempt("parent_gate_delivery", started->job.identity.job_id, "attempt_early", "fingerprint_early");
  {
    std::lock_guard lock(notify_mutex);
    expect(notifications == 0 && pending_blocked && pending_blocked->empty() && snap_blocked &&
               snap_blocked->job.execution == ava::agent::SubagentExecutionState::Completed &&
               snap_blocked->job.delivery == ava::agent::SubagentDeliveryState::Pending && !raced_attempt,
           "blocked terminal history append shows Pending but does not expose automatic delivery");
  }
  coordinator->set_terminal_sink([&](ava::agent::SubagentCoordinatorJobSnapshot const&) {
    std::lock_guard lock(notify_mutex);
    ++notifications;
    notify_changed.notify_all();
  });
  {
    std::lock_guard lock(notify_mutex);
    expect(notifications == 0, "installing a terminal sink during blocked history append does not emit early delivery");
  }
  history->release();
  {
    std::unique_lock lock(notify_mutex);
    expect(notify_changed.wait_for(lock, std::chrono::seconds(2), [&] { return notifications == 1; }),
           "releasing terminal history append arms normal automatic delivery once");
  }
  auto pending_armed = coordinator->pending_deliveries("parent_gate_delivery");
  auto snap_armed = coordinator->snapshot("parent_gate_delivery", started->job.identity.job_id);
  expect(pending_armed && pending_armed->size() == 1 && snap_armed && snap_armed->job.delivery == ava::agent::SubagentDeliveryState::Pending &&
             snap_armed->job.execution == ava::agent::SubagentExecutionState::Completed && snap_armed->job.summary == "gated",
         "after history append, background delivery is Pending with the actual worker outcome");
}

void test_safe_bounds_attempt_validation_and_shutdown()
{
  auto coordinator = coordinator_with();
  if (!coordinator)
    return;
  std::string large(20U * 1024U, 'x');
  auto completed = complete_background(coordinator, "parent_bounds", "child_bounds", large);
  expect(completed.job.summary && completed.job.summary->size() == 16U * 1024U && completed.job.summary_truncated,
         "coordinator-owned normalization preserves the former bounded terminal summary contract");
  auto invalid = coordinator->record_delivery_attempt("parent_bounds", completed.job.identity.job_id, "../bad", "fingerprint");
  ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> last_attempt = completed;
  for (std::size_t index = 0; index < 64 && last_attempt; ++index)
    last_attempt = coordinator->record_delivery_attempt("parent_bounds", completed.job.identity.job_id, "attempt_" + std::to_string(index),
                                                        "fingerprint_" + std::to_string(index));
  auto overflow = coordinator->record_delivery_attempt("parent_bounds", completed.job.identity.job_id, "attempt_overflow", "fingerprint_overflow");
  auto invalid_start = coordinator->start_background(std::string(97, 'p'), {.child_session_id = "child_invalid"},
                                                     [](auto const&) { return ava::agent::BackgroundJobCompletion{}; });
  expect(!invalid && last_attempt && last_attempt->job.delivery_attempt_history.size() == 64 && !overflow && !invalid_start &&
             ava::agent::subagent_publication_commit_state(invalid_start.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
         "job and delivery identities plus attempt history retain coordinator-owned safety bounds");

  auto worker = std::make_shared<BlockingWorker>();
  auto running =
      coordinator->start_background("parent_shutdown", {.child_session_id = "child_shutdown"}, [worker](auto const& context) { return worker->run(context); });
  expect(running && worker->wait_started(), "shutdown fixture starts");
  coordinator->shutdown();
  auto rejected = coordinator->start_background("parent_shutdown", {.child_session_id = "child_after_shutdown"},
                                                [](auto const&) { return ava::agent::BackgroundJobCompletion{}; });
  {
    std::lock_guard lock(worker->mutex);
    expect(worker->canceled && !rejected &&
               ava::agent::subagent_publication_commit_state(rejected.error()) == ava::agent::SubagentPublicationCommitState::ProvenUnpublished,
           "shutdown rejects admission, cooperatively cancels, joins, and tags later failures proven unpublished");
  }
}

}  // namespace

void run_subagent_coordinator_tests()
{
  test_hidden_immediate_completion_publishes_once_after_visibility();
  test_process_locality_after_prior_coordinator_shutdown();
  test_owner_filter_wait_result_cancel_and_wire_contract();
  test_launch_display_survives_coordinator_lifecycle_and_owner_checks();
  test_exact_v1_job_snapshot_and_enum_strings();
  test_foreground_promotion_cancel_and_interaction_gate();
  test_concurrent_state_transition_matrices();
  test_real_foreground_interaction_gate_blocks_promotion();
  test_retention_registry_independence_and_hard_cap();
  test_retry_exhaustion_is_internal_and_settled();
  test_bounded_identity_collision_preserves_existing_job();
  test_identity_generator_faults_are_proven_unpublished();
  test_all_start_error_families_are_proven_unpublished();
  test_live_inspection_missing_mismatch_owner_and_prepublication();
  test_live_inspection_generation_freeze_and_lifecycle();
  test_live_inspection_deterministic_stale_inflight_race();
  test_live_inspection_monotonic_concurrent_publish_race();
  test_live_inspection_two_client_lost_response_and_sink();
  test_live_inspection_path_free_refresh_failures();
  test_live_inspection_eviction_and_freeze_failure();
  test_parent_maintenance_serializes_start_and_live_jobs();
  test_owner_checked_bounded_steering_fifo();
  test_safe_bounds_attempt_validation_and_shutdown();
  test_job_history_start_before_worker_and_immediate_completion_order();
  test_job_history_start_failure_does_not_launch_worker();
  test_job_history_terminal_failure_keeps_worker_outcome();
  test_job_history_registry_start_failure_leaves_unmatched_start();
  test_job_history_terminal_append_gates_automatic_delivery();
}
