#include "sys.h"
#include "ava/process/supervisor_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/syscall.h>
#endif
#endif

namespace ava::process::detail {

#if !defined(_WIN32)
namespace {

void update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value) noexcept
{
  auto previous = peak.load(std::memory_order_relaxed);
  while (previous < value && !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed))
  {
  }
}

int fcntl_retry(int descriptor, int operation, int value = 0) noexcept
{
  while (true)
  {
    int const result = ::fcntl(descriptor, operation, value);
    if (result < 0 && errno == EINTR)
      continue;
    return result;
  }
}

struct PreparedDescriptor
{
  ava::core::UniqueFd descriptor;
  int error_number = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

PreparedDescriptor prepare_descriptor(int raw_descriptor, std::shared_ptr<MonitorTelemetry> const& telemetry) noexcept
{
  int descriptor_value = raw_descriptor;
  if (descriptor_value <= STDERR_FILENO)
  {
    int const moved = fcntl_retry(descriptor_value, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    int const move_error = errno;
    static_cast<void>(::close(descriptor_value));
    if (moved < 0)
      return {.descriptor = ava::core::UniqueFd{}, .error_number = move_error == 0 ? EIO : move_error};
    descriptor_value = moved;
  }
  ava::core::UniqueFd descriptor(descriptor_value);

  telemetry->cloexec_checks.fetch_add(1, std::memory_order_relaxed);
  int descriptor_flags = fcntl_retry(descriptor.get(), F_GETFD);
  if (descriptor_flags < 0 || ((descriptor_flags & FD_CLOEXEC) == 0 && fcntl_retry(descriptor.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0))
  {
    telemetry->cloexec_failures.fetch_add(1, std::memory_order_relaxed);
    return {.descriptor = ava::core::UniqueFd{}, .error_number = errno == 0 ? EIO : errno};
  }

  telemetry->nonblocking_checks.fetch_add(1, std::memory_order_relaxed);
  int const status_flags = fcntl_retry(descriptor.get(), F_GETFL);
  if (status_flags < 0 || ((status_flags & O_NONBLOCK) == 0 && fcntl_retry(descriptor.get(), F_SETFL, status_flags | O_NONBLOCK) != 0))
  {
    telemetry->nonblocking_failures.fetch_add(1, std::memory_order_relaxed);
    return {.descriptor = ava::core::UniqueFd{}, .error_number = errno == 0 ? EIO : errno};
  }
  return {.descriptor = std::move(descriptor)};
}

void count_pidfd_failure(std::shared_ptr<MonitorTelemetry> const& telemetry, PidfdFailureClass failure) noexcept
{
  switch (failure)
  {
    case PidfdFailureClass::Unavailable:
      telemetry->pidfd_unavailable_failures.fetch_add(1, std::memory_order_relaxed);
      break;
    case PidfdFailureClass::Denied:
      telemetry->pidfd_denied_failures.fetch_add(1, std::memory_order_relaxed);
      break;
    case PidfdFailureClass::Resource:
      telemetry->pidfd_resource_failures.fetch_add(1, std::memory_order_relaxed);
      break;
    case PidfdFailureClass::Other:
      telemetry->pidfd_other_failures.fetch_add(1, std::memory_order_relaxed);
      break;
    case PidfdFailureClass::None:
      break;
  }
}

PidfdFailureClass classify_pidfd_error(int error_number) noexcept
{
  if (error_number == ENOSYS || error_number == EINVAL || error_number == ENODEV)
    return PidfdFailureClass::Unavailable;
  if (error_number == EPERM || error_number == EACCES)
    return PidfdFailureClass::Denied;
  if (error_number == EMFILE || error_number == ENFILE || error_number == ENOMEM)
    return PidfdFailureClass::Resource;
  return PidfdFailureClass::Other;
}

}  // namespace

PidfdWatch::PidfdWatch(ava::core::UniqueFd descriptor_value, std::shared_ptr<MonitorTelemetry> telemetry_value) noexcept
    : descriptor(std::move(descriptor_value)), telemetry(std::move(telemetry_value))
{
  auto const current = telemetry->current_watches.fetch_add(1, std::memory_order_relaxed) + 1;
  update_peak(telemetry->peak_watches, current);
}

PidfdWatch::~PidfdWatch()
{
  descriptor.reset();
  telemetry->current_watches.fetch_sub(1, std::memory_order_relaxed);
}

MonitorWake::MonitorWake(ava::core::UniqueFd read_descriptor, ava::core::UniqueFd write_descriptor, bool uses_eventfd,
                         std::shared_ptr<MonitorTelemetry> telemetry_value) noexcept
    : read_end(std::move(read_descriptor)), write_end(std::move(write_descriptor)), eventfd(uses_eventfd), telemetry(std::move(telemetry_value))
{
  auto const current = telemetry->current_wake_descriptors.fetch_add(descriptor_count(), std::memory_order_relaxed) + descriptor_count();
  update_peak(telemetry->peak_wake_descriptors, current);
}

MonitorWake::~MonitorWake()
{
  read_end.reset();
  write_end.reset();
  telemetry->current_wake_descriptors.fetch_sub(descriptor_count(), std::memory_order_relaxed);
}

ava::core::Result<std::shared_ptr<MonitorWake>> make_monitor_wake(std::shared_ptr<MonitorTelemetry> const& telemetry)
{
  try
  {
#if defined(__linux__)
    int const event_descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_descriptor >= 0)
    {
      auto prepared = prepare_descriptor(event_descriptor, telemetry);
      if (prepared.descriptor.get() >= 0)
        return std::make_shared<MonitorWake>(std::move(prepared.descriptor), ava::core::UniqueFd{}, true, telemetry);
    }
#endif

    auto pipe = make_cloexec_pipe();
    if (!pipe)
      return std::unexpected(std::move(pipe.error()));
    auto prepared_read = prepare_descriptor(pipe->read_end.release(), telemetry);
    if (prepared_read.descriptor.get() < 0)
      return std::unexpected(io_error("failed to prepare the process monitor wake reader", prepared_read.error_number));
    auto prepared_write = prepare_descriptor(pipe->write_end.release(), telemetry);
    if (prepared_write.descriptor.get() < 0)
      return std::unexpected(io_error("failed to prepare the process monitor wake writer", prepared_write.error_number));
    return std::make_shared<MonitorWake>(std::move(prepared_read.descriptor), std::move(prepared_write.descriptor), false, telemetry);
  }
  catch (std::exception const& error)
  {
    return std::unexpected(
        process_error(ava::core::ErrorCategory::Io, "failed to allocate the process monitor wake channel").with_context("cause", error.what()));
  }
  catch (...)
  {
    return std::unexpected(process_error(ava::core::ErrorCategory::Io, "failed to allocate the process monitor wake channel"));
  }
}

void signal_monitor_wake(std::shared_ptr<MonitorWake> const& wake) noexcept
{
  if (!wake)
    return;
  wake->telemetry->wake_attempts.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t const event_value = 1;
  char const pipe_value = 'W';
  void const* data = wake->eventfd ? static_cast<void const*>(&event_value) : static_cast<void const*>(&pipe_value);
  std::size_t const size = wake->eventfd ? sizeof(event_value) : sizeof(pipe_value);
  while (true)
  {
    ssize_t const result = ::write(wake->signal_descriptor(), data, size);
    if (result == static_cast<ssize_t>(size))
      return;
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      wake->telemetry->wake_coalesces.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    wake->telemetry->wake_failures.fetch_add(1, std::memory_order_relaxed);
    return;
  }
}

void drain_monitor_wake(std::shared_ptr<MonitorWake> const& wake) noexcept
{
  if (!wake)
    return;
  wake->telemetry->wake_drains.fetch_add(1, std::memory_order_relaxed);
  std::array<char, 256> pipe_values{};
  std::uint64_t event_value = 0;
  void* data = wake->eventfd ? static_cast<void*>(&event_value) : static_cast<void*>(pipe_values.data());
  std::size_t const size = wake->eventfd ? sizeof(event_value) : pipe_values.size();
  while (true)
  {
    ssize_t const result = ::read(wake->poll_descriptor(), data, size);
    if (result > 0)
      continue;
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    if (result == 0)
      wake->telemetry->wake_failures.fetch_add(1, std::memory_order_relaxed);
    else if (result < 0)
      wake->telemetry->wake_failures.fetch_add(1, std::memory_order_relaxed);
    return;
  }
}

PidfdOpenResult open_pidfd_watch(pid_t process, MonitorBackendMode mode, bool runtime_unavailable, std::shared_ptr<MonitorTelemetry> const& telemetry) noexcept
{
  if (mode == MonitorBackendMode::ForceFallbackUnavailable || runtime_unavailable)
  {
    count_pidfd_failure(telemetry, PidfdFailureClass::Unavailable);
    return {.watch = {}, .failure = PidfdFailureClass::Unavailable, .cache_runtime_unavailable = false, .prompt_exact_probe = false};
  }
  if (mode == MonitorBackendMode::ForceFallbackDenied)
  {
    count_pidfd_failure(telemetry, PidfdFailureClass::Denied);
    return {.watch = {}, .failure = PidfdFailureClass::Denied, .cache_runtime_unavailable = false, .prompt_exact_probe = false};
  }

#if defined(__linux__) && defined(SYS_pidfd_open)
  telemetry->pidfd_attempts.fetch_add(1, std::memory_order_relaxed);
  errno = 0;
  long const raw_result = ::syscall(SYS_pidfd_open, process, 0);
  if (raw_result >= 0)
  {
    auto prepared = prepare_descriptor(static_cast<int>(raw_result), telemetry);
    if (prepared.descriptor.get() >= 0)
    {
      try
      {
        auto watch = std::make_shared<PidfdWatch>(std::move(prepared.descriptor), telemetry);
        telemetry->pidfd_successes.fetch_add(1, std::memory_order_relaxed);
        return {.watch = std::move(watch), .failure = PidfdFailureClass::None, .cache_runtime_unavailable = false, .prompt_exact_probe = false};
      }
      catch (...)
      {
        count_pidfd_failure(telemetry, PidfdFailureClass::Resource);
        return {.watch = {}, .failure = PidfdFailureClass::Resource, .cache_runtime_unavailable = false, .prompt_exact_probe = false};
      }
    }
    auto const failure = classify_pidfd_error(prepared.error_number);
    count_pidfd_failure(telemetry, failure);
    return {.watch = {},
            .failure = failure,
            .cache_runtime_unavailable = failure == PidfdFailureClass::Unavailable,
            .prompt_exact_probe = failure == PidfdFailureClass::Other};
  }

  int const error_number = errno == 0 ? EIO : errno;
  auto const failure = classify_pidfd_error(error_number);
  count_pidfd_failure(telemetry, failure);
  return {.watch = {},
          .failure = failure,
          .cache_runtime_unavailable = failure == PidfdFailureClass::Unavailable,
          .prompt_exact_probe = failure == PidfdFailureClass::Other};
#else
  static_cast<void>(process);
  count_pidfd_failure(telemetry, PidfdFailureClass::Unavailable);
  return {.watch = {}, .failure = PidfdFailureClass::Unavailable, .cache_runtime_unavailable = true, .prompt_exact_probe = false};
#endif
}

#endif

}  // namespace ava::process::detail
