#include "sys.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/process/environment.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/clipboard_image.h"
#include "ava/app/clipboard_image_test_support.h"
#include "ava/session/attachments.h"
#include "ava/session/session_store.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AVA_FAKE_CLIPBOARD_CHILD_PATH
#define AVA_FAKE_CLIPBOARD_CHILD_PATH ""
#endif

namespace {

using namespace std::chrono_literals;

ava::process::ProcessScopeV1 require_session_scope(std::shared_ptr<ava::process::Supervisor> const& supervisor)
{
  auto application = ava::process::ProcessScopeV1::application(supervisor);
  if (!application)
    throw std::runtime_error(application.error().format());
  auto session = application->session();
  if (!session)
    throw std::runtime_error(session.error().format());
  return *session;
}

std::string tiny_png()
{
  std::string bytes;
  bytes.push_back(static_cast<char>(0x89));
  bytes += "PNG\r\n";
  bytes.push_back(static_cast<char>(0x1A));
  bytes += "\nava-clipboard-image";
  return bytes;
}

class ClipboardFixture final
{
 public:
  explicit ClipboardFixture(std::string_view label)
      : root(ScopedTestDirectory::create_unique(temp_root(), "clipboard-process-" + std::string(label) + "-")),
        workspace(root / "workspace"),
        store({.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "clipboard-session"}),
        supervisor(std::make_shared<ava::process::Supervisor>()),
        scope(require_session_scope(supervisor)),
        log(root / "invocations.log")
  {
    std::filesystem::create_directories(workspace);
  }

  [[nodiscard]] ava::core::Result<std::optional<ava::session::ImageAttachmentRef>> import(std::string scenario)
  {
    return ava::app::testing::ClipboardImageTestAccess::import_with_helper(store, scope, AVA_FAKE_CLIPBOARD_CHILD_PATH, std::move(scenario), log);
  }

  [[nodiscard]] ava::process::ProcessSnapshotV1 snapshot() const { return supervisor->snapshot(); }

  ScopedTestDirectory root;
  std::filesystem::path workspace;
  ava::session::SessionStore store;
  std::shared_ptr<ava::process::Supervisor> supervisor;
  ava::process::ProcessScopeV1 scope;
  std::filesystem::path log;
};

struct LoggedInvocation
{
  std::vector<std::string> arguments;
  std::map<std::string, std::string> environment;
};

std::vector<LoggedInvocation> read_invocation_log(std::filesystem::path const& path)
{
  std::ifstream input(path);
  std::vector<LoggedInvocation> result;
  std::optional<LoggedInvocation> current;
  std::string line;
  while (std::getline(input, line))
  {
    if (line == "BEGIN")
    {
      current.emplace();
      continue;
    }
    if (line == "END")
    {
      if (current)
        result.push_back(std::move(*current));
      current.reset();
      continue;
    }
    if (!current)
      continue;
    if (line.starts_with("ARG:"))
    {
      current->arguments.push_back(line.substr(4));
      continue;
    }
    if (line.starts_with("ENV:"))
    {
      auto const separator = line.find('=', 4);
      if (separator != std::string::npos)
        current->environment.emplace(line.substr(4, separator - 4), line.substr(separator + 1));
    }
  }
  return result;
}

bool no_waitable_immediate_child()
{
  errno = 0;
  int status = 0;
  auto const child = ::waitpid(-1, &status, WNOHANG);
  return child == -1 && errno == ECHILD;
}

bool all_records_settled(ava::process::ProcessSnapshotV1 const& snapshot, std::size_t count)
{
  if (snapshot.records.size() != count || snapshot.live_records != 0)
    return false;
  std::set<std::uint64_t> owners;
  for (auto const& record : snapshot.records)
  {
    if (record.role != ava::process::ProcessRoleV1::ClipboardHelper || record.state != ava::process::ProcessStateV1::Finished ||
        record.cleanup != ava::process::CleanupStateV1::Complete || record.settlement_count != 1)
    {
      return false;
    }
    owners.insert(record.owner_alias);
  }
  return owners.size() == count;
}

bool successful_import(ClipboardFixture const& fixture, ava::core::Result<std::optional<ava::session::ImageAttachmentRef>> const& result,
                       std::string const& expected = tiny_png())
{
  if (!ava::tests::require(result && result->has_value(), "clipboard import produces an attachment"))
    return false;
  auto loaded = ava::session::load_image_attachment(fixture.store, **result);
  if (!ava::tests::require(loaded.has_value(), "clipboard attachment can be read through the session attachment API"))
    return false;
  ava::tests::expect_eq(loaded->bytes, expected, "clipboard attachment retains every imported image byte");
  ava::tests::expect_eq((**result).byte_size, expected.size(), "clipboard attachment metadata records exact image size");
  bool clean = true;
  for (auto const& entry : std::filesystem::recursive_directory_iterator(fixture.root.path()))
  {
    if (entry.is_regular_file() && entry.path() != loaded->path && entry.path() != fixture.log && entry.path() != fixture.workspace / "override.png")
    {
      expect(false, "clipboard import leaves no stray artifact: " + entry.path().string());
      clean = false;
    }
  }
  return loaded->bytes == expected && (**result).mime_type == "image/png" && (**result).byte_size == expected.size() && clean;
}

void test_settled_status_requires_complete_cleanup()
{
  using Disposition = ava::app::testing::ClipboardHelperStatusDisposition;

  ava::process::ExitStatusV1 success{
      .reason = ava::process::TerminationReasonV1::NaturalExit,
      .kind = ava::process::ExitKindV1::Exited,
      .cleanup = ava::process::CleanupStateV1::Complete,
      .exit_code = 0,
      .has_exit_code = true,
  };
  auto accepted = ava::app::testing::ClipboardImageTestAccess::classify_helper_status(success, false);

  auto nonzero = success;
  nonzero.exit_code = 7;
  auto unavailable = ava::app::testing::ClipboardImageTestAccess::classify_helper_status(nonzero, false);

  ava::process::ExitStatusV1 output_limit{
      .reason = ava::process::TerminationReasonV1::OutputLimit,
      .kind = ava::process::ExitKindV1::Signaled,
      .cleanup = ava::process::CleanupStateV1::Complete,
      .signal_number = SIGTERM,
      .has_signal_number = true,
  };
  auto limited = ava::app::testing::ClipboardImageTestAccess::classify_helper_status(output_limit, true);
  expect(accepted && *accepted == Disposition::Accepted && unavailable && *unavailable == Disposition::Unavailable && limited &&
             *limited == Disposition::OutputLimit,
         "complete helper cleanup preserves success, ordinary nonzero fallback, and the specific output-limit disposition");

  success.cleanup = ava::process::CleanupStateV1::Incomplete;
  nonzero.cleanup = ava::process::CleanupStateV1::Incomplete;
  output_limit.cleanup = ava::process::CleanupStateV1::Incomplete;
  auto incomplete_success = ava::app::testing::ClipboardImageTestAccess::classify_helper_status(success, false);
  auto incomplete_nonzero = ava::app::testing::ClipboardImageTestAccess::classify_helper_status(nonzero, false);
  auto incomplete_limit = ava::app::testing::ClipboardImageTestAccess::classify_helper_status(output_limit, true);
  auto is_lifecycle_error = [](auto const& result) {
    return !result && result.error().category() == ava::core::ErrorCategory::Io &&
           result.error().message() == "clipboard helper process cleanup did not complete";
  };
  expect(is_lifecycle_error(incomplete_success) && is_lifecycle_error(incomplete_nonzero) && is_lifecycle_error(incomplete_limit),
         "incomplete cleanup rejects success bytes, nonzero fallback, and output-limit classification with one content-free lifecycle error");
}

void test_pre_reservation_deadline_is_unavailable_without_record()
{
  ClipboardFixture fixture("pre-reservation-deadline");
  auto result = ava::app::testing::ClipboardImageTestAccess::capture_list_after_preparation_delay(fixture.scope, AVA_FAKE_CLIPBOARD_CHILD_PATH,
                                                                                                  "pre-reservation-deadline", fixture.log, 1100ms);
  auto const snapshot = fixture.snapshot();
  ava::tests::expect_all("preparation exhausting the helper deadline is unavailable before any process authority is created",
                         {{"unavailable", result && !*result},
                          {"no records", snapshot.records.empty()},
                          {"no live records", snapshot.live_records == 0},
                          {"no monitor", !snapshot.monitor_started},
                          {"no helper invocation", !std::filesystem::exists(fixture.log)},
                          {"no waitable child", no_waitable_immediate_child()}});
}

void test_file_override_and_no_helper_branches_need_no_scope()
{
  ClipboardFixture fixture("no-scope");

  auto const image_path = fixture.workspace / "override.png";
  ava::tests::write_test_file(image_path, tiny_png());
  ScopedEnvVar environment_245("AVA_CLIPBOARD_IMAGE_FILE", image_path.c_str());
  auto overridden = ava::app::import_clipboard_image_attachment(fixture.store, std::nullopt);
  auto snapshot = fixture.snapshot();
  expect(successful_import(fixture, overridden) && snapshot.records.empty() && snapshot.live_records == 0 && !snapshot.monitor_started,
         "clipboard file override imports with missing process scope and starts no reservation or monitor");

  ScopedEnvVar environment_251("AVA_CLIPBOARD_IMAGE_FILE", std::nullopt);
  ScopedEnvVar environment_252("TERMUX_VERSION", "termux-parent-only");
  auto termux =
      ava::app::testing::ClipboardImageTestAccess::import_with_helper(fixture.store, std::nullopt, AVA_FAKE_CLIPBOARD_CHILD_PATH, "success", fixture.log);
  snapshot = fixture.snapshot();
  expect(termux && !*termux && snapshot.records.empty() && snapshot.live_records == 0 && !snapshot.monitor_started && !std::filesystem::exists(fixture.log),
         "TERMUX clipboard short circuit needs no process scope and starts no helper authority");

  ScopedEnvVar environment_259("TERMUX_VERSION", std::nullopt);
  auto missing =
      ava::app::testing::ClipboardImageTestAccess::import_with_helper(fixture.store, std::nullopt, AVA_FAKE_CLIPBOARD_CHILD_PATH, "success", fixture.log);
  snapshot = fixture.snapshot();
  expect(!missing && missing.error().category() == ava::core::ErrorCategory::Configuration &&
             missing.error().message() == "clipboard helper process scope is unavailable" && snapshot.records.empty() && snapshot.live_records == 0 &&
             !snapshot.monitor_started && !std::filesystem::exists(fixture.log),
         "real clipboard helper selection rejects missing session scope before reservation");
}

void test_exact_wayland_and_xclip_argv_order()
{
  ScopedEnvVar environment_272("WAYLAND_DISPLAY", "wayland-test");
  ClipboardFixture wayland("wayland-order");
  auto wayland_result = wayland.import("wayland-order");
  auto const wayland_log = read_invocation_log(wayland.log);
  auto const wayland_snapshot = wayland.snapshot();
  expect(successful_import(wayland, wayland_result) && wayland_log.size() == 2 &&
             wayland_log[0].arguments == std::vector<std::string>{"wl-paste", "--list-types"} &&
             wayland_log[1].arguments == std::vector<std::string>{"wl-paste", "--type", "image/png", "--no-newline"},
         "Wayland clipboard list/data helpers preserve exact argv and PNG-first MIME priority");
  expect(all_records_settled(wayland_snapshot, 2) && no_waitable_immediate_child(),
         "Wayland list/data helpers each use one distinct operation and settle one complete ClipboardHelper record");

  ScopedEnvVar environment_283("WAYLAND_DISPLAY", std::nullopt);
  ClipboardFixture xclip("xclip-order");
  auto xclip_result = xclip.import("xclip-order");
  auto const xclip_log = read_invocation_log(xclip.log);
  auto const xclip_snapshot = xclip.snapshot();
  std::vector<std::vector<std::string>> const expected{
      {"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"},
      {"xclip", "-selection", "clipboard", "-t", "image/jpeg", "-o"},
      {"xclip", "-selection", "clipboard", "-t", "image/png", "-o"},
      {"xclip", "-selection", "clipboard", "-t", "image/webp", "-o"},
  };
  std::vector<std::vector<std::string>> observed;
  for (auto const& invocation : xclip_log)
    observed.push_back(invocation.arguments);
  expect(successful_import(xclip, xclip_result) && observed == expected,
         "xclip preserves TARGETS/data fallback order, JPEG priority, MIME deduplication, nonzero fallback, and empty fallback");
  expect(all_records_settled(xclip_snapshot, 4) && xclip_snapshot.records[1].has_exit_code && xclip_snapshot.records[1].exit_code == 7 &&
             xclip_snapshot.records[2].has_exit_code && xclip_snapshot.records[2].exit_code == 0 && no_waitable_immediate_child(),
         "xclip nonzero and empty data attempts remain natural exactly-once records before successful fallback");
}

bool clipboard_environment_name_allowed(std::string_view name)
{
  static std::set<std::string_view> const fixed{
      "PATH",
      "HOME",
      "USER",
      "LOGNAME",
      "TMPDIR",
      "TMP",
      "TEMP",
      "LANG",
      "LANGUAGE",
      "LC_ALL",
      "XDG_RUNTIME_DIR",
      "DISPLAY",
      "WAYLAND_DISPLAY",
      "XAUTHORITY",
      "DBUS_SESSION_BUS_ADDRESS",
  };
  return fixed.contains(name) || (name.starts_with("LC_") && name != "LC_ALL");
}

void test_exact_clipboard_environment_capture()
{
  ScopedEnvVar environment_329("PATH", "/captured/not/forwarded");
  ScopedEnvVar environment_330("HOME", "/captured/home");
  ScopedEnvVar environment_331("USER", "captured-user");
  ScopedEnvVar environment_332("LOGNAME", "captured-logname");
  ScopedEnvVar environment_333("TMPDIR", "/tmp");
  ScopedEnvVar environment_334("TMP", "/captured/tmp");
  ScopedEnvVar environment_335("TEMP", "/captured/temp");
  ScopedEnvVar environment_336("LANG", "captured-lang");
  ScopedEnvVar environment_337("LANGUAGE", "captured-language");
  ScopedEnvVar environment_338("LC_ALL", "captured-lc-all");
  ScopedEnvVar environment_339("LC_CLIPBOARD_TEST", "captured-locale");
  ScopedEnvVar environment_340("XDG_RUNTIME_DIR", "/captured/runtime");
  ScopedEnvVar environment_341("DISPLAY", ":captured");
  ScopedEnvVar environment_342("WAYLAND_DISPLAY", "captured-wayland");
  ScopedEnvVar environment_343("XAUTHORITY", "/captured/xauthority");
  ScopedEnvVar environment_344("DBUS_SESSION_BUS_ADDRESS", "unix:path=/captured/dbus");
  ScopedEnvVar environment_345("HTTPS_PROXY", "https://proxy-canary.invalid");
  ScopedEnvVar environment_346("CURL_CA_BUNDLE", "/ca-canary.pem");
  ScopedEnvVar environment_347("OPENAI_API_KEY", "PROVIDER_CANARY");
  ScopedEnvVar environment_348("AWS_SECRET_ACCESS_KEY", "CLOUD_CANARY");
  ScopedEnvVar environment_349("LD_PRELOAD", "/loader-canary.so");
  ScopedEnvVar environment_350("GIT_ASKPASS", "/askpass-canary");
  ScopedEnvVar environment_351("SSH_AUTH_SOCK", "/agent-canary");
  ScopedEnvVar environment_352("AVA_CLIPBOARD_SECRET", "ARBITRARY_AVA_CANARY");
  ScopedEnvVar environment_353("AVA_CLIPBOARD_TEST_SCENARIO", "AMBIENT_TEST_CANARY");
  ScopedEnvVar environment_354("TERMUX_VERSION", "PARENT_SELECTION_CANARY");
  ScopedEnvVar environment_355("AVA_CLIPBOARD_IMAGE_FILE", "/parent/selection/canary");

  ClipboardFixture fixture("environment");

  ScopedEnvVar environment_359("HOME", "/mutated/home");
  ScopedEnvVar environment_360("DISPLAY", ":mutated");
  ScopedEnvVar environment_361("WAYLAND_DISPLAY", "mutated-wayland");
  ScopedEnvVar environment_362("DBUS_SESSION_BUS_ADDRESS", "unix:path=/mutated/dbus");
  ScopedEnvVar environment_363("TERMUX_VERSION", std::nullopt);
  ScopedEnvVar environment_364("AVA_CLIPBOARD_IMAGE_FILE", std::nullopt);

  auto result = fixture.import("environment");
  auto const invocations = read_invocation_log(fixture.log);
  bool exact = successful_import(fixture, result) && invocations.size() == 2;
  for (auto const& invocation : invocations)
  {
    auto const& environment = invocation.environment;
    exact = exact && std::ranges::all_of(environment, [](auto const& variable) { return clipboard_environment_name_allowed(variable.first); });
    auto value_is = [&](std::string const& name, std::string const& value) {
      auto const found = environment.find(name);
      return found != environment.end() && found->second == value;
    };
    exact = exact && value_is("PATH", std::string(ava::process::kTrustedEnvironmentPathV1)) && value_is("HOME", "/captured/home") &&
            value_is("USER", "captured-user") && value_is("LOGNAME", "captured-logname") && value_is("TMPDIR", "/tmp") && value_is("TMP", "/captured/tmp") &&
            value_is("TEMP", "/captured/temp") && value_is("LANG", "captured-lang") && value_is("LANGUAGE", "captured-language") &&
            value_is("LC_ALL", "captured-lc-all") && value_is("LC_CLIPBOARD_TEST", "captured-locale") && value_is("XDG_RUNTIME_DIR", "/captured/runtime") &&
            value_is("DISPLAY", ":captured") && value_is("WAYLAND_DISPLAY", "captured-wayland") && value_is("XAUTHORITY", "/captured/xauthority") &&
            value_is("DBUS_SESSION_BUS_ADDRESS", "unix:path=/captured/dbus");
    for (auto const forbidden : {"HTTPS_PROXY", "CURL_CA_BUNDLE", "OPENAI_API_KEY", "AWS_SECRET_ACCESS_KEY", "LD_PRELOAD", "GIT_ASKPASS", "SSH_AUTH_SOCK",
                                 "AVA_CLIPBOARD_SECRET", "AVA_CLIPBOARD_TEST_SCENARIO", "TERMUX_VERSION", "AVA_CLIPBOARD_IMAGE_FILE"})
    {
      exact = exact && !environment.contains(forbidden);
    }
    for (auto const mutated : {"/mutated/home", ":mutated", "mutated-wayland", "unix:path=/mutated/dbus"})
    {
      exact = exact && std::ranges::none_of(environment, [&](auto const& variable) { return variable.second == mutated; });
    }
  }
  expect(exact,
         "ava-clipboard-desktop-v1 forwards exactly captured desktop values and excludes proxy, CA, provider, cloud, loader, askpass, agent, AVA, and "
         "parent-only selection/test values");
  expect(all_records_settled(fixture.snapshot(), 2), "clipboard environment inspection helpers remain completely and exactly-once supervised");
}

void test_buffered_hup_and_absolute_deadlines()
{
  ScopedEnvVar environment_402("WAYLAND_DISPLAY", "wayland-buffered");
  ClipboardFixture buffered("buffered-hup");
  auto buffered_result = buffered.import("buffered-hup");
  auto const buffered_snapshot = buffered.snapshot();
  auto buffered_bytes = tiny_png();
  buffered_bytes.resize(32U * 1024U, 'i');
  ava::tests::expect_all("clipboard drains every buffered HUP byte before complete settlement",
                         {{"imported exact bytes", successful_import(buffered, buffered_result, buffered_bytes)},
                          {"two settled helpers", all_records_settled(buffered_snapshot, 2)},
                          {"raw image size", buffered_snapshot.records.size() == 2 && buffered_snapshot.records[1].stdout_bytes == 32U * 1024U},
                          {"not truncated", buffered_snapshot.records.size() == 2 && !buffered_snapshot.records[1].stdout_truncated}});

  ScopedEnvVar environment_412("WAYLAND_DISPLAY", std::nullopt);
  ClipboardFixture list_deadline("list-deadline");
  auto const list_started = std::chrono::steady_clock::now();
  auto list_result = list_deadline.import("list-deadline");
  auto const list_elapsed = std::chrono::steady_clock::now() - list_started;
  auto const list_snapshot = list_deadline.snapshot();
  ava::tests::expect_all(
      "clipboard list progress retains the one-second absolute deadline before successful fallback",
      {{"fallback bytes", successful_import(list_deadline, list_result)},
       {"budget not short-circuited", list_elapsed >= 900ms},
       {"two settled helpers", all_records_settled(list_snapshot, 2)},
       {"list deadline", !list_snapshot.records.empty() && list_snapshot.records[0].reason == ava::process::TerminationReasonV1::DeadlineExpired},
       {"list progress", !list_snapshot.records.empty() && list_snapshot.records[0].stdout_bytes > 0},
       {"not truncated", !list_snapshot.records.empty() && !list_snapshot.records[0].stdout_truncated}});

  ClipboardFixture image_deadline("image-deadline");
  auto const image_started = std::chrono::steady_clock::now();
  auto image_result = image_deadline.import("term-refusal");
  auto const image_elapsed = std::chrono::steady_clock::now() - image_started;
  auto const image_snapshot = image_deadline.snapshot();
  ava::tests::expect_all(
      "clipboard image progress expires the three-second deadline and kills the ready TERM-refusing helper before fallback",
      {{"fallback bytes", successful_import(image_deadline, image_result)},
       {"budget not short-circuited", image_elapsed >= 2900ms},
       {"three settled helpers", all_records_settled(image_snapshot, 3)},
       {"image deadline", image_snapshot.records.size() > 1 && image_snapshot.records[1].reason == ava::process::TerminationReasonV1::DeadlineExpired},
       {"image progress", image_snapshot.records.size() > 1 && image_snapshot.records[1].stdout_bytes > 0},
       {"not truncated", image_snapshot.records.size() > 1 && !image_snapshot.records[1].stdout_truncated},
       {"SIGKILL observed",
        image_snapshot.records.size() > 1 && image_snapshot.records[1].has_signal_number && image_snapshot.records[1].signal_number == SIGKILL},
       {"no waitable child", no_waitable_immediate_child()}});
}

void test_output_limits_and_group_cleanup()
{
  ScopedEnvVar environment_436("WAYLAND_DISPLAY", std::nullopt);

  ClipboardFixture list_limit("list-limit");
  auto list_result = list_limit.import("list-limit");
  auto const list_snapshot = list_limit.snapshot();
  expect(successful_import(list_limit, list_result) && all_records_settled(list_snapshot, 2) &&
             list_snapshot.records[0].reason == ava::process::TerminationReasonV1::OutputLimit && list_snapshot.records[0].stdout_bytes > 64U * 1024U &&
             list_snapshot.records[0].stdout_truncated,
         "oversized clipboard target list requests OutputLimit, accounts the drained bytes, and falls back without retaining truncation");

  ClipboardFixture image_limit("image-limit");
  auto image_result = image_limit.import("image-limit");
  auto const image_snapshot = image_limit.snapshot();
  auto const formatted = image_result ? std::string{} : image_result.error().format();
  expect(!image_result && image_result.error().message() == "clipboard image is too large" && formatted.find("ARBITRARY_AVA_CANARY") == std::string::npos &&
             all_records_settled(image_snapshot, 2) && image_snapshot.records[1].reason == ava::process::TerminationReasonV1::OutputLimit &&
             image_snapshot.records[1].stdout_bytes == ava::session::kMaxImageAttachmentBytes + 1 && image_snapshot.records[1].stdout_truncated,
         "oversized clipboard image requests OutputLimit with full observed accounting and preserves the actionable final size error");

  ScopedEnvVar environment_455("WAYLAND_DISPLAY", "wayland-descendant");
  ClipboardFixture descendant("descendant");
  auto descendant_result = descendant.import("descendant");
  auto const descendant_snapshot = descendant.snapshot();
  ava::tests::expect_all("clipboard cleans a same-group descendant before EOF and complete natural settlement",
                         {{"exact image bytes", successful_import(descendant, descendant_result)},
                          {"two settled helpers", all_records_settled(descendant_snapshot, 2)},
                          {"natural leader reason",
                           descendant_snapshot.records.size() == 2 && descendant_snapshot.records[1].reason == ava::process::TerminationReasonV1::NaturalExit},
                          {"no waitable child", no_waitable_immediate_child()}});
}

void test_incomplete_cleanup_stops_helper_fallback()
{
  ScopedEnvVar environment_466("WAYLAND_DISPLAY", std::nullopt);

  ClipboardFixture fixture("cleanup-incomplete");
  auto result = fixture.import("cleanup-incomplete");
  auto const invocations = read_invocation_log(fixture.log);
  auto const snapshot = fixture.snapshot();
  auto const formatted = result ? std::string{} : result.error().format();
  expect(!result && result.error().category() == ava::core::ErrorCategory::Io &&
             result.error().message() == "clipboard helper process cleanup did not complete" && invocations.size() == 1 &&
             invocations[0].arguments == std::vector<std::string>{"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"} &&
             all_records_settled(snapshot, 1) && formatted.find("cleanup-incomplete") == std::string::npos &&
             formatted.find("image/png") == std::string::npos && no_waitable_immediate_child(),
         "an injected incomplete settlement returns a content-free lifecycle error without importing bytes or trying another helper");
}

void test_exec_and_read_failures_have_no_legacy_retry()
{
  ScopedEnvVar environment_483("WAYLAND_DISPLAY", std::nullopt);

  ClipboardFixture exec_failure("exec-failure");
  auto const malformed = exec_failure.root / "malformed-helper";
  ava::tests::write_test_file(malformed, "not an executable image\n", 0700);
  auto exec_result = ava::app::testing::ClipboardImageTestAccess::import_with_helper(exec_failure.store, exec_failure.scope, malformed, "EXEC_FAILURE_CANARY",
                                                                                     exec_failure.log);
  auto const exec_snapshot = exec_failure.snapshot();
  bool exec_records =
      exec_result && !*exec_result && exec_snapshot.records.size() == 6 && exec_snapshot.live_records == 0 && !std::filesystem::exists(exec_failure.log);
  for (auto const& record : exec_snapshot.records)
  {
    exec_records = exec_records && record.role == ava::process::ProcessRoleV1::ClipboardHelper && record.state == ava::process::ProcessStateV1::Finished &&
                   record.reason == ava::process::TerminationReasonV1::ExecFailed && record.cleanup == ava::process::CleanupStateV1::Complete &&
                   record.settlement_count == 1;
  }
  expect(exec_records && no_waitable_immediate_child(),
         "clipboard exec failure continues fixed helper selection through Supervisor records without any legacy process fallback");

  ScopedEnvVar environment_506("WAYLAND_DISPLAY", "wayland-read-failure");
  ClipboardFixture read_failure("read-failure");
  auto read_result = read_failure.import("read-failure");
  auto const read_snapshot = read_failure.snapshot();
  auto const read_error = read_result ? std::string{} : read_result.error().format();
  expect(!read_result && read_result.error().category() == ava::core::ErrorCategory::Io &&
             read_result.error().message() == "failed to read clipboard helper output" && read_error.find("read-failure") == std::string::npos &&
             all_records_settled(read_snapshot, 1) && read_snapshot.records[0].reason == ava::process::TerminationReasonV1::ProtocolFailure &&
             no_waitable_immediate_child(),
         "clipboard read failure requests ProtocolFailure, settles once, and omits scenario/output canaries from errors");
}

}  // namespace

void run_clipboard_image_process_tests()
{
  ava::tests::HermeticTestEnvironment environment("clipboard-process-");
  ScopedEnvVar file_override("AVA_CLIPBOARD_IMAGE_FILE", std::nullopt);
  ScopedEnvVar termux("TERMUX_VERSION", std::nullopt);
  ScopedEnvVar wayland("WAYLAND_DISPLAY", std::nullopt);
  expect(std::string_view(AVA_FAKE_CLIPBOARD_CHILD_PATH).starts_with('/'), "repository-owned fake clipboard child has an absolute test path");
  test_settled_status_requires_complete_cleanup();
  test_pre_reservation_deadline_is_unavailable_without_record();
  test_file_override_and_no_helper_branches_need_no_scope();
  test_exact_wayland_and_xclip_argv_order();
  test_exact_clipboard_environment_capture();
  test_buffered_hup_and_absolute_deadlines();
  test_output_limits_and_group_cleanup();
  test_incomplete_cleanup_stops_helper_fallback();
  test_exec_and_read_failures_have_no_legacy_retry();
}
