#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
  auto const tmpdir = std::filesystem::temp_directory_path();
  auto const impostor = tmpdir / ("ava_core_tests_" + std::to_string(static_cast<unsigned long long>(::getpid())));
  std::error_code create_error;
  std::filesystem::create_directories(impostor, create_error);
  auto const marker = impostor / "not-owned";
  if (!create_error)
    std::ofstream(marker) << "leave me";
  auto const root = temp_root();
  expect(!create_error && root != impostor && std::filesystem::exists(marker),
         "temp_root uniquely creates a namespace and does not adopt an existing pid-named directory");
  std::error_code remove_error;
  std::filesystem::remove_all(impostor, remove_error);
}

void test_cleanup_does_not_remove_untracked_directories()
{
  auto const tmpdir = std::filesystem::temp_directory_path();
  auto const untracked = tmpdir / ("ava-harness-untracked-" + std::to_string(static_cast<unsigned long long>(::getpid())));
  std::error_code create_error;
  std::filesystem::create_directories(untracked, create_error);
  {
    auto owned = ScopedTestDirectory::create_unique(tmpdir, "ava-harness-tracked-");
    std::ofstream(owned.path() / "owned.txt") << "owned";
  }
  expect(!create_error && std::filesystem::is_directory(untracked), "teardown removes only directories acquired by this process and leaves untracked paths");
  std::error_code remove_error;
  std::filesystem::remove_all(untracked, remove_error);
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
}
