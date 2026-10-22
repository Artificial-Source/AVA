#include "tests/support/process_group_test_support.h"
#include "tests/support/process_test_support.h"
#include "tests/support/syscall_errno_injection.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void test_write_all_transfers_exact_pipe_payload()
{
  std::array<int, 2> descriptors{-1, -1};
  bool const pipe_created = ::pipe(descriptors.data()) == 0;
  if (!pipe_created)
  {
    expect(false, "test harness exact-transfer pipe is created");
    return;
  }

  constexpr std::string_view payload = "exact pipe transfer";
  bool const written = write_all_to_descriptor_for_test(descriptors[1], payload.data(), payload.size());
  static_cast<void>(::close(descriptors[1]));

  std::array<char, payload.size()> observed{};
  bool const read = read_exact_from_descriptor_for_test(descriptors[0], observed.data(), observed.size());
  static_cast<void>(::close(descriptors[0]));

  expect(written && read && std::string_view(observed.data(), observed.size()) == payload, "test harness write helper transfers an exact pipe payload");
}

void test_write_all_reports_closed_pipe_without_sigpipe_termination()
{
  std::array<int, 2> descriptors{-1, -1};
  bool const pipe_created = ::pipe(descriptors.data()) == 0;
  if (!pipe_created)
  {
    expect(false, "test harness closed-reader pipe is created");
    return;
  }

  static_cast<void>(::close(descriptors[0]));
  constexpr char payload = 'x';
  bool const written = write_all_to_descriptor_for_test(descriptors[1], &payload, sizeof(payload));
  static_cast<void>(::close(descriptors[1]));

  expect(!written, "test harness write helper reports a closed pipe without terminating the test process");
}

void test_debug_timeout_deadlines_honor_runtime_environment()
{
  ScopedEnvVar const enabled("AVA_DEBUG_NO_TIMEOUT", "1");
  ScopedEnvVar const seconds("AVA_DEBUG_NO_TIMEOUT_SECONDS", "17");

  auto const before_integer = std::chrono::steady_clock::now();
  auto const integer_deadline = ava::tests::now_plus_seconds(2);
  auto const integer_delay = integer_deadline - before_integer;
  expect(integer_delay >= std::chrono::seconds(16) && integer_delay <= std::chrono::seconds(18),
         "test timeout helper applies AVA_DEBUG_NO_TIMEOUT_SECONDS to integer-second deadlines");

  auto const before_duration = std::chrono::steady_clock::now();
  auto const duration_deadline = ava::tests::now_plus_seconds(std::chrono::milliseconds(250));
  auto const duration_delay = duration_deadline - before_duration;
  expect(duration_delay >= std::chrono::seconds(16) && duration_delay <= std::chrono::seconds(18),
         "test timeout helper applies AVA_DEBUG_NO_TIMEOUT_SECONDS to duration-valued deadlines");
}

void test_scoped_directory_teardown_removes_owned_tree()
{
  std::filesystem::path captured;
  {
    auto dir = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-teardown-");
    captured = dir.path();
    std::filesystem::create_directories(dir.path() / "nested");
    std::ofstream(dir.path() / "nested" / "file.txt") << "owned";
    expect(std::filesystem::exists(captured / "nested" / "file.txt"), "scoped test directory creates an owned tree");
  }
  expect(!std::filesystem::exists(captured), "scoped test directory removes its tree on destruction");
}

void test_scoped_directory_teardown_on_exception()
{
  std::filesystem::path captured;
  try
  {
    auto dir = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-exception-");
    captured = dir.path();
    std::filesystem::create_directories(dir.path() / "nested");
    throw std::runtime_error("scoped test directory exception teardown");
  }
  catch (std::runtime_error const&)
  {
  }
  expect(!captured.empty() && !std::filesystem::exists(captured), "scoped test directory removes its tree when an exception unwinds");
}

void test_scoped_directory_does_not_follow_symlink_into_foreign_target()
{
  auto foreign = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-foreign-");
  auto const marker = foreign.path() / "keep-me";
  std::ofstream(marker) << "keep";
  std::filesystem::path symlink_path;
  {
    auto owned = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-symlink-");
    symlink_path = owned.path();
    std::filesystem::remove(owned.path());
    std::filesystem::create_directory_symlink(foreign.path(), owned.path());
  }
  expect(!std::filesystem::exists(symlink_path) && std::filesystem::exists(marker),
         "owned symlink teardown unlinks the symlink and leaves the foreign target in place");
}

void test_forked_child_does_not_remove_parent_fixtures()
{
  auto dir = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-fork-");
  auto const marker = dir.path() / "parent-owned";
  std::ofstream(marker) << "parent";
  pid_t const child = ::fork();
  if (child == 0)
  {
    static_cast<void>(cleanup_owned_test_directories());
    int const still_present = std::filesystem::exists(marker) ? 0 : 1;
    ::_exit(still_present);
  }
  if (child < 0)
  {
    expect(false, "fixture-ownership fork starts");
    return;
  }
  int status = 0;
  bool const waited = ::waitpid(child, &status, 0) == child;
  expect(waited && WIFEXITED(status) && WEXITSTATUS(status) == 0 && std::filesystem::exists(marker),
         "forked child cleanup leaves parent-owned fixtures in place");
}

void test_temp_root_does_not_adopt_existing_directory()
{
  auto staging = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-no-adopt-");
  ScopedEnvVar const tmpdir("TMPDIR", staging.path().string());
  // Simulate a legacy PID-named directory only inside this test's private namespace.
  auto const impostor = staging.path() / ("ava_core_tests_" + std::to_string(static_cast<unsigned long long>(::getpid())));
  std::filesystem::create_directory(impostor);
  auto const marker = impostor / "not-owned";
  std::ofstream(marker) << "leave me";
  auto const root = temp_root();
  expect(root != impostor && std::filesystem::exists(marker), "temp_root uniquely creates a namespace and does not adopt an existing pid-named directory");
}

void test_cleanup_does_not_remove_untracked_directories()
{
  auto staging = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-siblings-");
  auto const untracked = staging.path() / "untracked";
  std::filesystem::create_directory(untracked);
  {
    auto owned = ScopedTestDirectory::create_unique(staging.path(), "tracked-");
    std::ofstream(owned.path() / "owned.txt") << "owned";
  }
  expect(std::filesystem::is_directory(untracked), "scoped teardown leaves a sibling outside its acquired directory intact");
}

struct RestoreCwd
{
  std::filesystem::path previous = std::filesystem::current_path();
  ~RestoreCwd()
  {
    std::error_code error;
    std::filesystem::current_path(previous, error);
  }
};

void test_owned_directory_cleanup_survives_relative_parent_and_cwd_change()
{
  RestoreCwd restore;
  auto staging = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-relcwd-");
  auto const relative_parent = std::filesystem::path("rel-parent");
  std::filesystem::create_directories(staging.path() / relative_parent);
  std::filesystem::current_path(staging.path());
  std::filesystem::path captured;
  {
    auto dir = ScopedTestDirectory::create_unique(relative_parent, "ava-harness-rel-");
    captured = dir.path();
    expect(captured.is_absolute(), "owned directory stores an absolute lexical path at creation");
    std::ofstream(captured / "marker") << "x";
    std::filesystem::current_path(restore.previous);
  }
  expect(!captured.empty() && captured.is_absolute() && !std::filesystem::exists(captured),
         "owned directory teardown uses the creation-time absolute path after cwd changes");
}

void test_temp_root_relative_tmpdir_survives_cwd_change()
{
  RestoreCwd restore;
  auto staging = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-reltmp-");
  auto const work = staging.path() / "work";
  auto const tmp = staging.path() / "tmp";
  std::filesystem::create_directories(work);
  std::filesystem::create_directories(tmp);
  std::filesystem::current_path(work);
  std::filesystem::path root;
  {
    ScopedEnvVar const tmpdir("TMPDIR", (std::filesystem::path("..") / "tmp").string());
    root = temp_root();
    std::ofstream(root / "marker") << "x";
  }
  std::filesystem::current_path(restore.previous);
  expect(root.is_absolute() && std::filesystem::exists(root / "marker"), "temp_root keeps an absolute namespace after cwd changes with a relative TMPDIR");
}

void test_forked_child_temp_root_is_distinct_and_does_not_remove_parent()
{
  auto const parent_root = temp_root();
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()) != 0)
  {
    expect(false, "temp_root fork pipe is created");
    return;
  }
  pid_t const child = ::fork();
  if (child == 0)
  {
    static_cast<void>(::close(descriptors[0]));
    auto const child_root = temp_root();
    auto const text = child_root.string();
    bool const distinct = child_root != parent_root && std::filesystem::exists(parent_root);
    bool const written = write_all_to_descriptor_for_test(descriptors[1], text.data(), text.size());
    static_cast<void>(::close(descriptors[1]));
    static_cast<void>(cleanup_owned_test_directories());
    ::_exit(distinct && written && std::filesystem::exists(parent_root) ? 0 : 1);
  }
  static_cast<void>(::close(descriptors[1]));
  if (child < 0)
  {
    static_cast<void>(::close(descriptors[0]));
    expect(false, "temp_root fork starts");
    return;
  }
  std::string child_root;
  char buffer[4096];
  for (;;)
  {
    auto const transferred = ::read(descriptors[0], buffer, sizeof(buffer));
    if (transferred < 0 && errno == EINTR)
      continue;
    if (transferred <= 0)
      break;
    child_root.append(buffer, static_cast<std::size_t>(transferred));
  }
  static_cast<void>(::close(descriptors[0]));
  int status = 0;
  bool const waited = ::waitpid(child, &status, 0) == child;
  expect(waited && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !child_root.empty() && std::filesystem::path(child_root) != parent_root &&
             std::filesystem::exists(parent_root) && !std::filesystem::exists(child_root),
         "forked child temp_root is a distinct namespace and child cleanup leaves the parent namespace in place");
}

void run_harness_probe(std::function<bool(std::ostringstream&)> const& probe, std::string const& message)
{
  std::cout.flush();
  std::cerr.flush();
  pid_t const child = ::fork();
  if (child == 0)
  {
    // A stalled probe may own children of its own. Its watchdog owns their
    // private group as well, rather than killing only the probe leader.
    if (::setpgid(0, 0) != 0)
      ::_exit(1);
    std::ostringstream output;
    auto* previous_error = std::cerr.rdbuf(output.rdbuf());
    auto* previous_output = std::cout.rdbuf(output.rdbuf());
    bool passed = false;
    try
    {
      passed = probe(output);
    }
    catch (std::exception const& error)
    {
      output << "probe threw: " << error.what() << '\n';
    }
    catch (...)
    {
      output << "probe threw an unknown exception\n";
    }
    std::cerr.rdbuf(previous_error);
    std::cout.rdbuf(previous_output);
    if (!passed)
      std::cerr << output.str();
    ::_exit(passed ? 0 : 1);
  }
  if (!ava::tests::require(child > 0, message + ": fork probe"))
    return;
  ava::tests::ScopedChild owner(child, true);
  auto const status = owner.wait_until(ava::tests::now_plus_seconds(5));
  ava::tests::expect_child_success(status ? std::expected<int, int>(*status) : std::unexpected(errno), message);
}

void test_assertion_diagnostics_and_fixture_guards()
{
  run_harness_probe(
      [](std::ostringstream& output) {
        auto const before = ava::tests::failures();
        ava::tests::clear_skip();
        expect(true, "ordinary success");
        ava::tests::expect_eq(3, 3, "equal values");
        ava::tests::expect_eq(3U, 3, "mixed-sign equal values");
        ava::tests::expect_eq(-1, static_cast<unsigned>(-1), "negative values do not equal unsigned wraparound");
        ava::tests::expect_eq(3, 7, "different values");
        ava::tests::expect_all("named conditions", {{"ready", true}, {"settled", false}, {"reaped", false}});
        bool const fixture = ava::tests::require(false, "missing fixture");
        auto const text = output.str();
        return !fixture && ava::tests::failures() == before + 4 && ava::tests::assertion_count() == 7 && text.find("actual: 3") != std::string::npos &&
               text.find("expected: 7") != std::string::npos && text.find("settled=false") != std::string::npos &&
               text.find("reaped=false") != std::string::npos && text.find("ready=false") == std::string::npos &&
               text.find("missing fixture") != std::string::npos && text.find("test_harness_tests.cpp:") != std::string::npos;
      },
      "assertions count failures once and diagnose values, false members, locations and failed fixtures");
}

void test_skip_accounting_is_per_suite_and_reports_every_case()
{
  run_harness_probe(
      [](std::ostringstream& output) {
        ava::tests::clear_skip();
        ava::tests::request_skip("first case", "missing first facility");
        ava::tests::request_skip("second case", "missing second facility");
        bool const only_skips = ava::tests::skip_requested() && ava::tests::all_cases_skipped() && ava::tests::skipped_case_count() == 2;
        expect(true, "executed case");
        bool const partial = ava::tests::skip_requested() && !ava::tests::all_cases_skipped();
        auto const messages = ava::tests::skip_message();
        ava::tests::clear_skip();
        bool const reset =
            !ava::tests::skip_requested() && !ava::tests::all_cases_skipped() && ava::tests::skipped_case_count() == 0 && ava::tests::assertion_count() == 0;
        auto const outcome = ava::tests::expect_child_success(std::expected<int, int>(77 << 8), "child case", "missing injector");
        bool const child_skip = outcome == ava::tests::ChildTestOutcome::Skipped && ava::tests::all_cases_skipped();
        auto const passed = ava::tests::expect_child_success(std::expected<int, int>(0), "later child");
        int const before_failure = ava::tests::failures();
        auto const failed = ava::tests::expect_child_success(std::expected<int, int>(1 << 8), "failed child");
        auto const text = output.str();
        return only_skips && partial && reset && child_skip && passed == ava::tests::ChildTestOutcome::Passed && !ava::tests::all_cases_skipped() &&
               failed == ava::tests::ChildTestOutcome::Failed && ava::tests::failures() == before_failure + 1 &&
               messages.find("first case: missing first facility") != std::string::npos &&
               messages.find("second case: missing second facility") != std::string::npos &&
               text.find("SKIP: first case: missing first facility") != std::string::npos &&
               text.find("SKIP: second case: missing second facility") != std::string::npos &&
               text.find("SKIP: child case: missing injector") != std::string::npos;
      },
      "skips retain every named reason and distinguish wholly skipped suites from executed cases");
}

void test_worker_assertions_and_skip_records_are_synchronized()
{
  run_harness_probe(
      [](std::ostringstream&) {
        int const before = ava::tests::failures();
        ava::tests::clear_skip();
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 4; ++worker)
          workers.emplace_back([worker] {
            for (int iteration = 0; iteration < 25; ++iteration)
            {
              expect(false, "worker failure");
              ava::tests::request_skip("worker " + std::to_string(worker), "optional facility");
              static_cast<void>(ava::tests::failures());
              static_cast<void>(ava::tests::skip_message());
            }
          });
        workers.clear();
        return ava::tests::failures() == before + 100 && ava::tests::assertion_count() == 100 && ava::tests::skipped_case_count() == 100 &&
               ava::tests::skip_requested() && !ava::tests::all_cases_skipped();
      },
      "worker assertions and skip records retain every concurrent update");
}

void test_environment_guards_restore_unset_empty_and_nested_values()
{
  ScopedEnvVar const initially_unset("AVA_HARNESS_ENV_PROBE", std::nullopt);
  {
    ScopedEnvVar const empty("AVA_HARNESS_ENV_PROBE", "");
    auto const* value = std::getenv("AVA_HARNESS_ENV_PROBE");
    expect(value && std::string_view(value).empty(), "environment guard preserves an explicitly empty value");
    {
      ScopedEnvVar const absent("AVA_HARNESS_ENV_PROBE", std::nullopt);
      expect(std::getenv("AVA_HARNESS_ENV_PROBE") == nullptr, "environment guard supports a genuinely unset value");
      ScopedEnvVar const nested("AVA_HARNESS_ENV_PROBE", "nested");
      expect(std::string_view(std::getenv("AVA_HARNESS_ENV_PROBE")) == "nested", "environment guard permits nested overrides");
    }
    value = std::getenv("AVA_HARNESS_ENV_PROBE");
    expect(value && std::string_view(value).empty(), "nested environment guards restore an empty value");
  }
  expect(std::getenv("AVA_HARNESS_ENV_PROBE") == nullptr, "environment guard restores an originally absent variable");
}

void test_hermetic_environment_clears_ambient_inputs_and_restores_on_exception()
{
  constexpr auto names = std::to_array<char const*>({
      "ANTHROPIC_API_KEY",
      "ANTHROPIC_OAUTH_TOKEN",
      "ANTHROPIC_AUTH_TOKEN",
      "ANTHROPIC_BASE_URL",
      "ANTHROPIC_FUTURE_CREDENTIAL",
      "OPENAI_API_KEY",
      "OPENAI_BASE_URL",
      "GEMINI_API_KEY",
      "GEMINI_BASE_URL",
      "GOOGLE_API_KEY",
      "GOOGLE_APPLICATION_CREDENTIALS",
      "XAI_API_KEY",
      "XAI_BASE_URL",
      "GROQ_API_KEY",
      "GROQ_BASE_URL",
      "CEREBRAS_API_KEY",
      "CEREBRAS_BASE_URL",
      "TOGETHER_API_KEY",
      "TOGETHER_BASE_URL",
      "FIREWORKS_API_KEY",
      "FIREWORKS_BASE_URL",
      "MISTRAL_API_KEY",
      "MISTRAL_BASE_URL",
      "DEEPSEEK_API_KEY",
      "DEEPSEEK_BASE_URL",
      "KIMI_API_KEY",
      "KIMI_BASE_URL",
      "MOONSHOT_API_KEY",
      "MOONSHOT_BASE_URL",
      "OPENROUTER_API_KEY",
      "OPENROUTER_BASE_URL",
      "ZAI_API_KEY",
      "ZAI_BASE_URL",
      "ZAI_CODING_CN_API_KEY",
      "ZAI_CODING_CN_BASE_URL",
      "http_proxy",
      "https_proxy",
      "all_proxy",
      "no_proxy",
      "HTTP_PROXY",
      "HTTPS_PROXY",
      "ALL_PROXY",
      "NO_PROXY",
      "HOME",
      "XDG_CONFIG_HOME",
      "XDG_STATE_HOME",
      "XDG_DATA_HOME",
      "XDG_CACHE_HOME",
  });
  std::deque<ScopedEnvVar> ambient;
  for (auto const* name : names)
    ambient.emplace_back(name, "ambient-canary");
  ScopedEnvVar const new_variable("OPENAI_TEST_NEW", std::nullopt);
  std::filesystem::path root;
  try
  {
    ava::tests::HermeticTestEnvironment const environment;
    root = environment.root();
    for (std::size_t index = 0; index < names.size() - 5; ++index)
      expect(std::getenv(names[index]) == nullptr, std::string("hermetic environment clears ") + names[index]);
    constexpr std::array<char const*, 5> directories{"home", "config", "state", "data", "cache"};
    for (std::size_t index = 0; index < directories.size(); ++index)
    {
      auto const* value = std::getenv(names[names.size() - 5 + index]);
      expect(value && std::filesystem::path(value) == root / directories[index] && std::filesystem::is_directory(value),
             std::string("hermetic environment isolates ") + directories[index]);
    }
    expect(::setenv("OPENAI_TEST_NEW", "temporary-canary", 1) == 0, "hermetic environment test adds a new provider input");
    throw std::runtime_error("unwind hermetic fixture");
  }
  catch (std::runtime_error const&)
  {
  }
  for (auto const* name : names)
  {
    auto const* value = std::getenv(name);
    expect(value && std::string_view(value) == "ambient-canary", std::string("hermetic environment restores ") + name);
  }
  expect(std::getenv("OPENAI_TEST_NEW") == nullptr, "hermetic environment removes provider inputs added during its scope");
  expect(!root.empty() && !std::filesystem::exists(root), "hermetic environment removes its private tree after exception unwinding");
}

void test_shared_pipe_reader_preserves_bytes_and_enforces_bounds()
{
  run_harness_probe(
      [](std::ostringstream&) {
        std::array<int, 2> descriptors{-1, -1};
        if (::pipe(descriptors.data()) != 0)
          return false;
        ava::core::UniqueFd reader(descriptors[0]);
        ava::core::UniqueFd writer(descriptors[1]);
        std::string received = "REA";
        constexpr std::string_view payload = "DY\nNEXT\n";
        if (!write_all_to_descriptor_for_test(writer.get(), payload.data(), payload.size()))
          return false;
        writer.reset();
        bool const first = ava::tests::read_until(reader.get(), received, "READY\n", ava::tests::now_plus_seconds(2), 11);
        bool const retained = ava::tests::read_until(reader.get(), received, "NEXT\n", std::chrono::steady_clock::now(), 11);
        bool const eof = !ava::tests::read_until(reader.get(), received, "missing", ava::tests::now_plus_seconds(2), 32);
        ava::process::PipeEndpoint invalid;
        std::string empty;
        bool const invalid_endpoint = !ava::tests::read_until(invalid, empty, "marker", ava::tests::now_plus_seconds(2));
        return first && retained && eof && received == "READY\nNEXT\n" && invalid_endpoint;
      },
      "shared pipe reader retains complete output, cross-chunk markers, EOF and invalid endpoint failures");

  run_harness_probe(
      [](std::ostringstream&) {
        std::array<int, 2> descriptors{-1, -1};
        if (::pipe(descriptors.data()) != 0)
          return false;
        ava::core::UniqueFd reader(descriptors[0]);
        ava::core::UniqueFd writer(descriptors[1]);
        constexpr std::string_view payload = "123456789";
        if (!write_all_to_descriptor_for_test(writer.get(), payload.data(), payload.size()))
          return false;
        std::string received;
        bool const bounded = !ava::tests::read_until(reader.get(), received, "absent", ava::tests::now_plus_seconds(2), 4) && received == "1234";
        std::string remainder;
        bool const exact = ava::tests::read_until(reader.get(), remainder, "9", ava::tests::now_plus_seconds(2), 5) && remainder == "56789";
        ScopedEnvVar const ordinary_timeout("AVA_DEBUG_NO_TIMEOUT", std::nullopt);
        constexpr std::string_view fragment = "partial";
        if (!write_all_to_descriptor_for_test(writer.get(), fragment.data(), fragment.size()))
          return false;
        std::string partial;
        bool const fragment_observed = ava::tests::read_until(reader.get(), partial, fragment, ava::tests::now_plus_seconds(2));
    // The writer stays open: neither EOF nor a complete record can rescue a blocking read.
        bool const timed_out = !ava::tests::read_until(reader.get(), partial, "never", ava::tests::now_plus_seconds(std::chrono::milliseconds(10)));
        return bounded && exact && fragment_observed && timed_out && partial == fragment;
      },
      "shared pipe reader respects an exact total byte cap and times out with an unterminated record");
}

void test_owned_child_wait_and_live_process_observation()
{
  run_harness_probe(
      [](std::ostringstream&) {
        std::array<int, 2> descriptors{-1, -1};
        if (::pipe(descriptors.data()) != 0)
          return false;
        ava::core::UniqueFd reader(descriptors[0]);
        ava::core::UniqueFd writer(descriptors[1]);
        pid_t const child = ::fork();
        if (child == 0)
        {
          writer.reset();
          char release = '\0';
          bool const released = read_exact_from_descriptor_for_test(reader.get(), &release, 1);
          ::_exit(released ? 23 : 1);
        }
        if (child < 0)
          return false;
        ava::tests::ScopedChild owner(child);
        reader.reset();
        bool const live = ava::tests::process_is_live(child) && !ava::tests::wait_for_process_absent(child, std::chrono::steady_clock::now());
        constexpr char release = 'x';
        if (!write_all_to_descriptor_for_test(writer.get(), &release, 1))
          return false;
        writer.reset();
        bool const dead = ava::tests::wait_for_process_absent(child, ava::tests::now_plus_seconds(2));
    // A direct child remains a zombie until this owner reaps it, independent of PID 1.
        bool const zombie_still_exists = ::kill(child, 0) == 0;
        auto const status = owner.wait_until(ava::tests::now_plus_seconds(2));
        bool const reaped = ::waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD;
        return live && dead && zombie_still_exists && status && WIFEXITED(*status) && WEXITSTATUS(*status) == 23 && reaped &&
               !ava::tests::process_is_live(child);
      },
      "process observation excludes zombies while owned child wait preserves and reaps the exact exit status");

  run_harness_probe(
      [](std::ostringstream&) {
        pid_t const child = ::fork();
        if (child == 0)
          for (;;)
            static_cast<void>(::pause());
        if (child < 0)
          return false;
        {
          ava::tests::ScopedChild owner(child);
        }
        bool const destructor_reaped = ::waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD;
        pid_t const timed_child = ::fork();
        if (timed_child == 0)
          for (;;)
            static_cast<void>(::pause());
        if (timed_child < 0)
          return false;
        ava::tests::ScopedChild timed_owner(timed_child);
        auto const status = timed_owner.wait_until(std::chrono::steady_clock::now());
        int const wait_error = errno;
        bool const timeout_reaped = ::waitpid(timed_child, nullptr, WNOHANG) == -1 && errno == ECHILD;
        return destructor_reaped && !status && wait_error == ETIMEDOUT && timeout_reaped && !ava::tests::wait_child_until(0, ava::tests::now_plus_seconds(2)) &&
               errno == EINVAL && !ava::tests::wait_for_process_absent(0, std::chrono::steady_clock::now()) && errno == EINVAL &&
               !ava::test::wait_for_process_group_exit(0, std::chrono::steady_clock::now()) && errno == EINVAL &&
               !ava::test::wait_for_process_group_exit(::getpgrp(), std::chrono::steady_clock::now());
      },
      "child ownership kills and reaps on early scope exit and expired deadlines, and rejects non-child identifiers");
}

void test_child_runner_accepts_an_explicit_watchdog_deadline()
{
  run_harness_probe(
      [](std::ostringstream&) {
        auto const status = ava::tests::run_in_child_with_syscall_errno(
            std::span<ava::tests::SyscallErrno const>{},
            []() -> int {
              for (;;)
                static_cast<void>(::pause());
            },
            std::chrono::steady_clock::now());
        bool const timed_out = !status && status.error() == ETIMEDOUT;
        bool const reaped = ::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD;
        return timed_out && reaped;
      },
      "an explicit child-runner deadline overrides the default and still reaps the timed-out child");
}

void test_timeout_policy_matrix_and_small_harness_utilities()
{
  {
    ScopedEnvVar const disabled("AVA_DEBUG_NO_TIMEOUT", std::nullopt);
    ava::tests::expect_eq(ava::tests::detail::effective_test_timeout(std::chrono::milliseconds(250)).count(),
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count(),
                          "unset debug override preserves the authored timeout");
  }
  ScopedEnvVar const enabled("AVA_DEBUG_NO_TIMEOUT", "1");
  {
    ScopedEnvVar const unset_seconds("AVA_DEBUG_NO_TIMEOUT_SECONDS", std::nullopt);
    auto const actual = std::chrono::duration_cast<std::chrono::seconds>(ava::tests::detail::effective_test_timeout(std::chrono::seconds(2))).count();
    ava::tests::expect_eq(actual, 3600, "unset debug duration uses the one-hour default");
  }
  for (auto const* text : {"", "0", "-5", "abc", "12.5", "05", "17"})
  {
    ScopedEnvVar const seconds("AVA_DEBUG_NO_TIMEOUT_SECONDS", text);
    auto const expected = std::string_view(text) == "05" ? 5 : std::string_view(text) == "17" ? 17 : 3600;
    auto const actual = std::chrono::duration_cast<std::chrono::seconds>(ava::tests::detail::effective_test_timeout(std::chrono::seconds(2))).count();
    ava::tests::expect_eq(actual, expected, std::string("runtime timeout policy input '") + text + "'");
  }
  ava::tests::expect_eq(strip_sgr("\x1b[31mred\x1b[0m plain"), std::string("red plain"), "SGR stripping retains visible text");
  ava::tests::expect_eq(ava::tests::count_occurrences("aaaaa", "aa"), std::size_t{2}, "occurrence counting is nonoverlapping");
  ava::tests::expect_eq(ava::tests::count_occurrences("anything", ""), std::size_t{0}, "empty search terms have no occurrences");
}

void test_root_reuse_and_missing_owned_directory_cleanup()
{
  run_harness_probe(
      [](std::ostringstream&) {
        auto const first = create_empty_root("harness-root-reuse");
        std::ofstream(first / "stale") << "old";
        auto const second = create_empty_root("harness-root-reuse");
        bool const reused = first == second && std::filesystem::is_empty(second);
        auto owned = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-harness-missing-");
        std::filesystem::remove(owned.path());
        int const before = ava::tests::failures();
        bool const cleaned = cleanup_owned_test_directories();
        return reused && cleaned && ava::tests::failures() == before;
      },
      "logical roots are emptied on reuse and missing owned directories clean up successfully");
}

}  // namespace

void run_test_harness_tests()
{
  test_write_all_transfers_exact_pipe_payload();
  test_write_all_reports_closed_pipe_without_sigpipe_termination();
  test_debug_timeout_deadlines_honor_runtime_environment();
  test_scoped_directory_teardown_removes_owned_tree();
  test_scoped_directory_teardown_on_exception();
  test_scoped_directory_does_not_follow_symlink_into_foreign_target();
  test_forked_child_does_not_remove_parent_fixtures();
  test_temp_root_does_not_adopt_existing_directory();
  test_cleanup_does_not_remove_untracked_directories();
  test_owned_directory_cleanup_survives_relative_parent_and_cwd_change();
  test_temp_root_relative_tmpdir_survives_cwd_change();
  test_forked_child_temp_root_is_distinct_and_does_not_remove_parent();
  test_assertion_diagnostics_and_fixture_guards();
  test_skip_accounting_is_per_suite_and_reports_every_case();
  test_worker_assertions_and_skip_records_are_synchronized();
  test_environment_guards_restore_unset_empty_and_nested_values();
  test_hermetic_environment_clears_ambient_inputs_and_restores_on_exception();
  test_shared_pipe_reader_preserves_bytes_and_enforces_bounds();
  test_owned_child_wait_and_live_process_observation();
  test_child_runner_accepts_an_explicit_watchdog_deadline();
  test_timeout_policy_matrix_and_small_harness_utilities();
  test_root_reuse_and_missing_owned_directory_cleanup();
}
