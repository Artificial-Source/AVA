#include "sys.h"
#include "ava/process/environment.h"
#include "ava/process/supervisor.h"
#include "ava/app/clipboard_image.h"
#include "ava/app/clipboard_image_test_support.h"
#include "ava/session/session_store.h"
#include "ava/core/error.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kListTimeout = 1000ms;
constexpr auto kReadTimeout = 3000ms;
constexpr auto kStartupTimeout = 2000ms;
constexpr auto kCleanupBudget = 2s;
constexpr auto kSettlementObservationBudget = 250ms;
constexpr std::uint32_t kStdoutWatch = 1;
constexpr std::size_t kMaxTypeListBytes = 64 * 1024;
constexpr std::array<std::string_view, 4> kSupportedImageMimeTypes = {"image/png", "image/jpeg", "image/webp", "image/gif"};
constexpr std::string_view kTestScenarioMarker = "--ava-clipboard-test-scenario";
constexpr std::string_view kTestLogMarker = "--ava-clipboard-test-log";

struct CapturedCommand
{
  bool ok = false;
  bool too_large = false;
  std::string stdout_data = {};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct HelperTestInjection
{
  std::string executable;
  std::string scenario;
  std::string invocation_log;
  std::chrono::milliseconds preparation_delay = 0ms;
  mutable bool preparation_delay_used = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::optional<std::string> env_value(char const* name)
{
  auto const* value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    return std::nullopt;
  return std::string(value);
}

ava::core::Error clipboard_error(ava::core::ErrorCategory category, std::string message)
{
  return ava::core::Error(category, std::move(message));
}

ava::core::Error clipboard_helper_io_error()
{
  return clipboard_error(ava::core::ErrorCategory::Io, "failed to read clipboard helper output");
}

ava::core::Error clipboard_helper_lifecycle_error()
{
  return clipboard_error(ava::core::ErrorCategory::Io, "clipboard helper process cleanup did not complete");
}

ava::core::Result<testing::ClipboardHelperStatusDisposition> classify_settled_helper_status(ava::process::ExitStatusV1 const& status, bool output_limit)
{
  if (status.cleanup != ava::process::CleanupStateV1::Complete)
    return std::unexpected(clipboard_helper_lifecycle_error());
  if (output_limit)
    return testing::ClipboardHelperStatusDisposition::OutputLimit;
  if (status.reason == ava::process::TerminationReasonV1::NaturalExit && status.kind == ava::process::ExitKindV1::Exited && status.has_exit_code &&
      status.exit_code == 0)
  {
    return testing::ClipboardHelperStatusDisposition::Accepted;
  }
  return testing::ClipboardHelperStatusDisposition::Unavailable;
}

Clock::time_point saturating_add(Clock::time_point value, Clock::duration duration) noexcept
{
  if (duration <= Clock::duration::zero())
    return value;
  auto const maximum = Clock::time_point::max().time_since_epoch();
  if (value.time_since_epoch() > maximum - duration)
    return Clock::time_point::max();
  return value + duration;
}

std::span<std::byte> writable_bytes(std::array<char, 4096>& buffer) noexcept
{
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

ava::core::Result<CapturedCommand> capture_command_stdout(ava::process::ProcessScopeV1 const& session_process_scope, std::vector<std::string> argv,
                                                          std::chrono::milliseconds timeout, std::size_t max_bytes, bool capacity_is_limit,
                                                          HelperTestInjection const* test_injection)
{
  if (argv.empty())
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::InvalidArgument, "clipboard helper command is empty"));

  auto const helper_deadline = saturating_add(Clock::now(), timeout);
  auto const cleanup_deadline = saturating_add(helper_deadline, kCleanupBudget);
  auto const settlement_deadline = saturating_add(cleanup_deadline, kSettlementObservationBudget);

  auto operation = session_process_scope.operation();
  if (!operation)
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::Configuration, "failed to derive clipboard helper process authority"));
  }
  auto environment = ava::process::make_clipboard_desktop_environment_v1(operation->host_environment());
  if (!environment)
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::Configuration, "failed to create clipboard helper process environment"));
  }

  // One test-only delay proves that parent-side authority/environment work can
  // exhaust the same absolute deadline without creating a process record.
  if (test_injection != nullptr && test_injection->preparation_delay > 0ms && !test_injection->preparation_delay_used)
  {
    test_injection->preparation_delay_used = true;
    std::this_thread::sleep_for(test_injection->preparation_delay);
  }
  if (Clock::now() >= helper_deadline)
    return CapturedCommand{};

  auto& supervisor = operation->supervisor();
  auto reservation = supervisor.reserve(operation->owner_prefix(), ava::process::ProcessRoleV1::ClipboardHelper,
                                        {.termination_grace = 0ms, .startup_timeout = kStartupTimeout, .execution_deadline = helper_deadline});
  if (!reservation)
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::Io, "failed to reserve clipboard helper process"));

  auto executable = argv.front();
  if (test_injection != nullptr)
  {
    executable = test_injection->executable;
    argv.emplace_back(kTestScenarioMarker);
    argv.push_back(test_injection->scenario);
    argv.emplace_back(kTestLogMarker);
    argv.push_back(test_injection->invocation_log);
  }

  auto spawned = supervisor.spawn(std::move(*reservation), {.executable = std::move(executable),
                                                            .argv = std::move(argv),
                                                            .environment = std::move(*environment),
                                                            .cwd = "/",
                                                            .stdin_mode = ava::process::StreamModeV1::Discard,
                                                            .stdout_mode = ava::process::StreamModeV1::Capture,
                                                            .stderr_mode = ava::process::StreamModeV1::Discard});
  // Resolution, launch, and exec failures mean that this helper is unavailable.
  // Supervisor has already exactly-once settled the typed reservation, and
  // selection may continue without any legacy process retry.
  if (!spawned)
    return CapturedCommand{};

  auto& handle = spawned->handle;
  if (!spawned->standard_output)
  {
    static_cast<void>(supervisor.request_stop(handle, ava::process::TerminationReasonV1::ProtocolFailure, cleanup_deadline));
    auto settled = supervisor.wait(handle, settlement_deadline);
    if (settled && settled->cleanup != ava::process::CleanupStateV1::Complete)
      return std::unexpected(clipboard_helper_lifecycle_error());
    return std::unexpected(clipboard_helper_io_error());
  }

  auto& output = *spawned->standard_output;
  auto output_watch = output.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
  if (!output_watch)
  {
    output.close();
    static_cast<void>(supervisor.request_stop(handle, ava::process::TerminationReasonV1::ProtocolFailure, cleanup_deadline));
    auto settled = supervisor.wait(handle, settlement_deadline);
    if (settled && settled->cleanup != ava::process::CleanupStateV1::Complete)
      return std::unexpected(clipboard_helper_lifecycle_error());
    return std::unexpected(clipboard_helper_io_error());
  }

  // This deterministic parent-side fault is reachable only through the
  // absolute fake-helper API. It does not alter production selection, argv, or
  // child environment and proves the read-error cleanup path.
  if (test_injection != nullptr && test_injection->scenario == "read-failure")
    output.close();

  CapturedCommand captured;
  std::optional<ava::process::TerminationReasonV1> failure_reason;
  std::optional<ava::core::Error> protocol_error;
  std::optional<ava::process::ExitStatusV1> terminal_status;
  std::optional<Clock::time_point> failure_cleanup_deadline;
  std::optional<Clock::time_point> failure_settlement_deadline;
  bool output_open = true;
  bool output_cap_reached = false;
  bool stop_requested = false;

  auto set_failure = [&](ava::process::TerminationReasonV1 reason) {
    if (failure_reason)
      return;
    failure_reason = reason;
    failure_cleanup_deadline = std::min(cleanup_deadline, saturating_add(Clock::now(), kCleanupBudget));
    failure_settlement_deadline = saturating_add(*failure_cleanup_deadline, kSettlementObservationBudget);
  };
  auto set_protocol_failure = [&] {
    if (failure_reason)
      return;
    set_failure(ava::process::TerminationReasonV1::ProtocolFailure);
    protocol_error.emplace(clipboard_helper_io_error());
  };

  auto request_cleanup = [&] {
    if (!failure_reason || stop_requested)
      return;
    stop_requested = true;
    auto stopped = supervisor.request_stop(handle, *failure_reason, *failure_cleanup_deadline);
    if (!stopped && !protocol_error)
      protocol_error.emplace(clipboard_helper_io_error());
  };

  auto drain_stdout = [&] {
    bool progressed = false;
    std::array<char, 4096> buffer{};
    while (output_open)
    {
      auto read = output.read(writable_bytes(buffer));
      if (!read)
      {
        set_protocol_failure();
        output.close();
        output_open = false;
        break;
      }
      if (read->state == ava::process::PipeIoStateV1::WouldBlock)
        break;
      if (read->state == ava::process::PipeIoStateV1::EndOfStream)
      {
        output.close();
        output_open = false;
        progressed = true;
        break;
      }
      if (read->state != ava::process::PipeIoStateV1::Progress || read->bytes == 0)
      {
        set_protocol_failure();
        output.close();
        output_open = false;
        break;
      }

      progressed = true;
      bool truncated = output_cap_reached;
      if (!output_cap_reached)
      {
        auto const available = captured.stdout_data.size() >= max_bytes ? std::size_t{0} : max_bytes - captured.stdout_data.size();
        auto const retained = std::min(available, read->bytes);
        captured.stdout_data.append(buffer.data(), retained);
        if (retained != read->bytes || (capacity_is_limit && captured.stdout_data.size() == max_bytes))
        {
          output_cap_reached = true;
          truncated = true;
          captured.stdout_data.clear();
          if (!failure_reason)
          {
            captured.too_large = true;
            set_failure(ava::process::TerminationReasonV1::OutputLimit);
          }
        }
      }

      if (auto accounted = supervisor.account_output(handle, ava::process::StreamKindV1::StandardOutput, read->bytes, truncated); !accounted && !failure_reason)
      {
        set_protocol_failure();
      }

      // Continuous output must not monopolize the drain loop past the one
      // reservation-time absolute deadline. Cleanup then continues draining.
      if (!failure_reason && Clock::now() >= helper_deadline)
        set_failure(ava::process::TerminationReasonV1::DeadlineExpired);
      if (failure_reason && !stop_requested)
        break;
    }
    return progressed;
  };

  while (!terminal_status || output_open)
  {
    static_cast<void>(drain_stdout());

    auto waited = supervisor.try_wait(handle);
    if (!waited)
      set_protocol_failure();
    else if (*waited)
      terminal_status = **waited;

    if (terminal_status && output_open)
      static_cast<void>(drain_stdout());
    if (terminal_status && !output_open)
      break;

    if (!failure_reason && Clock::now() >= helper_deadline)
      set_failure(ava::process::TerminationReasonV1::DeadlineExpired);
    request_cleanup();

    if (failure_settlement_deadline && Clock::now() >= *failure_settlement_deadline)
      break;

    auto const watches = output_open ? std::span<ava::process::PipeWatchV1 const>(&*output_watch, 1) : std::span<ava::process::PipeWatchV1 const>{};
    auto const observation_deadline = failure_settlement_deadline.value_or(helper_deadline);
    auto activity = supervisor.wait_for_activity(handle, watches, observation_deadline);
    if (!activity)
    {
      set_protocol_failure();
    }
    else
    {
      auto const invalid_ready =
          std::ranges::any_of(activity->ready, [](ava::process::PipeReadyV1 const& ready) { return ready.token != kStdoutWatch || ready.error; });
      if (invalid_ready)
        set_protocol_failure();
      if (activity->deadline_expired && !failure_reason)
        set_failure(ava::process::TerminationReasonV1::DeadlineExpired);
    }
    request_cleanup();
  }

  auto settled = supervisor.wait(handle, failure_settlement_deadline.value_or(settlement_deadline));
  if (settled)
    terminal_status = *settled;
  else
  {
    set_protocol_failure();
    request_cleanup();
  }

  while (output_open && drain_stdout())
  {
  }
  if (output_open)
  {
    output.close();
    output_open = false;
    set_protocol_failure();
  }

  if (!terminal_status)
    return std::unexpected(clipboard_helper_io_error());

  // Deterministically exercise the otherwise platform-failure-only status
  // decision without weakening the Supervisor's actual cleanup invariant.
  if (test_injection != nullptr && test_injection->scenario == "cleanup-incomplete")
    terminal_status->cleanup = ava::process::CleanupStateV1::Incomplete;

  auto disposition = classify_settled_helper_status(*terminal_status, captured.too_large);
  if (!disposition)
    return std::unexpected(std::move(disposition.error()));
  if (protocol_error)
    return std::unexpected(std::move(*protocol_error));
  if (*disposition == testing::ClipboardHelperStatusDisposition::OutputLimit)
    return captured;

  captured.ok = *disposition == testing::ClipboardHelperStatusDisposition::Accepted;
  return captured;
}

std::string base_mime_type(std::string_view mime_type)
{
  auto const semicolon = mime_type.find(';');
  auto base = std::string(mime_type.substr(0, semicolon == std::string_view::npos ? mime_type.size() : semicolon));
  while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back())) != 0)
    base.pop_back();
  auto begin = std::size_t{0};
  while (begin < base.size() && std::isspace(static_cast<unsigned char>(base[begin])) != 0)
    ++begin;
  if (begin > 0)
    base.erase(0, begin);
  std::ranges::transform(base, base.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return base;
}

std::vector<std::string> split_lines(std::string_view text)
{
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size())
  {
    auto const newline = text.find('\n', start);
    auto const end = newline == std::string_view::npos ? text.size() : newline;
    auto line = base_mime_type(text.substr(start, end - start));
    if (!line.empty())
      lines.push_back(std::move(line));
    if (newline == std::string_view::npos)
      break;
    start = newline + 1;
  }
  return lines;
}

std::optional<std::string> select_preferred_image_mime_type(std::vector<std::string> const& mime_types)
{
  for (auto const preferred : kSupportedImageMimeTypes)
  {
    auto const found = std::ranges::find(mime_types, preferred);
    if (found != mime_types.end())
      return *found;
  }
  return std::nullopt;
}

ava::core::Result<std::optional<std::string>> read_clipboard_data_command(ava::process::ProcessScopeV1 const& session_process_scope,
                                                                          std::vector<std::string> argv, HelperTestInjection const* test_injection)
{
  auto data = capture_command_stdout(session_process_scope, std::move(argv), kReadTimeout, ava::session::kMaxImageAttachmentBytes + 1, true, test_injection);
  if (!data)
    return std::unexpected(std::move(data.error()));
  if (data->too_large)
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::InvalidArgument, "clipboard image is too large"));
  if (!data->ok || data->stdout_data.empty())
    return std::optional<std::string>{};
  return std::optional<std::string>{std::move(data->stdout_data)};
}

ava::core::Result<std::optional<std::string>> read_clipboard_image_wl_paste(ava::process::ProcessScopeV1 const& session_process_scope,
                                                                            HelperTestInjection const* test_injection)
{
  auto types = capture_command_stdout(session_process_scope, {"wl-paste", "--list-types"}, kListTimeout, kMaxTypeListBytes, false, test_injection);
  if (!types)
    return std::unexpected(std::move(types.error()));
  if (!types->ok || types->too_large || types->stdout_data.empty())
    return std::optional<std::string>{};
  auto mime_type = select_preferred_image_mime_type(split_lines(types->stdout_data));
  if (!mime_type)
    return std::optional<std::string>{};
  return read_clipboard_data_command(session_process_scope, {"wl-paste", "--type", *mime_type, "--no-newline"}, test_injection);
}

ava::core::Result<std::optional<std::string>> read_clipboard_image_xclip(ava::process::ProcessScopeV1 const& session_process_scope,
                                                                         HelperTestInjection const* test_injection)
{
  auto targets = capture_command_stdout(session_process_scope, {"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"}, kListTimeout, kMaxTypeListBytes,
                                        false, test_injection);
  if (!targets)
    return std::unexpected(std::move(targets.error()));

  std::vector<std::string> candidates;
  if (targets->ok && !targets->too_large && !targets->stdout_data.empty())
  {
    if (auto preferred = select_preferred_image_mime_type(split_lines(targets->stdout_data)))
      candidates.push_back(std::move(*preferred));
  }
  for (auto const mime_type : kSupportedImageMimeTypes)
  {
    if (std::ranges::find(candidates, mime_type) == candidates.end())
      candidates.emplace_back(mime_type);
  }

  for (auto const& mime_type : candidates)
  {
    auto data = read_clipboard_data_command(session_process_scope, {"xclip", "-selection", "clipboard", "-t", mime_type, "-o"}, test_injection);
    if (!data)
      return std::unexpected(std::move(data.error()));
    if (*data)
      return data;
  }
  return std::optional<std::string>{};
}

ava::core::Result<std::optional<std::string>> read_clipboard_image_bytes(std::optional<ava::process::ProcessScopeV1> const& session_process_scope,
                                                                         HelperTestInjection const* test_injection)
{
  if (env_value("TERMUX_VERSION"))
    return std::optional<std::string>{};

  if (!session_process_scope)
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::Configuration, "clipboard helper process scope is unavailable"));
  }

  if (auto wayland = env_value("WAYLAND_DISPLAY"); wayland)
  {
    auto image = read_clipboard_image_wl_paste(*session_process_scope, test_injection);
    if (!image)
      return std::unexpected(std::move(image.error()));
    if (*image)
      return image;
  }

  auto xclip = read_clipboard_image_xclip(*session_process_scope, test_injection);
  if (!xclip)
    return std::unexpected(std::move(xclip.error()));
  if (*xclip)
    return xclip;

  if (!env_value("WAYLAND_DISPLAY"))
  {
    auto image = read_clipboard_image_wl_paste(*session_process_scope, test_injection);
    if (!image)
      return std::unexpected(std::move(image.error()));
    if (*image)
      return image;
  }
  return std::optional<std::string>{};
}

ava::core::Result<std::optional<ava::session::ImageAttachmentRef>> import_clipboard_image_attachment_impl(
    ava::session::SessionStore const& store, std::optional<ava::process::ProcessScopeV1> const& session_process_scope,
    HelperTestInjection const* test_injection)
{
  if (auto override_file = env_value("AVA_CLIPBOARD_IMAGE_FILE"))
  {
    auto imported = ava::session::import_image_attachment(store, std::filesystem::path(*override_file));
    if (!imported)
      return std::unexpected(std::move(imported.error()));
    return std::optional<ava::session::ImageAttachmentRef>{std::move(*imported)};
  }

  auto bytes = read_clipboard_image_bytes(session_process_scope, test_injection);
  if (!bytes)
    return std::unexpected(std::move(bytes.error()));
  if (!*bytes)
    return std::optional<ava::session::ImageAttachmentRef>{};

  auto imported = ava::session::import_image_attachment_bytes(store, **bytes);
  if (!imported)
    return std::unexpected(std::move(imported.error()));
  return std::optional<ava::session::ImageAttachmentRef>{std::move(*imported)};
}

bool contains_nul(std::string_view value) noexcept
{
  return value.find('\0') != std::string_view::npos;
}

ava::core::Result<HelperTestInjection> make_test_injection(std::filesystem::path const& executable, std::string scenario,
                                                           std::filesystem::path const& invocation_log, std::chrono::milliseconds preparation_delay = 0ms)
{
  auto const executable_value = executable.string();
  auto const log_value = invocation_log.string();
  if (!executable.is_absolute() || executable_value.empty() || contains_nul(executable_value))
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::InvalidArgument, "fake clipboard helper executable must be one absolute path"));
  }
  if (scenario.empty() || scenario.size() > 128 || contains_nul(scenario))
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::InvalidArgument, "fake clipboard helper scenario is invalid"));
  }
  if (!invocation_log.is_absolute() || log_value.empty() || contains_nul(log_value))
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::InvalidArgument, "fake clipboard helper log must be one absolute path"));
  }
  if (preparation_delay < 0ms || preparation_delay > 10s)
  {
    return std::unexpected(clipboard_error(ava::core::ErrorCategory::InvalidArgument, "fake clipboard helper preparation delay is invalid"));
  }

  return HelperTestInjection{
      .executable = executable_value,
      .scenario = std::move(scenario),
      .invocation_log = log_value,
      .preparation_delay = preparation_delay,
  };
}

}  // namespace

ava::core::Result<std::optional<ava::session::ImageAttachmentRef>> import_clipboard_image_attachment(
    ava::session::SessionStore const& store, std::optional<ava::process::ProcessScopeV1> const& session_process_scope)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("importing a clipboard image attachment");
  return import_clipboard_image_attachment_impl(store, session_process_scope, nullptr);
}

ava::core::Result<std::optional<ava::session::ImageAttachmentRef>> testing::ClipboardImageTestAccess::import_with_helper(
    ava::session::SessionStore const& store, std::optional<ava::process::ProcessScopeV1> const& session_process_scope, std::filesystem::path const& executable,
    std::string scenario, std::filesystem::path const& invocation_log)
{
  auto injection = make_test_injection(executable, std::move(scenario), invocation_log);
  if (!injection)
    return std::unexpected(std::move(injection.error()));
  return import_clipboard_image_attachment_impl(store, session_process_scope, &*injection);
}

ava::core::Result<bool> testing::ClipboardImageTestAccess::capture_list_after_preparation_delay(ava::process::ProcessScopeV1 const& session_process_scope,
                                                                                                std::filesystem::path const& executable, std::string scenario,
                                                                                                std::filesystem::path const& invocation_log,
                                                                                                std::chrono::milliseconds preparation_delay)
{
  auto injection = make_test_injection(executable, std::move(scenario), invocation_log, preparation_delay);
  if (!injection)
    return std::unexpected(std::move(injection.error()));
  auto captured = capture_command_stdout(session_process_scope, {"wl-paste", "--list-types"}, kListTimeout, kMaxTypeListBytes, false, &*injection);
  if (!captured)
    return std::unexpected(std::move(captured.error()));
  return captured->ok;
}

ava::core::Result<testing::ClipboardHelperStatusDisposition> testing::ClipboardImageTestAccess::classify_helper_status(ava::process::ExitStatusV1 const& status,
                                                                                                                       bool output_limit)
{
  return classify_settled_helper_status(status, output_limit);
}

}  // namespace ava::app
