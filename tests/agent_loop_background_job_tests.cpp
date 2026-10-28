#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/agent/agent_loop.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace {
ava::core::Result<ava::agent::BackgroundJobSnapshot> wait_for_terminal(ava::agent::BackgroundJobRegistry& registry, std::string const& id)
{
  auto const deadline = ava::tests::now_plus_seconds(5);
  return registry.wait(id, std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
}
}  // namespace

void test_background_job_registry_worker_exception_marks_failed()
{
  ava::agent::BackgroundJobRegistry registry(ava::agent::BackgroundJobRegistryOptions{.max_running_jobs = 1, .max_retained_finished_jobs = 4});
  auto started = registry.start(ava::agent::BackgroundJobStartOptions{.title = "throws"},
                                [](ava::agent::BackgroundJobContext const&) -> ava::agent::BackgroundJobCompletion { throw std::runtime_error("boom"); });
  expect(started.has_value(), "background registry starts worker that throws");
  if (started)
  {
    auto failed = wait_for_terminal(registry, started->job_id);
    if (!ava::tests::require(failed.has_value(), "throwing worker reaches a terminal snapshot"))
      return;
    ava::tests::expect_all("background registry records and retains thrown worker failures",
                           {{"failed state", failed->state == ava::agent::BackgroundJobState::Failed},
                            {"exception detail", failed->error && failed->error->find("boom") != std::string::npos}});
    // Starting more work may reap finished workers internally; no explicit
    // join is required before capacity can be reused after an exception.
    auto replacement = registry.start(ava::agent::BackgroundJobStartOptions{.title = "after exception"}, [](ava::agent::BackgroundJobContext const&) {
      return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "replacement", .stop_reason = ""};
    });
    if (!ava::tests::require(replacement.has_value(), "worker exception permits reuse of the only running slot without an explicit join"))
      return;
    auto completed = wait_for_terminal(registry, replacement->job_id);
    if (!ava::tests::require(completed.has_value(), "replacement worker reaches a terminal snapshot"))
      return;
    ava::tests::expect_eq(completed->final_text, std::string("replacement"), "replacement worker executes after the exception");
    auto const joined = registry.join_finished();
    auto retained = registry.wait(started->job_id, std::chrono::milliseconds(0));
    // start() already joined the failed worker; only the replacement remains.
    ava::tests::expect_all("joining the replacement keeps the failed job queryable",
                           {{"replacement worker joined", joined == 1},
                            {"no worker joined twice", registry.join_finished() == 0},
                            {"failed snapshot retained", retained && retained->state == ava::agent::BackgroundJobState::Failed}});
  }
}

void test_background_job_registry_enforces_running_limit()
{
  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
  };
  auto state = std::make_shared<State>();
  ava::agent::BackgroundJobRegistry registry(ava::agent::BackgroundJobRegistryOptions{.max_running_jobs = 1, .max_retained_finished_jobs = 4});
  auto blocking_worker = [state](ava::agent::BackgroundJobContext const& context) {
    std::unique_lock lock(state->mutex);
    std::stop_callback notify_stop(context.stop_token, [&] { state->changed.notify_all(); });
    state->started = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return context.stop_token.stop_requested(); });
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Canceled, .final_text = "", .stop_reason = "canceled"};
  };
  auto first = registry.start(ava::agent::BackgroundJobStartOptions{.title = "first"}, blocking_worker);
  expect(first.has_value(), "background registry starts first job under running limit");
  {
    std::unique_lock lock(state->mutex);
    expect(state->changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return state->started; }),
           "running-limit test worker starts before second job attempt");
  }
  auto second = registry.start(ava::agent::BackgroundJobStartOptions{.title = "second"}, [](ava::agent::BackgroundJobContext const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "unexpected", .stop_reason = "completed"};
  });
  expect(!second && second.error().category() == ava::core::ErrorCategory::Tool &&
             std::ranges::any_of(second.error().context(), [](auto const& field) { return field.key == "max_running_jobs" && field.value == "1"; }),
         "background registry rejects jobs above the configured running limit with typed capacity context");
  if (first)
  {
    static_cast<void>(registry.cancel(first->job_id));
    auto canceled = wait_for_terminal(registry, first->job_id);
    expect(canceled && canceled->state == ava::agent::BackgroundJobState::Canceled, "background registry frees running capacity after cancellation");
  }
  registry.join_finished();
  auto third = registry.start(ava::agent::BackgroundJobStartOptions{.title = "third"}, [](ava::agent::BackgroundJobContext const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "third done", .stop_reason = "completed"};
  });
  expect(third.has_value(), "background registry accepts a new job after running job completes");
  if (third)
  {
    auto completed = wait_for_terminal(registry, third->job_id);
    expect(completed && completed->final_text == "third done", "background registry completes job after running limit frees capacity");
  }
  registry.join_finished();
}

void test_background_job_registry_coerces_non_terminal_completion_to_failed()
{
  ava::agent::BackgroundJobRegistry registry;
  auto started = registry.start(ava::agent::BackgroundJobStartOptions{.title = "bad completion"}, [](ava::agent::BackgroundJobContext const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Running, .final_text = "bad", .stop_reason = "running"};
  });
  expect(started.has_value(), "background registry starts worker with invalid completion state");
  if (started)
  {
    auto failed = wait_for_terminal(registry, started->job_id);
    expect(failed && failed->state == ava::agent::BackgroundJobState::Failed && failed->error && failed->error->find("non-terminal") != std::string::npos,
           "background registry coerces non-terminal worker completions to failed snapshots");
    registry.join_finished();
  }
}

void test_background_job_registry_bounds_snapshot_text()
{
  ava::agent::BackgroundJobRegistry registry(
      ava::agent::BackgroundJobRegistryOptions{.max_running_jobs = 4, .max_retained_finished_jobs = 4, .max_description_bytes = 5, .max_final_text_bytes = 7});
  auto const multi_byte_suffix = std::string("\xE2\x82\xAC", 3) + "tail";
  auto started = registry.start(ava::agent::BackgroundJobStartOptions{.title = "bounded", .description = std::string("abcd") + multi_byte_suffix},
                                [](ava::agent::BackgroundJobContext const&) {
                                  return ava::agent::BackgroundJobCompletion{
                                      .state = ava::agent::BackgroundJobState::Completed,
                                      .final_text = std::string("abcdef") + std::string("\xE2\x82\xAC", 3) + "tail",
                                      .stop_reason = "",
                                  };
                                });
  expect(started && started->description == "abcd" && started->description_truncated,
         "background registry truncates oversized job descriptions without splitting UTF-8 codepoints");
  if (started)
  {
    auto completed = wait_for_terminal(registry, started->job_id);
    expect(completed && completed->final_text == "abcdef" && completed->final_text_truncated,
           "background registry truncates oversized final text without splitting UTF-8 codepoints");
    registry.join_finished();
  }
}

void test_background_job_registry_normalizes_terminal_completion_fields()
{
  ava::agent::BackgroundJobRegistry registry;
  auto completed_with_error =
      registry.start(ava::agent::BackgroundJobStartOptions{.title = "completed with error"}, [](ava::agent::BackgroundJobContext const&) {
        auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "ignored completion error");
        return ava::agent::BackgroundJobCompletion{
            .state = ava::agent::BackgroundJobState::Completed,
            .final_text = "done",
            .stop_reason = "",
            .error = std::move(error),
        };
      });
  auto failed_without_error =
      registry.start(ava::agent::BackgroundJobStartOptions{.title = "failed without error"}, [](ava::agent::BackgroundJobContext const&) {
        return ava::agent::BackgroundJobCompletion{
            .state = ava::agent::BackgroundJobState::Failed,
            .final_text = "should disappear",
            .stop_reason = "",
        };
      });
  auto canceled_with_error = registry.start(ava::agent::BackgroundJobStartOptions{.title = "canceled with error"}, [](ava::agent::BackgroundJobContext const&) {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "ignored cancel error");
    return ava::agent::BackgroundJobCompletion{
        .state = ava::agent::BackgroundJobState::Canceled,
        .final_text = "should disappear",
        .stop_reason = "",
        .error = std::move(error),
    };
  });

  expect(completed_with_error && failed_without_error && canceled_with_error, "background registry starts terminal normalization workers");
  if (completed_with_error)
  {
    auto completed = wait_for_terminal(registry, completed_with_error->job_id);
    expect(completed && completed->state == ava::agent::BackgroundJobState::Completed && completed->final_text == "done" &&
               completed->stop_reason == "completed" && !completed->error,
           "background registry clears completed-job errors and defaults stop reasons");
  }
  if (failed_without_error)
  {
    auto failed = wait_for_terminal(registry, failed_without_error->job_id);
    expect(failed && failed->state == ava::agent::BackgroundJobState::Failed && failed->final_text.empty() && failed->stop_reason == "failed" &&
               failed->error && failed->error->find("without an error") != std::string::npos,
           "background registry synthesizes failed-job errors and clears final text");
  }
  if (canceled_with_error)
  {
    auto canceled = wait_for_terminal(registry, canceled_with_error->job_id);
    expect(canceled && canceled->state == ava::agent::BackgroundJobState::Canceled && canceled->final_text.empty() && canceled->stop_reason == "canceled" &&
               !canceled->error,
           "background registry clears canceled-job errors and final text");
  }
  registry.join_finished();
}

void test_background_job_registry_join_finished_is_idempotent_across_threads()
{
  ava::agent::BackgroundJobRegistry registry;
  auto started = registry.start(ava::agent::BackgroundJobStartOptions{.title = "concurrent join"}, [](ava::agent::BackgroundJobContext const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "done", .stop_reason = ""};
  });
  expect(started.has_value(), "background registry starts concurrent join worker");
  if (!started)
    return;
  auto completed = wait_for_terminal(registry, started->job_id);
  expect(completed && completed->state == ava::agent::BackgroundJobState::Completed, "background registry has a terminal job before concurrent join");

  std::atomic_bool threw = false;
  std::atomic_size_t joined_total = 0;
  std::barrier start_together(2);
  auto joiner = [&] {
    start_together.arrive_and_wait();
    try
    {
      joined_total.fetch_add(registry.join_finished(), std::memory_order_relaxed);
    }
    catch (...)
    {
      threw = true;
    }
  };
  std::thread first(joiner);
  std::thread second(joiner);
  first.join();
  second.join();
  ava::tests::expect_all("join_finished is idempotent across simultaneously released callers",
                         {{"no exception", !threw.load()}, {"exactly one join", joined_total.load(std::memory_order_relaxed) == 1}});
}

void test_background_job_registry_prunes_retained_finished_jobs()
{
  ava::agent::BackgroundJobRegistry registry(ava::agent::BackgroundJobRegistryOptions{.max_running_jobs = 4, .max_retained_finished_jobs = 1});
  auto first = registry.start(
      ava::agent::BackgroundJobStartOptions{.job_id = "job_retention_a", .title = "first retained"}, [](ava::agent::BackgroundJobContext const&) {
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "first", .stop_reason = ""};
      });
  if (!ava::tests::require(first.has_value(), "retention fixture starts the older worker"))
    return;
  auto older = wait_for_terminal(registry, first->job_id);
  if (!ava::tests::require(older && older->state == ava::agent::BackgroundJobState::Completed, "older retained job finishes before newer job starts"))
    return;
  // Completion timestamps have second precision; explicit ordered IDs make
  // the documented tie-break deterministic when both finish in the same second.
  auto second = registry.start(
      ava::agent::BackgroundJobStartOptions{.job_id = "job_retention_z", .title = "second retained"}, [](ava::agent::BackgroundJobContext const&) {
        return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "second", .stop_reason = ""};
      });
  if (!ava::tests::require(second.has_value(), "retention fixture starts the newer worker"))
    return;
  auto newer = wait_for_terminal(registry, second->job_id);
  if (!ava::tests::require(newer && newer->state == ava::agent::BackgroundJobState::Completed, "newer retained job finishes"))
    return;
  registry.join_finished();
  auto const retained = registry.snapshot();
  if (!ava::tests::require(retained.size() == 1, "retention keeps exactly one finished snapshot, rather than deleting every job"))
    return;
  ava::tests::expect_eq(retained.front().job_id, second->job_id, "retention keeps the newest completed job");
}

void test_background_job_registry_destructor_stops_running_jobs()
{
  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool canceled = false;
  };
  auto state = std::make_shared<State>();

  {
    ava::agent::BackgroundJobRegistry registry;
    auto started = registry.start(ava::agent::BackgroundJobStartOptions{.title = "destructor stop"}, [state](ava::agent::BackgroundJobContext const& context) {
      std::unique_lock lock(state->mutex);
      std::stop_callback notify_stop(context.stop_token, [&] { state->changed.notify_all(); });
      state->started = true;
      state->changed.notify_all();
      state->changed.wait(lock, [&] { return context.stop_token.stop_requested(); });
      state->canceled = true;
      state->changed.notify_all();
      return ava::agent::BackgroundJobCompletion{
          .state = ava::agent::BackgroundJobState::Canceled,
          .final_text = "",
          .stop_reason = "canceled",
      };
    });
    expect(started.has_value(), "background registry starts destructor stop worker");
    std::unique_lock lock(state->mutex);
    expect(state->changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return state->started; }),
           "background registry destructor test worker starts before scope exit");
  }

  std::lock_guard lock(state->mutex);
  expect(state->canceled, "background registry destructor requests stop and joins running jobs");
}

void test_background_job_registry_stop_callbacks_can_reenter_cancel()
{
  ava::agent::BackgroundJobRegistry registry;
  std::mutex mutex;
  std::condition_variable changed;
  bool ready = false;
  std::atomic_bool reentered = false;
  auto started = registry.start({.title = "reentrant cancellation"}, [&](ava::agent::BackgroundJobContext const& context) {
    std::stop_callback callback(context.stop_token, [&] {
      auto const canceled = registry.cancel(context.job_id);
      {
        std::lock_guard lock(mutex);
        reentered = canceled.has_value();
      }
      changed.notify_all();
    });
    std::unique_lock lock(mutex);
    ready = true;
    changed.notify_all();
    changed.wait(lock, [&] { return reentered.load(); });
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Canceled, .final_text = {}, .stop_reason = {}};
  });
  if (!ava::tests::require(started.has_value(), "reentrant cancellation worker starts"))
    return;
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return ready; }), "stop callback is registered before cancellation");
  }
  auto canceled = registry.cancel(started->job_id);
  expect(canceled.has_value() && reentered.load(), "stop callbacks can cancel the same job without taking a held thread mutex");
  registry.shutdown();
}

void test_background_job_registry_shutdown_racing_start_preflight()
{
  std::mutex mutex;
  std::condition_variable changed;
  bool preflight_entered = false;
  bool release_preflight = false;
  ava::agent::BackgroundJobRegistryOptions options;
  options.thread_start_preflight = [&]() -> ava::core::VoidResult {
    std::unique_lock lock(mutex);
    preflight_entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release_preflight; });
    return {};
  };
  ava::agent::BackgroundJobRegistry registry(std::move(options));
  std::atomic_bool launched = false;
  std::atomic_bool rejected = false;
  auto starter = ava::core::JoinThread::create("registry_start", [&] {
    auto result = registry.start({.title = "shutdown during admission"}, [&](auto const&) {
      launched = true;
      return ava::agent::BackgroundJobCompletion{};
    });
    rejected = !result;
  });
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return preflight_entered; }), "start pauses at preflight before shutdown");
  }
  registry.shutdown();
  {
    std::lock_guard lock(mutex);
    release_preflight = true;
  }
  changed.notify_all();
  starter.join();
  expect(rejected.load() && !launched.load(), "shutdown rejects a preflight-paused start instead of launching an unjoined worker afterwards");
}

void test_background_job_registry_preflight_exception_does_not_publish()
{
  for (bool const standard_exception : {true, false})
  {
    std::size_t attempts = 0;
    ava::agent::BackgroundJobRegistryOptions options;
    options.max_running_jobs = 1;
    options.thread_start_preflight = [&]() -> ava::core::VoidResult {
      if (++attempts != 1)
        return {};
      if (standard_exception)
        throw std::runtime_error("preflight failed");
      throw 7;
    };
    ava::agent::BackgroundJobRegistry registry(std::move(options));
    auto worker = [](auto const&) { return ava::agent::BackgroundJobCompletion{}; };
    auto failed = registry.start({.title = "preflight failure"}, worker);
    expect(!failed && registry.snapshot().empty(), "throwing preflight returns an error without publishing a running record");
    if (!failed && standard_exception)
      expect(failed.error().format().find("preflight failed") != std::string::npos, "preflight preserves exception context");
    auto replacement = registry.start({.title = "replacement"}, worker);
    expect(replacement.has_value(), "preflight exception does not consume running capacity");
    if (replacement)
      expect(wait_for_terminal(registry, replacement->job_id).has_value(), "replacement worker finishes after preflight failure");
  }
}
