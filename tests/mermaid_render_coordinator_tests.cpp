#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/app/mermaid_render_coordinator.h"
#include "ava/app/tui/mermaid_tui_bridge.h"
#include "ava/app/tui/tui_display_settings.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef AVA_FAKE_MERMAID_HELPER_PATH
#define AVA_FAKE_MERMAID_HELPER_PATH ""
#endif

namespace {

using namespace std::chrono_literals;

ava::config::XdgPaths display_paths(std::filesystem::path const& root)
{
  auto const config = root / "config";
  std::filesystem::create_directories(config / "ava");
  return {.config_home = config,
          .state_home = root / "state",
          .data_home = root / "data",
          .ava_config_dir = config / "ava",
          .ava_state_dir = root / "state" / "ava",
          .auth_file = config / "ava" / "auth.json",
          .compaction_file = config / "ava" / "compaction.json",
          .global_agents_file = config / "ava" / "AGENTS.md",
          .models_file = config / "ava" / "models.json",
          .providers_file = config / "ava" / "providers.json",
          .prompts_dir = config / "ava" / "prompts",
          .sessions_dir = root / "state" / "ava" / "sessions"};
}

void write_file(std::filesystem::path const& path, std::string_view content)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.flush();
}

std::string read_file(std::filesystem::path const& path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string json_argv(std::vector<std::string> const& arguments)
{
  std::string result = "[";
  for (std::size_t index = 0; index < arguments.size(); ++index)
  {
    if (index != 0)
      result += ',';
    result += '"';
    result += arguments[index];
    result += '"';
  }
  result += ']';
  return result;
}

ava::app::MermaidRenderConfiguration helper_config(std::uint64_t epoch, std::vector<std::string> arguments)
{
  std::vector<std::string> argv{AVA_FAKE_MERMAID_HELPER_PATH};
  argv.insert(argv.end(), std::make_move_iterator(arguments.begin()), std::make_move_iterator(arguments.end()));
  return {.epoch = epoch, .enabled = true, .argv = std::move(argv)};
}

std::optional<ava::app::MermaidRenderCompletion> await_completion(ava::app::MermaidRenderCoordinator& coordinator, std::uint64_t identity,
                                                                  std::chrono::milliseconds timeout = 4s)
{
  auto completions = coordinator.take_completions();
  for (auto& completion : completions)
  {
    if (completion.identity == identity)
      return std::move(completion);
  }
  if (!coordinator.wait_until_idle(timeout))
    return std::nullopt;
  completions = coordinator.take_completions();
  for (auto& completion : completions)
  {
    if (completion.identity == identity)
      return std::move(completion);
  }
  return std::nullopt;
}

std::optional<ava::app::MermaidRenderCompletion> render(ava::app::MermaidRenderCoordinator& coordinator, std::uint64_t identity, std::uint64_t epoch,
                                                        std::string source = "graph TD; A-->B")
{
  auto const queued = coordinator.enqueue({.identity = identity, .config_epoch = epoch, .source = std::move(source)});
  if (queued == ava::app::MermaidEnqueueResult::QueueFull || queued == ava::app::MermaidEnqueueResult::StaleEpoch)
    return std::nullopt;
  return await_completion(coordinator, identity);
}

void test_display_settings_mermaid_schema_and_preservation()
{
  auto const root = create_empty_root("mermaid-display-settings");
  auto const paths = display_paths(root);
  auto const display = ava::app::tui_display_settings_file(paths);

  auto missing = ava::app::load_tui_display_settings(paths);
  expect(missing && !missing->mermaid.enabled && missing->mermaid.argv.empty(), "missing display.json defaults Mermaid rendering to disabled");

  write_file(display, R"({"theme":"light","mermaid":{"argv":["/not/installed/helper","--flag"],"future_nested":{"x":1}},"future_top":[1,2]})");
  auto disabled = ava::app::load_tui_display_settings(paths);
  expect(disabled && !disabled->mermaid.enabled && !disabled->mermaid.enabled_configured && disabled->mermaid.argv_configured &&
             disabled->mermaid.argv.size() == 2 && disabled->mermaid.argv.front() == "/not/installed/helper",
         "missing mermaid.enabled is disabled and helper existence is not checked while parsing");

  auto stored = ava::app::store_tui_show_images_setting(paths, false);
  auto preserved = ava::app::load_display_settings_document(paths);
  auto const preserved_text = read_file(display);
  expect(stored && preserved && preserved->mermaid && preserved->mermaid->unknown_fields.size() == 1 && preserved->unknown_fields.size() == 1 &&
             preserved_text.find("future_nested") != std::string::npos && preserved_text.find("future_top") != std::string::npos,
         "display field updates preserve unknown top-level and Mermaid nested fields");

  write_file(display, R"({"mermaid":{"enabled":true,"argv":["/not/installed/helper","literal"]}})");
  auto enabled = ava::app::load_tui_display_settings(paths);
  auto good_watch = ava::app::load_tui_display_settings_watch_state(paths);
  auto render_configuration =
      enabled ? ava::app::mermaid_render_configuration_from_display_settings(enabled->mermaid, 17) : ava::app::MermaidRenderConfiguration{};
  expect(enabled && enabled->mermaid.enabled && enabled->mermaid.argv.size() == 2 && good_watch && good_watch->mermaid.enabled &&
             render_configuration.epoch == 17 && render_configuration.enabled && render_configuration.argv == enabled->mermaid.argv,
         "enabled Mermaid settings expose a validated absolute argv through load, watch, and coordinator configuration seams");

  auto const last_good = enabled ? enabled->mermaid : ava::app::MermaidDisplaySettings{};
  write_file(display, R"({"mermaid":{"enabled":true}})");
  auto invalid_reload = ava::app::load_tui_display_settings(paths);
  expect(!invalid_reload && last_good.enabled && last_good.argv.size() == 2,
         "an invalid display reload fails without mutating the caller-owned last known-good Mermaid settings");

  std::vector<std::string> too_many(33, "x");
  too_many.front() = "/helper";
  std::vector<std::string> too_large_total(5, std::string(4096, 'x'));
  too_large_total.front().replace(0, 1, "/");
  std::vector<std::string> invalid_documents{
      R"({"mermaid":true})",
      R"({"mermaid":{"enabled":"yes","argv":["/helper"]}})",
      R"({"mermaid":{"enabled":true,"argv":[]}})",
      R"({"mermaid":{"enabled":true,"argv":["relative"]}})",
      std::string("{\"mermaid\":{\"enabled\":true,\"argv\":") + json_argv(too_many) + "}}",
      std::string("{\"mermaid\":{\"enabled\":true,\"argv\":[\"/") + std::string(4096, 'x') + "\"]}}",
      std::string("{\"mermaid\":{\"enabled\":true,\"argv\":") + json_argv(too_large_total) + "}}",
      R"({"mermaid":{"enabled":true,"argv":["/helper",1]}})",
  };
  bool all_rejected = true;
  for (auto const& invalid : invalid_documents)
  {
    write_file(display, invalid);
    all_rejected = all_rejected && !ava::app::load_tui_display_settings(paths);
  }
  expect(all_rejected, "Mermaid settings reject wrong types, empty/relative argv, count, per-argument, and aggregate byte overflows");

  write_file(display, R"({"mermaid":{"enabled":false,"argv":[]}})");
  auto explicitly_disabled = ava::app::load_tui_display_settings(paths);
  expect(explicitly_disabled && !explicitly_disabled->mermaid.enabled && explicitly_disabled->mermaid.argv.empty(),
         "explicitly disabled Mermaid settings may retain an empty argv");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void test_no_shell_environment_and_descriptor_launch()
{
  auto const root = create_empty_root("mermaid-process-boundaries");
  auto const injected_path = root / "shell-injection";
  auto const metacharacter = std::string("; touch ") + injected_path.string() + " $(printf unsafe)";
  auto coordinator = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"argument", metacharacter}));
  if (!coordinator)
  {
    expect(false, coordinator.error().format());
    return;
  }
  auto metacharacter_result = render(**coordinator, 1, 1);
  expect(metacharacter_result && metacharacter_result->outcome == ava::app::MermaidRenderOutcome::Accepted && metacharacter_result->text == metacharacter &&
             !std::filesystem::exists(injected_path),
         "Mermaid helper argv metacharacters are passed literally without a shell");

  {
    ScopedEnvVar home("HOME", "/private/home");
    ScopedEnvVar xdg("XDG_CONFIG_HOME", "/private/xdg");
    ScopedEnvVar provider("OPENAI_API_KEY", "private-provider-token");
    ScopedEnvVar ava_secret("AVA_PRIVATE_TEST_SECRET", "private-ava-token");
    auto reconfigured = (*coordinator)->reconfigure(helper_config(2, {"environment"}));
    auto environment = render(**coordinator, 2, 2);
    std::string const expected = "AVA_MERMAID_PROTOCOL=1\nLANG=C.UTF-8\nLC_ALL=C.UTF-8\nNO_COLOR=1\nPATH=/usr/local/bin:/usr/bin:/bin\nPWD=/\nTERM=dumb";
    expect(reconfigured && environment && environment->outcome == ava::app::MermaidRenderOutcome::Accepted && environment->text == expected &&
               environment->text.find("HOME") == std::string::npos && environment->text.find("TOKEN") == std::string::npos,
           "Mermaid helper receives only the fixed synthetic environment and cwd metadata");
  }

  auto cwd_configured = (*coordinator)->reconfigure(helper_config(3, {"cwd"}));
  auto cwd = render(**coordinator, 3, 3);
  expect(cwd_configured && cwd && cwd->outcome == ava::app::MermaidRenderOutcome::Accepted && cwd->text == "/", "Mermaid helpers execute with fixed cwd /");

  auto const invalid_executable = root / "invalid-executable";
  write_file(invalid_executable, "not an executable image\n");
  static_cast<void>(::chmod(invalid_executable.c_str(), 0700));
  auto invalid_config = ava::app::MermaidRenderConfiguration{.epoch = 4, .enabled = true, .argv = {invalid_executable.string()}};
  auto invalid_reconfigured = (*coordinator)->reconfigure(std::move(invalid_config));
  auto launch_failed = render(**coordinator, 4, 4);

  auto const symlink_executable = root / "helper-symlink";
  std::error_code symlink_error;
  std::filesystem::create_symlink(AVA_FAKE_MERMAID_HELPER_PATH, symlink_executable, symlink_error);
  auto symlink_configured = (*coordinator)->reconfigure({.epoch = 5, .enabled = true, .argv = {symlink_executable.string()}});
  auto symlink_rejected = render(**coordinator, 5, 5);
  expect(invalid_reconfigured && launch_failed && launch_failed->outcome == ava::app::MermaidRenderOutcome::LaunchFailed && !symlink_error &&
             symlink_configured && symlink_rejected && symlink_rejected->outcome == ava::app::MermaidRenderOutcome::LaunchFailed,
         "descriptor execution rejects invalid executable images and nofollow symlinks as typed launch failures");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void test_typed_process_and_output_failures()
{
  auto coordinator = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"echo"}));
  if (!coordinator)
  {
    expect(false, coordinator.error().format());
    return;
  }
  auto accepted = render(**coordinator, 1, 1, "graph TD\nA-->B\n");
  expect(accepted && accepted->outcome == ava::app::MermaidRenderOutcome::Accepted && accepted->text == "graph TD\nA-->B",
         "accepted helper output removes at most one terminal LF");

  struct FailureCase
  {
    std::vector<std::string> argv;
    ava::app::MermaidRenderOutcome outcome;
  };
  std::vector<FailureCase> cases{
      {{"exit", "9"}, ava::app::MermaidRenderOutcome::NonzeroExit},
      {{"signal"}, ava::app::MermaidRenderOutcome::Signaled},
      {{"output-size", std::to_string(ava::app::kMaxMermaidOutputBytes + 1)}, ava::app::MermaidRenderOutcome::OutputOverflow},
      {{"invalid-utf8"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"nul"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"tab"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"carriage-return"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"escape"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"del"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"c1"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"bidi"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"long-line"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
      {{"too-many-lines"}, ava::app::MermaidRenderOutcome::UnsafeOutput},
  };
  bool all_typed = true;
  std::uint64_t identity = 10;
  std::uint64_t epoch = 2;
  for (auto const& failure : cases)
  {
    auto changed = (*coordinator)->reconfigure(helper_config(epoch, failure.argv));
    auto completion = render(**coordinator, identity, epoch);
    all_typed = all_typed && changed && completion && completion->outcome == failure.outcome && completion->text.empty();
    ++identity;
    ++epoch;
  }
  expect(all_typed, "nonzero, signal, overflow, invalid UTF-8, controls, bidi, line, and line-count failures are typed and payload-free");

  auto missing = (*coordinator)->reconfigure({.epoch = epoch, .enabled = true, .argv = {"/definitely/missing/ava-mermaid-helper"}});
  auto missing_completion = render(**coordinator, identity, epoch);
  expect(missing && missing_completion && missing_completion->outcome == ava::app::MermaidRenderOutcome::MissingHelper && missing_completion->text.empty(),
         "a missing helper produces a typed payload-free fallback completion");
}

void test_timeout_group_cleanup_cancel_reconfigure_and_shutdown()
{
  auto const root = create_empty_root("mermaid-lifecycle");
  auto const child_pid_file = root / "child.pid";
  auto coordinator = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"group-timeout", child_pid_file.string()}));
  if (!coordinator)
  {
    expect(false, coordinator.error().format());
    return;
  }
  auto timed_out = render(**coordinator, 1, 1);
  auto const child_pid_text = read_file(child_pid_file);
  auto const child_pid = child_pid_text.empty() ? -1 : static_cast<pid_t>(std::stoll(child_pid_text));
  errno = 0;
  auto const child_missing = child_pid > 1 && ::kill(child_pid, 0) != 0 && errno == ESRCH;
  expect(timed_out && timed_out->outcome == ava::app::MermaidRenderOutcome::Timeout && child_missing,
         "one absolute timeout terminates the verified helper process group and its ordinary descendant");

  auto cancel_configured = (*coordinator)->reconfigure(helper_config(2, {"timeout"}));
  auto queued = (*coordinator)->enqueue({.identity = 2, .config_epoch = 2, .source = "cancel me"});
  auto started = (*coordinator)->wait_for_in_flight(1s);
  auto canceled = (*coordinator)->cancel(2, 2);
  auto canceled_completion = await_completion(**coordinator, 2, 1s);
  expect(cancel_configured && queued == ava::app::MermaidEnqueueResult::Queued && started && canceled && canceled_completion &&
             canceled_completion->outcome == ava::app::MermaidRenderOutcome::Canceled && (*coordinator)->wait_until_idle(1s),
         "explicit cancellation promptly tears down active helper work and publishes a typed fallback");

  auto old_configured = (*coordinator)->reconfigure(helper_config(3, {"timeout"}));
  static_cast<void>((*coordinator)->enqueue({.identity = 3, .config_epoch = 3, .source = "stale source"}));
  auto old_started = (*coordinator)->wait_for_in_flight(1s);
  auto new_configured = (*coordinator)->reconfigure(helper_config(4, {"echo"}));
  auto current = render(**coordinator, 4, 4, "stale source");
  auto remaining = (*coordinator)->take_completions();
  auto const stale_published = std::ranges::any_of(remaining, [](auto const& completion) { return completion.identity == 3; });
  expect(old_configured && old_started && new_configured && current && current->outcome == ava::app::MermaidRenderOutcome::Accepted &&
             current->text == "stale source" && !stale_published,
         "epoch changes discard stale work and cache state, terminate active work, and admit the new configuration");

  auto shutdown_configured = (*coordinator)->reconfigure(helper_config(5, {"timeout"}));
  static_cast<void>((*coordinator)->enqueue({.identity = 5, .config_epoch = 5, .source = "shutdown"}));
  auto shutdown_started = (*coordinator)->wait_for_in_flight(1s);
  auto const shutdown_begin = std::chrono::steady_clock::now();
  (*coordinator)->shutdown();
  auto const shutdown_elapsed = std::chrono::steady_clock::now() - shutdown_begin;
  expect(shutdown_configured && shutdown_started && shutdown_elapsed < 1s && (*coordinator)->stats().shutting_down,
         "shutdown cancels, joins, and reaps active helper work within a finite cleanup budget");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void test_queue_deduplication_cache_and_bounds()
{
  auto coordinator = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"timeout"}));
  if (!coordinator)
  {
    expect(false, coordinator.error().format());
    return;
  }
  static_cast<void>((*coordinator)->enqueue({.identity = 1, .config_epoch = 1, .source = "active"}));
  auto started = (*coordinator)->wait_for_in_flight(1s);
  auto attached_in_flight = (*coordinator)->enqueue({.identity = 2, .config_epoch = 1, .source = "active"});
  auto queued = (*coordinator)->enqueue({.identity = 3, .config_epoch = 1, .source = "queued"});
  auto attached_queued = (*coordinator)->enqueue({.identity = 4, .config_epoch = 1, .source = "queued"});
  auto dedupe_stats = (*coordinator)->stats();
  expect(started && attached_in_flight == ava::app::MermaidEnqueueResult::AttachedToExisting && queued == ava::app::MermaidEnqueueResult::Queued &&
             attached_queued == ava::app::MermaidEnqueueResult::AttachedToExisting && dedupe_stats.queued_requests == 1 && dedupe_stats.pending_identities == 4,
         "identical in-flight and queued sources deduplicate helper work while retaining opaque request identities");
  static_cast<void>((*coordinator)->reconfigure(helper_config(2, {"timeout"})));
  static_cast<void>((*coordinator)->enqueue({.identity = 100, .config_epoch = 2, .source = "active-bound"}));
  auto bound_started = (*coordinator)->wait_for_in_flight(1s);
  bool accepted_to_bound = true;
  for (std::uint64_t identity = 101; identity < 132; ++identity)
  {
    accepted_to_bound =
        accepted_to_bound && (*coordinator)->enqueue({.identity = identity, .config_epoch = 2, .source = "queued-" + std::to_string(identity)}) ==
                                 ava::app::MermaidEnqueueResult::Queued;
  }
  auto const full = (*coordinator)->enqueue({.identity = 132, .config_epoch = 2, .source = "one-too-many"});
  expect(bound_started && accepted_to_bound && full == ava::app::MermaidEnqueueResult::QueueFull &&
             (*coordinator)->stats().pending_identities == ava::app::kMaxMermaidQueuedRequests,
         "the one-worker queue admits at most 32 undrained unique request identities");

  auto const root = create_empty_root("mermaid-cache");
  auto const count_file = root / "count";
  auto cache_configured = (*coordinator)->reconfigure(helper_config(3, {"--count", count_file.string(), "echo"}));
  auto first = render(**coordinator, 200, 3, "same source");
  auto second = render(**coordinator, 201, 3, "same source");
  auto const positive_count = read_file(count_file).size();
  auto negative_configured = (*coordinator)->reconfigure(helper_config(4, {"--count", count_file.string(), "exit", "7"}));
  auto first_negative = render(**coordinator, 202, 4, "bad source");
  auto second_negative = render(**coordinator, 203, 4, "bad source");
  auto const total_count = read_file(count_file).size();
  expect(cache_configured && first && second && first->outcome == ava::app::MermaidRenderOutcome::Accepted && second->from_cache && positive_count == 1 &&
             negative_configured && first_negative && second_negative && first_negative->outcome == ava::app::MermaidRenderOutcome::NonzeroExit &&
             second_negative->outcome == ava::app::MermaidRenderOutcome::NonzeroExit && second_negative->from_cache && total_count == 2,
         "accepted and negative active-epoch cache entries suppress repeated helper launches");

  auto entry_cache = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"echo"}));
  bool entry_runs = entry_cache.has_value();
  if (entry_cache)
  {
    for (std::uint64_t identity = 1; identity <= 129; ++identity)
    {
      auto completion = render(**entry_cache, identity, 1, "cache-source-" + std::to_string(identity));
      entry_runs = entry_runs && completion && completion->outcome == ava::app::MermaidRenderOutcome::Accepted;
    }
  }
  auto const entry_stats = entry_cache ? (*entry_cache)->stats() : ava::app::MermaidRenderCoordinatorStats{};
  expect(entry_runs && entry_stats.cache_entries == ava::app::kMaxMermaidCacheEntries, "accepted-output cache evicts to its 128-entry bound");

  auto byte_cache = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"output-size", std::to_string(ava::app::kMaxMermaidOutputBytes)}));
  bool byte_runs = byte_cache.has_value();
  if (byte_cache)
  {
    for (std::uint64_t identity = 1; identity <= 17; ++identity)
    {
      auto completion = render(**byte_cache, identity, 1, "large-cache-source-" + std::to_string(identity));
      byte_runs = byte_runs && completion && completion->outcome == ava::app::MermaidRenderOutcome::Accepted;
    }
  }
  auto const byte_stats = byte_cache ? (*byte_cache)->stats() : ava::app::MermaidRenderCoordinatorStats{};
  expect(byte_runs && byte_stats.accepted_cache_bytes <= ava::app::kMaxMermaidAcceptedCacheBytes && byte_stats.cache_entries <= 16,
         "accepted-output cache evicts before exceeding its 4 MiB aggregate text bound");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void test_tui_bridge_epoch_reload_and_callback_lifetime()
{
  ava::app::MermaidDisplaySettings disabled;
  auto created = ava::app::MermaidTuiBridge::create(disabled);
  if (!created)
  {
    expect(false, created.error().format());
    return;
  }
  auto bridge = *created;
  auto callbacks = bridge->tui_bridge();
  auto initial = bridge->presentation_configuration();
  auto const disabled_result = callbacks.enqueue({.identity = 1, .config_epoch = initial.epoch, .source = "graph TD; A-->B"});

  ava::app::MermaidDisplaySettings enabled{
      .enabled = true, .enabled_configured = true, .argv = {AVA_FAKE_MERMAID_HELPER_PATH, "echo"}, .argv_configured = true, .unknown_fields = {}};
  auto applied = bridge->apply(enabled);
  auto configured = bridge->presentation_configuration();
  auto unchanged = bridge->apply(enabled);
  auto configured_again = bridge->presentation_configuration();
  auto queued = callbacks.enqueue({.identity = 2, .config_epoch = configured.epoch, .source = "graph TD; A-->B\n"});
  std::optional<ava::tui::TuiMermaidRenderCompletion> completion;
  auto const deadline = std::chrono::steady_clock::now() + 4s;
  while (!completion && std::chrono::steady_clock::now() < deadline)
  {
    for (auto& candidate : callbacks.drain())
    {
      if (candidate.identity == 2)
        completion = std::move(candidate);
    }
    if (!completion)
      std::this_thread::sleep_for(1ms);
  }
  expect(initial.epoch != 0 && !initial.enabled && disabled_result == ava::tui::TuiMermaidEnqueueResult::Rejected && applied && configured.enabled &&
             configured.epoch > initial.epoch && unchanged && configured_again.epoch == configured.epoch &&
             queued == ava::tui::TuiMermaidEnqueueResult::Accepted && completion && completion->accepted && completion->text == "graph TD; A-->B",
         "app TUI bridge keeps disabled mode process-free, advances only effective configuration changes, and projects accepted helper completions");

  auto invalid = enabled;
  invalid.argv = {"relative-helper"};
  auto invalid_reload = bridge->apply(invalid);
  auto after_invalid = bridge->presentation_configuration();
  auto stale_enqueue = callbacks.enqueue({.identity = 3, .config_epoch = initial.epoch, .source = "stale"});
  expect(!invalid_reload && after_invalid.epoch == configured.epoch && after_invalid.enabled && stale_enqueue == ava::tui::TuiMermaidEnqueueResult::Rejected,
         "invalid bridge reloads retain last-known-good settings and old-epoch requests fail closed");

  std::weak_ptr<ava::app::MermaidTuiBridge> weak = bridge;
  bridge->shutdown();
  bridge.reset();
  created->reset();
  auto const after_shutdown = callbacks.enqueue({.identity = 4, .config_epoch = configured.epoch, .source = "after shutdown"});
  expect(!weak.expired() && after_shutdown == ava::tui::TuiMermaidEnqueueResult::Rejected,
         "coordinator shutdown precedes callback-capture teardown and retained callbacks fail closed");
  callbacks = {};
  expect(weak.expired(), "destroying the final neutral callback bridge releases the shut-down application adapter");
}

void test_configuration_and_immediate_request_bounds()
{
  auto invalid_relative = ava::app::MermaidRenderCoordinator::create({.epoch = 1, .enabled = true, .argv = {"relative"}});
  auto invalid_empty = ava::app::MermaidRenderCoordinator::create({.epoch = 1, .enabled = true, .argv = {}});
  expect(!invalid_relative && !invalid_empty, "coordinator independently rejects unsafe enabled configurations");

  auto disabled = ava::app::MermaidRenderCoordinator::create({.epoch = 9, .enabled = false, .argv = {}});
  if (!disabled)
  {
    expect(false, disabled.error().format());
    return;
  }
  auto rejected_reconfigure = (*disabled)->reconfigure({.epoch = 10, .enabled = true, .argv = {}});
  auto disabled_completion = render(**disabled, 1, 9, "source");
  auto oversized_completion = render(**disabled, 2, 9, std::string(ava::app::kMaxMermaidSourceBytes + 1, 'x'));
  expect(!rejected_reconfigure && (*disabled)->stats().config_epoch == 9 && disabled_completion &&
             disabled_completion->outcome == ava::app::MermaidRenderOutcome::Disabled && oversized_completion &&
             oversized_completion->outcome == ava::app::MermaidRenderOutcome::SourceTooLarge && disabled_completion->text.empty() &&
             oversized_completion->text.empty(),
         "invalid reconfiguration retains the last good epoch, while disabled and over-64-KiB source requests produce typed payload-free fallbacks");
}

}  // namespace

void run_mermaid_render_coordinator_tests()
{
  test_display_settings_mermaid_schema_and_preservation();
  test_no_shell_environment_and_descriptor_launch();
  test_typed_process_and_output_failures();
  test_timeout_group_cleanup_cancel_reconfigure_and_shutdown();
  test_queue_deduplication_cache_and_bounds();
  test_tui_bridge_epoch_reload_and_callback_lifetime();
  test_configuration_and_immediate_request_bounds();
}
