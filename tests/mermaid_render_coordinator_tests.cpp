#include "sys.h"
#include "tests/support/process_gate.h"
#include "tests/support/process_test_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/app/mermaid_render_coordinator.h"
#include "ava/app/tui/mermaid_tui_bridge.h"
#include "ava/app/tui/tui_display_settings.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
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
                                                                  std::vector<ava::app::MermaidRenderCompletion>& others)
{
  std::optional<ava::app::MermaidRenderCompletion> result;
  auto collect = [&] {
    for (auto& completion : coordinator.take_completions())
    {
      if (completion.identity == identity && !result)
        result = std::move(completion);
      else
        others.push_back(std::move(completion));
    }
  };
  collect();
  auto const timeout = std::chrono::duration_cast<std::chrono::milliseconds>(ava::tests::now_plus_seconds(10) - std::chrono::steady_clock::now());
  if (!ava::tests::require(coordinator.wait_until_idle(timeout), "Mermaid coordinator becomes idle before draining completions"))
    return std::nullopt;
  collect();
  return result;
}

std::optional<ava::app::MermaidRenderCompletion> await_completion(ava::app::MermaidRenderCoordinator& coordinator, std::uint64_t identity)
{
  std::vector<ava::app::MermaidRenderCompletion> others;
  auto result = await_completion(coordinator, identity, others);
  for (auto const& other : others)
    expect(false, "unexpected Mermaid completion identity " + std::to_string(other.identity) + " while awaiting " + std::to_string(identity));
  return result;
}

pid_t read_pid(std::filesystem::path const& path)
{
  std::ifstream input(path);
  pid_t pid = -1;
  input >> pid;
  if (!ava::tests::require(static_cast<bool>(input) && pid > 1, "helper publishes a valid pid: " + path.string()))
    return -1;
  return pid;
}

// Readiness is acknowledged after exec and, for group-timeout, descendant initialization.
// The coordinator retains process ownership; tests observe exit/reaping without adopting its child.
class HelperReadiness
{
 public:
  HelperReadiness()
      : root_(ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-mermaid-")),
        listener_(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0))
  {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto const path = socket_path().string();
    if (!listener_ || path.size() >= sizeof(address.sun_path))
      throw std::runtime_error("create Mermaid readiness listener");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener_.get(), 1) != 0)
      throw std::system_error(errno, std::generic_category(), "bind Mermaid readiness listener");
  }

  ava::app::MermaidRenderConfiguration configuration(std::uint64_t epoch, std::vector<std::string> mode) const
  {
    std::vector<std::string> args{"--control-socket", socket_path().string(), "--pid-file", (root_ / "helper.pid").string()};
    args.insert(args.end(), std::make_move_iterator(mode.begin()), std::make_move_iterator(mode.end()));
    return helper_config(epoch, std::move(args));
  }

  std::optional<pid_t> wait_ready(bool descendant = false)
  {
    auto const deadline = ava::tests::now_plus_seconds(10);
    try
    {
      ava::core::UniqueFd peer;
      while (!peer && std::chrono::steady_clock::now() < deadline)
      {
        pollfd descriptor{.fd = listener_.get(), .events = POLLIN, .revents = 0};
        auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto const polled = ::poll(&descriptor, 1, static_cast<int>(std::max(0ms, remaining).count()));
        if (polled < 0 && errno == EINTR)
          continue;
        if (polled <= 0)
          break;
        peer.reset(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!peer && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
          throw std::system_error(errno, std::generic_category(), "accept Mermaid readiness connection");
      }
      if (!ava::tests::require(static_cast<bool>(peer), "executed Mermaid helper connects its readiness socket"))
        return std::nullopt;
      ava::test::ProcessGateSet gates(peer.release());
      auto remaining = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now()));
      };
      gates.wait(0, remaining());
      if (descendant)
        gates.wait(1, remaining());
      auto const pid = read_pid(root_ / "helper.pid");
      if (!ava::tests::require(pid > 1 && ava::tests::process_is_live(pid), "initialized Mermaid helper is live before lifecycle action"))
        return std::nullopt;
      return pid;
    }
    catch (std::exception const& error)
    {
      expect(false, std::string("Mermaid helper readiness: ") + error.what());
      return std::nullopt;
    }
  }

 private:
  std::filesystem::path socket_path() const { return root_ / "ready.sock"; }
  ScopedTestDirectory root_;
  ava::core::UniqueFd listener_;
};

void expect_helper_reaped(pid_t pid, std::string const& label)
{
  expect(ava::tests::wait_for_process_absent(pid, ava::tests::now_plus_seconds(5)), label + ": helper is no longer live");
  int status = 0;
  errno = 0;
  auto const waited = ::waitpid(pid, &status, WNOHANG);
  auto const wait_error = errno;
  ava::tests::expect_all(label + ": coordinator reaps its executed helper", {{"no waitable child", waited == -1}, {"reaped child", wait_error == ECHILD}});
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

  ava::tests::write_test_file(display, R"({"theme":"light","mermaid":{"argv":["/not/installed/helper","--flag"],"future_nested":{"x":1}},"future_top":[1,2]})");
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

  ava::tests::write_test_file(display, R"({"mermaid":{"enabled":true,"argv":["/not/installed/helper","literal"]}})");
  auto enabled = ava::app::load_tui_display_settings(paths);
  auto good_watch = ava::app::load_tui_display_settings_watch_state(paths);
  auto render_configuration =
      enabled ? ava::app::mermaid_render_configuration_from_display_settings(enabled->mermaid, 17) : ava::app::MermaidRenderConfiguration{};
  expect(enabled && enabled->mermaid.enabled && enabled->mermaid.argv.size() == 2 && good_watch && good_watch->mermaid.enabled &&
             render_configuration.epoch == 17 && render_configuration.enabled && render_configuration.argv == enabled->mermaid.argv,
         "enabled Mermaid settings expose a validated absolute argv through load, watch, and coordinator configuration seams");

  auto const last_good = enabled ? enabled->mermaid : ava::app::MermaidDisplaySettings{};
  ava::tests::write_test_file(display, R"({"mermaid":{"enabled":true}})");
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
  for (auto const& invalid : invalid_documents)
  {
    ava::tests::write_test_file(display, invalid);
    expect(!ava::app::load_tui_display_settings(paths), "Mermaid settings reject invalid document: " + invalid);
  }

  ava::tests::write_test_file(display, R"({"mermaid":{"enabled":false,"argv":[]}})");
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
  ava::tests::write_test_file(invalid_executable, "not an executable image\n", 0700);
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
  std::uint64_t identity = 10;
  std::uint64_t epoch = 2;
  for (auto const& failure : cases)
  {
    auto changed = (*coordinator)->reconfigure(helper_config(epoch, failure.argv));
    auto completion = render(**coordinator, identity, epoch);
    ava::tests::expect_all("typed payload-free Mermaid failure: " + failure.argv.front(),
                           {{"configuration accepted", changed.has_value()},
                            {"completion present", completion.has_value()},
                            {"expected outcome", completion && completion->outcome == failure.outcome},
                            {"no payload", completion && completion->text.empty()}});
    ++identity;
    ++epoch;
  }

  auto missing = (*coordinator)->reconfigure({.epoch = epoch, .enabled = true, .argv = {"/definitely/missing/ava-mermaid-helper"}});
  auto missing_completion = render(**coordinator, identity, epoch);
  expect(missing && missing_completion && missing_completion->outcome == ava::app::MermaidRenderOutcome::MissingHelper && missing_completion->text.empty(),
         "a missing helper produces a typed payload-free fallback completion");
}

void test_timeout_group_cleanup_cancel_reconfigure_and_shutdown()
{
  auto const root = create_empty_root("mermaid-lifecycle");
  auto const child_pid_file = root / "child.pid";
  HelperReadiness timeout_helper;
  auto coordinator = ava::app::MermaidRenderCoordinator::create(timeout_helper.configuration(1, {"group-timeout", child_pid_file.string()}));
  if (!ava::tests::require(coordinator.has_value(), coordinator ? "lifecycle coordinator starts" : coordinator.error().format()))
    return;
  auto queued = (*coordinator)->enqueue({.identity = 1, .config_epoch = 1, .source = "group timeout"});
  if (!ava::tests::require(queued == ava::app::MermaidEnqueueResult::Queued, "group timeout request queues"))
    return;
  auto timeout_pid = timeout_helper.wait_ready(true);
  if (!timeout_pid)
    return;
  auto const child_pid = read_pid(child_pid_file);
  if (!ava::tests::require(child_pid > 1 && ava::tests::process_is_live(child_pid), "initialized ordinary descendant is live before timeout"))
    return;
  auto timed_out = await_completion(**coordinator, 1);
  ava::tests::expect_all("absolute timeout terminates the initialized helper group",
                         {{"typed timeout", timed_out && timed_out->outcome == ava::app::MermaidRenderOutcome::Timeout},
                          {"descendant gone", ava::tests::wait_for_process_absent(child_pid, ava::tests::now_plus_seconds(5))}});
  expect_helper_reaped(*timeout_pid, "group timeout");

  HelperReadiness cancel_helper;
  auto cancel_configured = (*coordinator)->reconfigure(cancel_helper.configuration(2, {"timeout"}));
  queued = (*coordinator)->enqueue({.identity = 2, .config_epoch = 2, .source = "cancel me"});
  if (!ava::tests::require(cancel_configured && queued == ava::app::MermaidEnqueueResult::Queued, "active cancellation request queues"))
    return;
  auto cancel_pid = cancel_helper.wait_ready();
  if (!cancel_pid)
    return;
  auto canceled = (*coordinator)->cancel(2, 2);
  auto canceled_completion = await_completion(**coordinator, 2);
  ava::tests::expect_all("explicit cancellation tears down an executed helper and publishes a typed fallback",
                         {{"cancellation accepted", canceled},
                          {"typed cancellation", canceled_completion && canceled_completion->outcome == ava::app::MermaidRenderOutcome::Canceled}});
  expect_helper_reaped(*cancel_pid, "explicit cancellation");

  HelperReadiness stale_helper;
  auto old_configured = (*coordinator)->reconfigure(stale_helper.configuration(3, {"timeout"}));
  queued = (*coordinator)->enqueue({.identity = 3, .config_epoch = 3, .source = "stale source"});
  if (!ava::tests::require(old_configured && queued == ava::app::MermaidEnqueueResult::Queued, "old epoch request queues"))
    return;
  auto stale_pid = stale_helper.wait_ready();
  if (!stale_pid)
    return;
  auto new_configured = (*coordinator)->reconfigure(helper_config(4, {"echo"}));
  queued = (*coordinator)->enqueue({.identity = 4, .config_epoch = 4, .source = "stale source"});
  std::vector<ava::app::MermaidRenderCompletion> others;
  auto current = await_completion(**coordinator, 4, others);
  auto remaining = (*coordinator)->take_completions();
  others.insert(others.end(), std::make_move_iterator(remaining.begin()), std::make_move_iterator(remaining.end()));
  ava::tests::expect_all("epoch changes suppress every observed stale completion and admit fresh work",
                         {{"new configuration accepted", new_configured.has_value()},
                          {"new request queued", queued == ava::app::MermaidEnqueueResult::Queued},
                          {"fresh accepted output", current && current->outcome == ava::app::MermaidRenderOutcome::Accepted && current->text == "stale source"},
                          {"not old cache", current && !current->from_cache},
                          {"no stale completion", std::ranges::none_of(others, [](auto const& item) { return item.identity == 3; })}});
  expect_helper_reaped(*stale_pid, "epoch replacement");

  HelperReadiness shutdown_helper;
  auto shutdown_configured = (*coordinator)->reconfigure(shutdown_helper.configuration(5, {"timeout"}));
  queued = (*coordinator)->enqueue({.identity = 5, .config_epoch = 5, .source = "shutdown"});
  if (!ava::tests::require(shutdown_configured && queued == ava::app::MermaidEnqueueResult::Queued, "shutdown request queues"))
    return;
  auto shutdown_pid = shutdown_helper.wait_ready();
  if (!shutdown_pid)
    return;
  (*coordinator)->shutdown();
  expect((*coordinator)->stats().shutting_down, "shutdown closes coordinator admission");
  expect_helper_reaped(*shutdown_pid, "shutdown");
}

void test_queue_deduplication_cache_and_bounds()
{
  HelperReadiness active_helper;
  auto coordinator = ava::app::MermaidRenderCoordinator::create(active_helper.configuration(1, {"timeout"}));
  if (!ava::tests::require(coordinator.has_value(), coordinator ? "queue coordinator starts" : coordinator.error().format()))
    return;
  auto const active_queued = (*coordinator)->enqueue({.identity = 1, .config_epoch = 1, .source = "active"});
  if (!ava::tests::require(active_queued == ava::app::MermaidEnqueueResult::Queued, "deduplication active request queues"))
    return;
  auto started = active_helper.wait_ready();
  if (!started)
    return;
  auto attached_in_flight = (*coordinator)->enqueue({.identity = 2, .config_epoch = 1, .source = "active"});
  auto queued = (*coordinator)->enqueue({.identity = 3, .config_epoch = 1, .source = "queued"});
  auto attached_queued = (*coordinator)->enqueue({.identity = 4, .config_epoch = 1, .source = "queued"});
  auto dedupe_stats = (*coordinator)->stats();
  ava::tests::expect_all("in-flight and queued sources deduplicate work while retaining request identities",
                         {{"attached in flight", attached_in_flight == ava::app::MermaidEnqueueResult::AttachedToExisting},
                          {"queued unique source", queued == ava::app::MermaidEnqueueResult::Queued},
                          {"attached queued", attached_queued == ava::app::MermaidEnqueueResult::AttachedToExisting},
                          {"one queued render", dedupe_stats.queued_requests == 1},
                          {"four identities", dedupe_stats.pending_identities == 4}});
  auto const canceled_queued = (*coordinator)->cancel(3, 1);
  auto const canceled_attached = (*coordinator)->cancel(4, 1);
  auto canceled_completions = (*coordinator)->take_completions();
  ava::tests::expect_all("queued cancellation publishes each identity without launching queued work",
                         {{"first queued cancellation", canceled_queued},
                          {"attached queued cancellation", canceled_attached},
                          {"queue removed", (*coordinator)->stats().queued_requests == 0},
                          {"two cancellations", canceled_completions.size() == 2},
                          {"identity 3 canceled", std::ranges::count_if(canceled_completions,
                                                                        [](auto const& item) {
                                                                          return item.identity == 3 && item.outcome == ava::app::MermaidRenderOutcome::Canceled;
                                                                        }) == 1},
                          {"identity 4 canceled", std::ranges::count_if(canceled_completions, [](auto const& item) {
                                                    return item.identity == 4 && item.outcome == ava::app::MermaidRenderOutcome::Canceled;
                                                  }) == 1}});
  HelperReadiness bound_helper;
  auto bounds_configured = (*coordinator)->reconfigure(bound_helper.configuration(2, {"timeout"}));
  auto const bounds_queued = (*coordinator)->enqueue({.identity = 100, .config_epoch = 2, .source = "active-bound"});
  if (!ava::tests::require(bounds_configured && bounds_queued == ava::app::MermaidEnqueueResult::Queued, "bounded queue active request queues"))
    return;
  auto bound_started = bound_helper.wait_ready();
  if (!bound_started)
    return;
  for (std::uint64_t identity = 101; identity < 132; ++identity)
  {
    auto const admitted = (*coordinator)->enqueue({.identity = identity, .config_epoch = 2, .source = "queued-" + std::to_string(identity)});
    ava::tests::expect_eq(admitted, ava::app::MermaidEnqueueResult::Queued, "bounded queue admits identity " + std::to_string(identity));
  }
  auto const full = (*coordinator)->enqueue({.identity = 132, .config_epoch = 2, .source = "one-too-many"});
  ava::tests::expect_all("queue admits exactly its undrained identity bound",
                         {{"next request rejected", full == ava::app::MermaidEnqueueResult::QueueFull},
                          {"exact pending identity count", (*coordinator)->stats().pending_identities == ava::app::kMaxMermaidQueuedRequests}});

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
  ava::tests::expect_all(
      "positive and negative cache entries suppress repeated helper launches",
      {{"positive configuration", cache_configured.has_value()},
       {"positive output", first && first->outcome == ava::app::MermaidRenderOutcome::Accepted},
       {"positive cache hit", second && second->from_cache},
       {"one positive launch", positive_count == 1},
       {"negative configuration", negative_configured.has_value()},
       {"negative output", first_negative && first_negative->outcome == ava::app::MermaidRenderOutcome::NonzeroExit},
       {"negative cache hit", second_negative && second_negative->from_cache && second_negative->outcome == ava::app::MermaidRenderOutcome::NonzeroExit},
       {"one negative launch", total_count == 2}});
  auto reset = (*coordinator)->reconfigure(helper_config(5, {"--count", count_file.string(), "exit", "7"}));
  auto const reset_stats = (*coordinator)->stats();
  auto after_reset = render(**coordinator, 204, 5, "bad source");
  ava::tests::expect_all(
      "an epoch change clears both positive and negative cache accounting",
      {{"new epoch accepted", reset.has_value()},
       {"cache empty before rendering", reset_stats.cache_entries == 0 && reset_stats.accepted_cache_bytes == 0},
       {"negative outcome rendered afresh", after_reset && !after_reset->from_cache && after_reset->outcome == ava::app::MermaidRenderOutcome::NonzeroExit},
       {"helper relaunched", read_file(count_file).size() == 3}});

  auto entry_cache = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"echo"}));
  if (!ava::tests::require(entry_cache.has_value(), "entry-bounded cache coordinator starts"))
    return;
  if (entry_cache)
  {
    for (std::uint64_t identity = 1; identity <= 129; ++identity)
    {
      auto completion = render(**entry_cache, identity, 1, "cache-source-" + std::to_string(identity));
      if (!ava::tests::require(completion && completion->outcome == ava::app::MermaidRenderOutcome::Accepted,
                               "entry cache accepts source " + std::to_string(identity)))
        return;
      if (identity == 128)
        ava::tests::expect_eq((*entry_cache)->stats().cache_entries, std::size_t{128}, "entry cache retains its exact capacity before eviction");
    }
  }
  auto const entry_stats = entry_cache ? (*entry_cache)->stats() : ava::app::MermaidRenderCoordinatorStats{};
  ava::tests::expect_eq(entry_stats.cache_entries, ava::app::kMaxMermaidCacheEntries, "entry cache retains exactly 128 entries after overflow");
  auto retained_entry = render(**entry_cache, 130, 1, "cache-source-2");
  auto evicted_entry = render(**entry_cache, 131, 1, "cache-source-1");
  ava::tests::expect_all(
      "entry capacity evicts the oldest source without clearing retained entries",
      {{"second source retained", retained_entry && retained_entry->from_cache}, {"first source evicted", evicted_entry && !evicted_entry->from_cache}});

  auto byte_cache = ava::app::MermaidRenderCoordinator::create(helper_config(1, {"output-size", std::to_string(ava::app::kMaxMermaidOutputBytes)}));
  if (!ava::tests::require(byte_cache.has_value(), "byte-bounded cache coordinator starts"))
    return;
  if (byte_cache)
  {
    for (std::uint64_t identity = 1; identity <= 17; ++identity)
    {
      auto completion = render(**byte_cache, identity, 1, "large-cache-source-" + std::to_string(identity));
      if (!ava::tests::require(completion && completion->outcome == ava::app::MermaidRenderOutcome::Accepted,
                               "byte cache accepts source " + std::to_string(identity)))
        return;
      if (identity == 16)
        ava::tests::expect_eq((*byte_cache)->stats().cache_entries, std::size_t{16}, "byte cache retains every entry below its aggregate bound");
    }
  }
  auto const byte_stats = byte_cache ? (*byte_cache)->stats() : ava::app::MermaidRenderCoordinatorStats{};
  ava::tests::expect_all("byte capacity evicts only the oldest accepted output",
                         {{"sixteen retained outputs", byte_stats.cache_entries == 16},
                          {"exact retained bytes", byte_stats.accepted_cache_bytes == 16 * (ava::app::kMaxMermaidOutputBytes - 1)}});
  auto retained_bytes = render(**byte_cache, 18, 1, "large-cache-source-2");
  auto evicted_bytes = render(**byte_cache, 19, 1, "large-cache-source-1");
  ava::tests::expect_all("byte eviction preserves newer cached output", {{"second source retained", retained_bytes && retained_bytes->from_cache},
                                                                         {"first source evicted", evicted_bytes && !evicted_bytes->from_cache}});

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
  auto const deadline = ava::tests::now_plus_seconds(10);
  while (!completion && std::chrono::steady_clock::now() < deadline)
  {
    for (auto& candidate : callbacks.drain())
    {
      if (candidate.identity == 2)
        completion = std::move(candidate);
    }
    if (!completion)
      std::this_thread::yield();
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
  ava::tests::HermeticTestEnvironment environment("mermaid-tests-");
  test_display_settings_mermaid_schema_and_preservation();
  test_no_shell_environment_and_descriptor_launch();
  test_typed_process_and_output_failures();
  test_timeout_group_cleanup_cancel_reconfigure_and_shutdown();
  test_queue_deduplication_cache_and_bounds();
  test_tui_bridge_epoch_reload_and_callback_lifetime();
  test_configuration_and_immediate_request_bounds();
}
