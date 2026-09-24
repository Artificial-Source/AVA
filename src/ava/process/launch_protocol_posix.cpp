#include "sys.h"
#include "ava/process/launch_protocol_posix.h"
#include "ava/process/supervisor_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

namespace ava::process::detail {
namespace {

LaunchProtocolOutcomeV1 protocol_outcome(LaunchProtocolDispositionV1 disposition, LaunchProtocolProblemV1 problem = LaunchProtocolProblemV1::None,
                                         LaunchFailureStageV1 stage = LaunchFailureStageV1::None, int error_number = 0) noexcept
{
  return {.disposition = disposition, .problem = problem, .stage = stage, .error_number = error_number};
}

#if !defined(_WIN32)

enum class FrameReadKind
{
  Frame,
  End,
  Malformed,
  Truncated,
  TimedOut,
  Canceled,
  Failed,
};

struct FrameReadResult
{
  FrameReadKind kind = FrameReadKind::Failed;
  LaunchFrameV1 frame;
  int error_number = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

int bounded_poll_timeout(ProcessDeadline deadline) noexcept
{
  auto const now = Clock::now();
  if (deadline <= now)
    return 0;
  auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining >= std::chrono::milliseconds(INT_MAX))
    return INT_MAX;
  return std::max(1, static_cast<int>(remaining.count()));
}

bool parent_canceled(std::function<bool()> const& cancel_requested) noexcept
{
  if (!cancel_requested)
    return false;
  try
  {
    return cancel_requested();
  }
  catch (...)
  {
    return true;
  }
}

std::optional<FrameReadResult> observe_wait_stop(ProcessDeadline deadline, std::function<bool()> const& cancel_requested) noexcept
{
  if (Clock::now() >= deadline)
    return FrameReadResult{.kind = FrameReadKind::TimedOut, .frame = {}, .error_number = 0};
  bool const canceled = parent_canceled(cancel_requested);
  if (Clock::now() >= deadline)
    return FrameReadResult{.kind = FrameReadKind::TimedOut, .frame = {}, .error_number = 0};
  if (canceled)
    return FrameReadResult{.kind = FrameReadKind::Canceled, .frame = {}, .error_number = 0};
  return std::nullopt;
}

bool valid_failure_stage(std::uint8_t raw) noexcept
{
  return raw >= static_cast<std::uint8_t>(LaunchFailureStageV1::SignalReset) && raw <= static_cast<std::uint8_t>(LaunchFailureStageV1::DescriptorValidation);
}

bool valid_frame(LaunchFrameV1 const& frame) noexcept
{
  if (frame.magic != kLaunchFrameMagicV1 || frame.version != kLaunchFrameVersionV1 || frame.reserved != 0)
    return false;
  auto const kind = static_cast<LaunchFrameKindV1>(frame.kind);
  switch (kind)
  {
    case LaunchFrameKindV1::LeaderReady:
    case LaunchFrameKindV1::BashContainmentApplied:
    case LaunchFrameKindV1::ExecAttempt:
      return frame.stage == static_cast<std::uint8_t>(LaunchFailureStageV1::None) && frame.error_number == 0;
    case LaunchFrameKindV1::LaunchFailed:
      return valid_failure_stage(frame.stage) && frame.error_number > 0;
    case LaunchFrameKindV1::ExecFailed:
      return frame.stage == static_cast<std::uint8_t>(LaunchFailureStageV1::None) && frame.error_number > 0;
  }
  return false;
}

FrameReadResult read_frame(int descriptor, ProcessDeadline deadline, std::function<bool()> const& cancel_requested = {}) noexcept
{
  constexpr auto cancellation_slice = std::chrono::milliseconds(10);
  std::array<std::byte, sizeof(LaunchFrameV1)> bytes{};
  std::size_t offset = 0;
  while (true)
  {
    if (auto stopped = observe_wait_stop(deadline, cancel_requested))
      return *stopped;

    pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
    int result = -1;
    int poll_error = 0;
    auto const observation_deadline = cancel_requested ? std::min(deadline, Clock::now() + cancellation_slice) : deadline;
    while (true)
    {
      result = ::poll(&item, 1, bounded_poll_timeout(observation_deadline));
      if (result >= 0)
        break;
      poll_error = errno == 0 ? EIO : errno;
      if (poll_error != EINTR)
        break;
      if (auto stopped = observe_wait_stop(deadline, cancel_requested))
        return *stopped;
    }
    if (result == 0)
    {
      if (auto stopped = observe_wait_stop(deadline, cancel_requested))
        return *stopped;
      if (observation_deadline < deadline)
        continue;
      return FrameReadResult{.kind = FrameReadKind::TimedOut, .frame = {}, .error_number = 0};
    }
    if (result < 0)
      return FrameReadResult{.kind = FrameReadKind::Failed, .frame = {}, .error_number = poll_error};
    if ((item.revents & POLLNVAL) != 0)
      return FrameReadResult{.kind = FrameReadKind::Failed, .frame = {}, .error_number = EBADF};

    auto const count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count > 0)
    {
      offset += static_cast<std::size_t>(count);
      if (offset != bytes.size())
        continue;
      LaunchFrameV1 frame;
      std::memcpy(&frame, bytes.data(), sizeof(frame));
      return FrameReadResult{.kind = valid_frame(frame) ? FrameReadKind::Frame : FrameReadKind::Malformed, .frame = frame, .error_number = 0};
    }
    if (count == 0)
      return FrameReadResult{.kind = offset == 0 ? FrameReadKind::End : FrameReadKind::Truncated, .frame = {}, .error_number = 0};
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
    {
      if (auto stopped = observe_wait_stop(deadline, cancel_requested))
        return *stopped;
      continue;
    }
    return FrameReadResult{.kind = FrameReadKind::Failed, .frame = {}, .error_number = errno == 0 ? EIO : errno};
  }
}

LaunchProtocolOutcomeV1 read_problem(FrameReadResult const& read, bool attempted) noexcept
{
  switch (read.kind)
  {
    case FrameReadKind::End:
      return protocol_outcome(attempted ? LaunchProtocolDispositionV1::ExecConfirmed : LaunchProtocolDispositionV1::LaunchFailed,
                              attempted ? LaunchProtocolProblemV1::None : LaunchProtocolProblemV1::EndBeforeAttempt);
    case FrameReadKind::Malformed:
      return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::MalformedFrame);
    case FrameReadKind::Truncated:
      return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::TruncatedFrame);
    case FrameReadKind::TimedOut:
      return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::TimedOut);
    case FrameReadKind::Canceled:
      return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::Canceled);
    case FrameReadKind::Failed:
      return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::ReadFailed, LaunchFailureStageV1::None, read.error_number);
    case FrameReadKind::Frame:
      break;
  }
  return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::OutOfOrderFrame);
}

LaunchFrameV1 make_frame(LaunchFrameKindV1 kind, LaunchFailureStageV1 stage, int error_number) noexcept
{
  return {.magic = kLaunchFrameMagicV1,
          .version = kLaunchFrameVersionV1,
          .kind = static_cast<std::uint8_t>(kind),
          .stage = static_cast<std::uint8_t>(stage),
          .error_number = error_number,
          .reserved = 0};
}

bool child_write_frame(int descriptor, LaunchFrameV1 const& frame) noexcept
{
  return descriptor > STDERR_FILENO && child_write_all(descriptor, &frame, sizeof(frame));
}

#endif

std::string_view stage_name(LaunchFailureStageV1 stage) noexcept
{
  switch (stage)
  {
    case LaunchFailureStageV1::None:
      return "none";
    case LaunchFailureStageV1::SignalReset:
      return "signal_reset";
    case LaunchFailureStageV1::ProcessGroup:
      return "process_group";
    case LaunchFailureStageV1::Gate:
      return "registration_gate";
    case LaunchFailureStageV1::Streams:
      return "streams";
    case LaunchFailureStageV1::WorkingDirectory:
      return "working_directory";
    case LaunchFailureStageV1::Containment:
      return "containment";
    case LaunchFailureStageV1::DescriptorValidation:
      return "descriptor_validation";
  }
  return "invalid";
}

std::string_view problem_name(LaunchProtocolProblemV1 problem) noexcept
{
  switch (problem)
  {
    case LaunchProtocolProblemV1::None:
      return "none";
    case LaunchProtocolProblemV1::ChildReported:
      return "child_reported";
    case LaunchProtocolProblemV1::EndBeforeAttempt:
      return "eof_before_exec_attempt";
    case LaunchProtocolProblemV1::MalformedFrame:
      return "malformed_frame";
    case LaunchProtocolProblemV1::TruncatedFrame:
      return "truncated_frame";
    case LaunchProtocolProblemV1::OutOfOrderFrame:
      return "out_of_order_frame";
    case LaunchProtocolProblemV1::TimedOut:
      return "startup_timeout";
    case LaunchProtocolProblemV1::Canceled:
      return "parent_canceled";
    case LaunchProtocolProblemV1::ReadFailed:
      return "status_read";
    case LaunchProtocolProblemV1::ContinuationFailed:
      return "containment_continuation";
  }
  return "invalid";
}

}  // namespace

bool child_write_leader_ready(int descriptor) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  return false;
#else
  return child_write_frame(descriptor, make_frame(LaunchFrameKindV1::LeaderReady, LaunchFailureStageV1::None, 0));
#endif
}

bool child_write_launch_failed(int descriptor, LaunchFailureStageV1 stage, int error_number) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  static_cast<void>(stage);
  static_cast<void>(error_number);
  return false;
#else
  auto const raw_stage = static_cast<std::uint8_t>(stage);
  if (!valid_failure_stage(raw_stage))
    stage = LaunchFailureStageV1::DescriptorValidation;
  if (error_number <= 0)
    error_number = EIO;
  return child_write_frame(descriptor, make_frame(LaunchFrameKindV1::LaunchFailed, stage, error_number));
#endif
}

bool child_write_bash_containment_applied(int descriptor) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  return false;
#else
  return child_write_frame(descriptor, make_frame(LaunchFrameKindV1::BashContainmentApplied, LaunchFailureStageV1::None, 0));
#endif
}

bool child_write_exec_attempt(int descriptor) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  return false;
#else
  return child_write_frame(descriptor, make_frame(LaunchFrameKindV1::ExecAttempt, LaunchFailureStageV1::None, 0));
#endif
}

bool child_write_exec_failed(int descriptor, int error_number) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  static_cast<void>(error_number);
  return false;
#else
  if (error_number <= 0)
    error_number = EIO;
  return child_write_frame(descriptor, make_frame(LaunchFrameKindV1::ExecFailed, LaunchFailureStageV1::None, error_number));
#endif
}

LaunchProtocolOutcomeV1 await_launch_leader_ready(int descriptor, ProcessDeadline deadline) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  static_cast<void>(deadline);
  return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::ReadFailed, LaunchFailureStageV1::None, ENOTSUP);
#else
  auto const read = read_frame(descriptor, deadline);
  if (read.kind != FrameReadKind::Frame)
    return read_problem(read, false);
  auto const kind = static_cast<LaunchFrameKindV1>(read.frame.kind);
  if (kind == LaunchFrameKindV1::LeaderReady)
    return protocol_outcome(LaunchProtocolDispositionV1::LeaderReady);
  if (kind == LaunchFrameKindV1::LaunchFailed)
  {
    return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::ChildReported,
                            static_cast<LaunchFailureStageV1>(read.frame.stage), read.frame.error_number);
  }
  return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::OutOfOrderFrame);
#endif
}

LaunchProtocolOutcomeV1 await_launch_exec_confirmation(int descriptor, ProcessDeadline deadline, bool containment_required,
                                                       int containment_continuation_descriptor, std::function<bool()> const& cancel_requested) noexcept
{
#if defined(_WIN32)
  static_cast<void>(descriptor);
  static_cast<void>(deadline);
  static_cast<void>(containment_required);
  static_cast<void>(containment_continuation_descriptor);
  static_cast<void>(cancel_requested);
  return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::ReadFailed, LaunchFailureStageV1::None, ENOTSUP);
#else
  bool checkpoint = false;
  bool attempted = false;
  while (true)
  {
    auto const read = read_frame(descriptor, deadline, cancel_requested);
    if (read.kind != FrameReadKind::Frame)
      return read_problem(read, attempted);

    auto const kind = static_cast<LaunchFrameKindV1>(read.frame.kind);
    if (!attempted && kind == LaunchFrameKindV1::LaunchFailed)
    {
      return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::ChildReported,
                              static_cast<LaunchFailureStageV1>(read.frame.stage), read.frame.error_number);
    }
    if (!attempted && kind == LaunchFrameKindV1::BashContainmentApplied)
    {
      if (!containment_required || checkpoint || containment_continuation_descriptor <= STDERR_FILENO)
        return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::OutOfOrderFrame);
      if (Clock::now() >= deadline)
        return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::TimedOut);
      char const continuation = 'C';
      if (!write_all_to_descriptor(containment_continuation_descriptor, &continuation, 1))
      {
        return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::ContinuationFailed, LaunchFailureStageV1::None, EIO);
      }
      checkpoint = true;
      continue;
    }
    if (!attempted && kind == LaunchFrameKindV1::ExecAttempt)
    {
      if (containment_required && !checkpoint)
        return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::OutOfOrderFrame);
      attempted = true;
      continue;
    }
    if (attempted && kind == LaunchFrameKindV1::ExecFailed)
    {
      return protocol_outcome(LaunchProtocolDispositionV1::ExecFailed, LaunchProtocolProblemV1::ChildReported, LaunchFailureStageV1::None,
                              read.frame.error_number);
    }
    return protocol_outcome(LaunchProtocolDispositionV1::LaunchFailed, LaunchProtocolProblemV1::OutOfOrderFrame);
  }
#endif
}

ava::core::Error launch_protocol_error(LaunchProtocolOutcomeV1 const& outcome, std::string_view operation)
{
  std::string message(operation);
  message += outcome.disposition == LaunchProtocolDispositionV1::ExecFailed ? " exec syscall returned" : " launch confirmation failed";
  auto error = outcome.error_number > 0 ? io_error(std::move(message), outcome.error_number) : process_error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("protocol", std::string(problem_name(outcome.problem)));
  if (outcome.stage != LaunchFailureStageV1::None)
    error.with_context("stage", std::string(stage_name(outcome.stage)));
  return error;
}

}  // namespace ava::process::detail
