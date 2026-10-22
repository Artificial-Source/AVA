#include "sys.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/runner_support.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::plugin {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kMaxDrainReadsPerObservation = 16;
constexpr auto kCancellationObservationInterval = 10ms;
constexpr auto kNaturalStatusObservation = 50ms;
constexpr std::uint32_t kInputWatch = 1;
constexpr std::uint32_t kStdoutWatch = 2;
constexpr std::uint32_t kStderrWatch = 3;

std::span<std::byte> writable_bytes(std::array<char, 4096>& buffer) noexcept
{
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

std::span<std::byte const> readable_bytes(std::string_view value) noexcept
{
  return {reinterpret_cast<std::byte const*>(value.data()), value.size()};
}

bool output_limit_error(ava::core::Error const& error)
{
  return std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& context) { return context.key == "output_limit" && context.value == "true"; });
}

ava::core::Error process_io_error(std::string message, PluginManifest const& manifest)
{
  return plugin_error(ava::core::ErrorCategory::Io, std::move(message), manifest);
}

std::size_t queued_stdout_byte_limit(std::size_t max_record_bytes) noexcept
{
  if (max_record_bytes > std::numeric_limits<std::size_t>::max() / kPluginRunnerQueuedByteMultiplier)
    return std::numeric_limits<std::size_t>::max();
  return max_record_bytes * kPluginRunnerQueuedByteMultiplier;
}

ava::core::Error stdout_limit_error(std::string message, PluginManifest const& manifest, PluginRunnerOptions const& options, std::size_t queued_bytes,
                                    std::size_t queued_records)
{
  auto error = protocol_error(std::move(message), manifest);
  error.with_context("max_bytes", std::to_string(options.max_record_bytes));
  error.with_context("queued_bytes", std::to_string(queued_bytes));
  error.with_context("queued_records", std::to_string(queued_records));
  error.with_context("max_queued_bytes", std::to_string(queued_stdout_byte_limit(options.max_record_bytes)));
  error.with_context("max_queued_records", std::to_string(kPluginRunnerQueuedRecordCap));
  error.with_context("output_limit", "true");
  return error;
}

ava::core::VoidResult validate_stdout_queue(std::string const& buffered, PluginManifest const& manifest, PluginRunnerOptions const& options)
{
  std::size_t record_begin = 0;
  std::size_t record_count = 0;
  while (true)
  {
    auto const newline = buffered.find('\n', record_begin);
    if (newline == std::string::npos)
      break;
    ++record_count;
    if (newline - record_begin > options.max_record_bytes)
      return std::unexpected(stdout_limit_error("plugin protocol record exceeds size cap", manifest, options, buffered.size(), record_count));
    if (record_count > kPluginRunnerQueuedRecordCap)
      return std::unexpected(stdout_limit_error("plugin protocol queued record count exceeds cap", manifest, options, buffered.size(), record_count));
    record_begin = newline + 1;
  }
  if (buffered.size() - record_begin > options.max_record_bytes)
    return std::unexpected(stdout_limit_error("plugin protocol record exceeds size cap", manifest, options, buffered.size(), record_count));
  if (buffered.size() > queued_stdout_byte_limit(options.max_record_bytes))
    return std::unexpected(stdout_limit_error("plugin protocol queued bytes exceed cap", manifest, options, buffered.size(), record_count));
  return {};
}

std::optional<ava::process::TerminationReasonV1> interruption_reason(Clock::time_point deadline, CancelCallback const& cancel_requested) noexcept
{
  if (Clock::now() >= deadline)
    return ava::process::TerminationReasonV1::DeadlineExpired;
  if (is_canceled(cancel_requested))
    return ava::process::TerminationReasonV1::Canceled;
  if (Clock::now() >= deadline)
    return ava::process::TerminationReasonV1::DeadlineExpired;
  return std::nullopt;
}

void add_bounded_stream_metadata(ava::core::Error& error, std::string const& stdout_buffer, std::string const& stderr_tail, bool stderr_truncated)
{
  error.with_context("response_bytes", std::to_string(stdout_buffer.size()));
  error.with_context("stderr_bytes", std::to_string(stderr_tail.size()));
  error.with_context("stderr_truncated", stderr_truncated ? "true" : "false");
}

}  // namespace

ava::core::VoidResult PluginProcess::write_record(std::string_view record, Clock::time_point deadline, std::chrono::milliseconds timeout,
                                                  std::string_view timeout_message, CancelCallback cancel_requested)
{
  if (record.size() > options_.max_record_bytes || record.size() == std::numeric_limits<std::size_t>::max())
  {
    auto error = protocol_error("plugin outbound protocol record exceeds size cap", manifest_);
    error.with_context("record_bytes", std::to_string(record.size()));
    error.with_context("max_bytes", std::to_string(options_.max_record_bytes));
    error.with_context("output_limit", "true");
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(error)));
  }
  if (!standard_input_.valid())
  {
    auto error = protocol_error("plugin stdin is closed", manifest_);
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }

  std::string frame;
  try
  {
    // Caller-provided JSON may be pretty-printed, but every transport record must occupy one line.
    auto compacted = ava::core::json::compact(record);
    if (!compacted)
    {
      auto error = protocol_error("plugin outbound protocol record contains an unescaped line break", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    frame = std::move(*compacted);
    frame.push_back('\n');
  }
  catch (...)
  {
    auto error = protocol_error("failed to retain a bounded plugin request", manifest_);
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }

  Dout(dc::plugin, "operation=record state=write bytes=" << record.size());
  std::size_t offset = 0;
  while (offset < frame.size())
  {
    if (auto interrupted = interruption_reason(deadline, cancel_requested))
    {
      if (*interrupted == ava::process::TerminationReasonV1::Canceled)
        return std::unexpected(fail_process(*interrupted, canceled_error("plugin request canceled", manifest_)));
      auto error = protocol_error(std::string(timeout_message), manifest_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      return std::unexpected(fail_process(*interrupted, std::move(error)));
    }

    auto written = standard_input_.write(readable_bytes(std::string_view(frame).substr(offset)));
    if (!written)
    {
      auto error = process_io_error("failed to write plugin request", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    if (written->state == ava::process::PipeIoStateV1::Progress && written->bytes != 0)
    {
      offset += written->bytes;
      continue;
    }
    if (written->state == ava::process::PipeIoStateV1::WouldBlock)
    {
      if (auto writable = wait_for_writable(deadline, timeout, timeout_message, cancel_requested); !writable)
        return std::unexpected(std::move(writable.error()));
      continue;
    }
    auto error = protocol_error("plugin request pipe closed", manifest_);
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
  return {};
}

ava::core::Result<std::string> PluginProcess::read_record(Clock::time_point deadline, std::chrono::milliseconds timeout, std::string_view timeout_message,
                                                          std::string_view closed_message, CancelCallback cancel_requested)
{
  while (true)
  {
    if (is_canceled(cancel_requested))
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, canceled_error("plugin request canceled", manifest_)));

    if (auto valid = validate_stdout_queue(stdout_buffer_, manifest_, options_); !valid)
    {
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, 0, true));
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(valid.error())));
    }
    if (auto const newline = stdout_buffer_.find('\n'); newline != std::string::npos)
    {
      auto record = stdout_buffer_.substr(0, newline);
      stdout_buffer_.erase(0, newline + 1);
      if (!record.empty() && record.back() == '\r')
        record.pop_back();
      Dout(dc::plugin, "operation=record state=read bytes=" << record.size());
      return record;
    }
    if (Clock::now() >= deadline)
    {
      auto error = protocol_error(std::string(timeout_message), manifest_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      add_bounded_stream_metadata(error, stdout_buffer_, stderr_tail_, stderr_truncated_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::DeadlineExpired, std::move(error)));
    }

    if (auto drained = drain_stderr(); !drained)
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(drained.error())));
    if (auto drained = drain_stdout(true); !drained)
    {
      auto const reason =
          output_limit_error(drained.error()) ? ava::process::TerminationReasonV1::OutputLimit : ava::process::TerminationReasonV1::ProtocolFailure;
      return std::unexpected(fail_process(reason, std::move(drained.error())));
    }
    if (stdout_buffer_.find('\n') != std::string::npos)
      continue;

    if (!standard_output_.valid())
    {
      auto const status_deadline = std::min(deadline, saturating_add(Clock::now(), kNaturalStatusObservation));
      auto observed = operation_scope_.supervisor().wait(process_handle_, status_deadline);
      if (observed)
        settlement_ = *observed;
      auto error = protocol_error(stdout_buffer_.empty() ? std::string(closed_message) : "plugin protocol record ended without newline", manifest_);
      if (settlement_)
        error.with_context("status", exit_detail(*settlement_));
      add_bounded_stream_metadata(error, stdout_buffer_, stderr_tail_, stderr_truncated_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }

    std::vector<ava::process::PipeWatchV1> watches;
    watches.reserve(2);
    auto stdout_watch = standard_output_.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
    if (!stdout_watch)
    {
      auto error = process_io_error("failed to watch plugin stdout", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    watches.push_back(std::move(*stdout_watch));
    if (standard_error_.valid())
    {
      auto stderr_watch = standard_error_.watch(ava::process::PipeInterestV1::Readable, kStderrWatch);
      if (!stderr_watch)
      {
        auto error = process_io_error("failed to watch plugin stderr", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      watches.push_back(std::move(*stderr_watch));
    }

    auto const activity_deadline = std::min(deadline, saturating_add(Clock::now(), kCancellationObservationInterval));
    auto activity = operation_scope_.supervisor().wait_for_activity(process_handle_, watches, activity_deadline);
    if (!activity)
    {
      auto error = process_io_error("failed while observing plugin process streams", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
  }
}

ava::core::VoidResult PluginProcess::wait_for_writable(Clock::time_point deadline, std::chrono::milliseconds timeout, std::string_view timeout_message,
                                                       CancelCallback cancel_requested)
{
  while (true)
  {
    if (auto interrupted = interruption_reason(deadline, cancel_requested))
    {
      if (*interrupted == ava::process::TerminationReasonV1::Canceled)
        return std::unexpected(fail_process(*interrupted, canceled_error("plugin request canceled", manifest_)));
      auto error = protocol_error(std::string(timeout_message), manifest_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      return std::unexpected(fail_process(*interrupted, std::move(error)));
    }
    if (!standard_input_.valid())
    {
      auto error = protocol_error("plugin request pipe closed", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }

    std::vector<ava::process::PipeWatchV1> watches;
    watches.reserve(3);
    auto input_watch = standard_input_.watch(ava::process::PipeInterestV1::Writable, kInputWatch);
    if (!input_watch)
    {
      auto error = process_io_error("failed to watch plugin stdin", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    watches.push_back(std::move(*input_watch));
    if (standard_output_.valid())
    {
      auto stdout_watch = standard_output_.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
      if (!stdout_watch)
      {
        auto error = process_io_error("failed to watch plugin stdout", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      watches.push_back(std::move(*stdout_watch));
    }
    if (standard_error_.valid())
    {
      auto stderr_watch = standard_error_.watch(ava::process::PipeInterestV1::Readable, kStderrWatch);
      if (!stderr_watch)
      {
        auto error = process_io_error("failed to watch plugin stderr", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      watches.push_back(std::move(*stderr_watch));
    }

    auto activity = operation_scope_.supervisor().wait_for_activity(process_handle_, watches,
                                                                    std::min(deadline, saturating_add(Clock::now(), kCancellationObservationInterval)));
    if (!activity)
    {
      auto error = process_io_error("failed while observing the plugin request stream", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    if (auto drained = drain_stderr(); !drained)
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(drained.error())));
    if (auto drained = drain_stdout(true); !drained)
    {
      auto const reason =
          output_limit_error(drained.error()) ? ava::process::TerminationReasonV1::OutputLimit : ava::process::TerminationReasonV1::ProtocolFailure;
      return std::unexpected(fail_process(reason, std::move(drained.error())));
    }
    auto const input_ready = std::ranges::find_if(activity->ready, [](ava::process::PipeReadyV1 const& ready) { return ready.token == kInputWatch; });
    if (input_ready != activity->ready.end() && input_ready->writable)
      return {};
    if (input_ready != activity->ready.end() && (input_ready->peer_closed || input_ready->error))
    {
      auto error = protocol_error("plugin request pipe closed", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    if (activity->process_finished)
    {
      if (auto observed = observe_settlement())
        settlement_ = *observed;
      auto error = protocol_error("plugin process closed before accepting its request", manifest_);
      if (settlement_)
        error.with_context("status", exit_detail(*settlement_));
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
  }
}

ava::core::VoidResult PluginProcess::drain_stdout(bool enforce_record_limit)
{
  if (!standard_output_.valid())
    return {};
  std::array<char, 4096> buffer{};
  for (int reads = 0; reads < kMaxDrainReadsPerObservation; ++reads)
  {
    auto read = standard_output_.read(writable_bytes(buffer));
    if (!read)
      return std::unexpected(process_io_error("failed to read plugin stdout", manifest_));
    if (read->state == ava::process::PipeIoStateV1::WouldBlock)
      return {};
    if (read->state == ava::process::PipeIoStateV1::EndOfStream)
    {
      standard_output_.close();
      return {};
    }
    if (read->state != ava::process::PipeIoStateV1::Progress || read->bytes == 0)
      return std::unexpected(process_io_error("plugin stdout returned an invalid stream state", manifest_));

    Dout(dc::plugin, "operation=stdout state=read bytes=" << read->bytes);
    if (enforce_record_limit)
    {
      auto const byte_limit = queued_stdout_byte_limit(options_.max_record_bytes);
      if (stdout_buffer_.size() > byte_limit || read->bytes > byte_limit - std::min(stdout_buffer_.size(), byte_limit))
      {
        auto const queued_records = static_cast<std::size_t>(std::count(stdout_buffer_.begin(), stdout_buffer_.end(), '\n')) +
                                    static_cast<std::size_t>(std::count(buffer.begin(), buffer.begin() + read->bytes, '\n'));
        auto const attempted_bytes = read->bytes > std::numeric_limits<std::size_t>::max() - stdout_buffer_.size() ? std::numeric_limits<std::size_t>::max()
                                                                                                                   : stdout_buffer_.size() + read->bytes;
        static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, true));
        return std::unexpected(stdout_limit_error("plugin protocol queued bytes exceed cap", manifest_, options_, attempted_bytes, queued_records));
      }

      try
      {
        stdout_buffer_.append(buffer.data(), read->bytes);
      }
      catch (...)
      {
        static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, true));
        return std::unexpected(process_io_error("failed to retain bounded plugin stdout", manifest_));
      }
      auto valid = validate_stdout_queue(stdout_buffer_, manifest_, options_);
      if (auto accounted = operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, !valid);
          !accounted)
      {
        return std::unexpected(process_io_error("failed to account plugin stdout", manifest_));
      }
      if (!valid)
        return std::unexpected(std::move(valid.error()));
      continue;
    }

    bool const truncated =
        read->bytes > options_.max_record_bytes || stdout_buffer_.size() > options_.max_record_bytes - std::min(options_.max_record_bytes, read->bytes);
    try
    {
      append_stdout_tail(std::string_view(buffer.data(), read->bytes));
    }
    catch (...)
    {
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, true));
      return std::unexpected(process_io_error("failed to retain bounded plugin stdout", manifest_));
    }
    if (auto accounted = operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, truncated);
        !accounted)
    {
      return std::unexpected(process_io_error("failed to account plugin stdout", manifest_));
    }
  }
  return {};
}

ava::core::VoidResult PluginProcess::drain_stderr()
{
  if (!standard_error_.valid())
    return {};
  std::array<char, 4096> buffer{};
  for (int reads = 0; reads < kMaxDrainReadsPerObservation; ++reads)
  {
    auto read = standard_error_.read(writable_bytes(buffer));
    if (!read)
      return std::unexpected(process_io_error("failed to read plugin stderr", manifest_));
    if (read->state == ava::process::PipeIoStateV1::WouldBlock)
      return {};
    if (read->state == ava::process::PipeIoStateV1::EndOfStream)
    {
      standard_error_.close();
      return {};
    }
    if (read->state != ava::process::PipeIoStateV1::Progress || read->bytes == 0)
      return std::unexpected(process_io_error("plugin stderr returned an invalid stream state", manifest_));

    auto const retained_before = stderr_tail_.size();
    bool const truncated = stderr_truncated_ || retained_before > options_.max_stderr_bytes - std::min(options_.max_stderr_bytes, read->bytes);
    Dout(dc::plugin, "operation=stderr state=read bytes=" << read->bytes << " retained_bytes=" << retained_before);
    try
    {
      append_stderr(std::string_view(buffer.data(), read->bytes));
    }
    catch (...)
    {
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardError, read->bytes, true));
      return std::unexpected(process_io_error("failed to retain bounded plugin stderr", manifest_));
    }
    if (auto accounted = operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardError, read->bytes,
                                                                      truncated || stderr_truncated_);
        !accounted)
    {
      return std::unexpected(process_io_error("failed to account plugin stderr", manifest_));
    }
  }
  return {};
}

void PluginProcess::append_stdout_tail(std::string_view chunk)
{
  if (chunk.empty())
    return;
  auto const max_bytes = options_.max_record_bytes;
  if (chunk.size() >= max_bytes)
  {
    stdout_buffer_.assign(chunk.substr(chunk.size() - max_bytes));
    return;
  }
  if (stdout_buffer_.size() > max_bytes - chunk.size())
    stdout_buffer_.erase(0, stdout_buffer_.size() - (max_bytes - chunk.size()));
  stdout_buffer_.append(chunk);
}

void PluginProcess::append_stderr(std::string_view chunk)
{
  if (chunk.empty())
    return;
  auto const max_bytes = options_.max_stderr_bytes;
  if (chunk.size() >= max_bytes)
  {
    stderr_truncated_ = stderr_truncated_ || !stderr_tail_.empty() || chunk.size() > max_bytes;
    stderr_tail_.assign(chunk.substr(chunk.size() - max_bytes));
    return;
  }
  auto const next_size = stderr_tail_.size() + chunk.size();
  if (next_size > max_bytes)
  {
    stderr_tail_.erase(0, next_size - max_bytes);
    stderr_truncated_ = true;
  }
  stderr_tail_.append(chunk);
}

ava::core::VoidResult PluginProcess::drain_for_cleanup(Clock::time_point deadline) noexcept
{
  try
  {
    if (stdout_buffer_.size() > options_.max_record_bytes)
    {
      stdout_buffer_.erase(0, stdout_buffer_.size() - options_.max_record_bytes);
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, 0, true));
    }
    static_cast<void>(drain_stdout(false));
    static_cast<void>(drain_stderr());
    if (Clock::now() >= deadline || !process_handle_.valid())
      return {};

    std::vector<ava::process::PipeWatchV1> watches;
    watches.reserve(2);
    if (standard_output_.valid())
    {
      auto watch = standard_output_.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
      if (watch)
        watches.push_back(std::move(*watch));
    }
    if (standard_error_.valid())
    {
      auto watch = standard_error_.watch(ava::process::PipeInterestV1::Readable, kStderrWatch);
      if (watch)
        watches.push_back(std::move(*watch));
    }
    static_cast<void>(operation_scope_.supervisor().wait_for_activity(process_handle_, watches, deadline));
    static_cast<void>(drain_stdout(false));
    static_cast<void>(drain_stderr());
  }
  catch (...)
  {
  }
  return {};
}

}  // namespace ava::plugin
