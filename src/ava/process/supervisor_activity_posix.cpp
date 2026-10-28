#include "sys.h"
#include "ava/process/supervisor_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#if !defined(_WIN32)
#include <poll.h>
#endif

namespace ava::process {
namespace {

using detail::Clock;

#if !defined(_WIN32)

struct ResolvedWatch
{
  std::shared_ptr<detail::PipeEndpointState> endpoint;
  PipeInterestV1 interest = PipeInterestV1::Readable;
  std::uint32_t token = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

int poll_timeout(ProcessDeadline deadline) noexcept
{
  auto const now = Clock::now();
  if (deadline <= now)
    return 0;
  auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining >= std::chrono::milliseconds(INT_MAX))
    return INT_MAX;
  return std::max(1, static_cast<int>(remaining.count()));
}

ava::core::Result<int> poll_activity(std::span<pollfd> descriptors, ProcessDeadline deadline, bool blocking)
{
  while (true)
  {
    int const result = ::poll(descriptors.data(), descriptors.size(), blocking ? poll_timeout(deadline) : 0);
    if (result > 0)
      return result;
    if (result < 0 && errno != EINTR)
      return std::unexpected(detail::io_error("failed while waiting for process activity", errno));
    if (!blocking || Clock::now() >= deadline)
      return 0;
  }
}

ava::core::Result<int> ensure_completion_channel_locked(detail::HandleState& handle, bool& created)
{
  created = false;
  if (!handle.completion_channel)
  {
    auto channel = detail::make_cloexec_pipe();
    if (!channel)
      return std::unexpected(std::move(channel.error()));
    if (auto nonblocking = detail::set_nonblocking(channel->read_end.get()); !nonblocking)
      return std::unexpected(std::move(nonblocking.error()));
    if (auto nonblocking = detail::set_nonblocking(channel->write_end.get()); !nonblocking)
      return std::unexpected(std::move(nonblocking.error()));
    handle.completion_channel.emplace(std::move(*channel));
    created = true;
    if (handle.final_status)
      detail::signal_completion_locked(handle);
  }
  if (handle.completion_channel->read_end.get() < 0)
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process completion channel is unavailable"));
  return handle.completion_channel->read_end.get();
}

ava::core::VoidResult invoke_completion_channel_hook(std::shared_ptr<detail::AfterForkBeforeReleaseHook> const& hook)
{
  if (!hook || !*hook)
    return {};
  try
  {
    (*hook)();
    return {};
  }
  catch (...)
  {
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process activity test seam failed"));
  }
}

#endif

}  // namespace

ava::core::Result<ProcessActivityV1> Supervisor::wait_for_activity(ProcessHandle const& handle, std::span<PipeWatchV1 const> watches,
                                                                   ProcessDeadline deadline) const
{
#if defined(_WIN32)
  static_cast<void>(handle);
  static_cast<void>(watches);
  static_cast<void>(deadline);
  return std::unexpected(detail::unsupported_error());
#else
  auto state = implementation_->state;
  if (!handle.valid() || handle.state_->supervisor.lock().get() != state.get())
    return std::unexpected(detail::invalid_error("process handle does not belong to this supervisor"));
  if (watches.size() > kMaxPipeWatchesV1)
    return std::unexpected(detail::invalid_error("process activity watch count exceeds the supported bound"));

  try
  {
    auto handle_state = handle.state_;
    std::array<ResolvedWatch, kMaxPipeWatchesV1> resolved{};
    for (std::size_t index = 0; index < watches.size(); ++index)
    {
      auto const& watch = watches[index];
      if (!is_valid(watch.interest_))
        return std::unexpected(detail::invalid_error("process pipe watch has an unknown interest"));
      auto endpoint = watch.endpoint_.lock();
      if (!endpoint)
        return std::unexpected(detail::invalid_error("process pipe watch is stale"));
      for (std::size_t previous = 0; previous < index; ++previous)
      {
        if (resolved[previous].token == watch.token_)
          return std::unexpected(detail::invalid_error("process activity watch tokens must be unique"));
        if (resolved[previous].endpoint.get() == endpoint.get() && resolved[previous].interest == watch.interest_)
          return std::unexpected(detail::invalid_error("process activity contains a duplicate endpoint interest"));
      }
      resolved[index] = ResolvedWatch{.endpoint = std::move(endpoint), .interest = watch.interest_, .token = watch.token_};
    }

    std::array<std::shared_ptr<detail::PipeEndpointState>, kMaxPipeWatchesV1> unique_endpoints{};
    std::size_t unique_count = 0;
    for (std::size_t index = 0; index < watches.size(); ++index)
    {
      bool duplicate = false;
      for (std::size_t previous = 0; previous < unique_count; ++previous)
        duplicate = duplicate || unique_endpoints[previous].get() == resolved[index].endpoint.get();
      if (!duplicate)
        unique_endpoints[unique_count++] = resolved[index].endpoint;
    }
    for (std::size_t index = 0; index < unique_count; ++index)
    {
      for (std::size_t next = index + 1; next < unique_count; ++next)
      {
        if (std::less<detail::PipeEndpointState*>{}(unique_endpoints[next].get(), unique_endpoints[index].get()))
          std::swap(unique_endpoints[index], unique_endpoints[next]);
      }
    }
    std::array<int, kMaxPipeWatchesV1> endpoint_descriptors{};
    {
      std::array<std::unique_lock<std::mutex>, kMaxPipeWatchesV1> endpoint_locks{};
      for (std::size_t index = 0; index < unique_count; ++index)
        endpoint_locks[index] = std::unique_lock(unique_endpoints[index]->mutex);

      for (std::size_t index = 0; index < watches.size(); ++index)
      {
        auto const& item = resolved[index];
        if (item.endpoint->descriptor.get() < 0)
          return std::unexpected(detail::invalid_error("process pipe watch endpoint is closed"));
        if (item.interest == PipeInterestV1::Readable && !item.endpoint->readable)
          return std::unexpected(detail::invalid_error("process pipe watch does not match a readable endpoint"));
        if (item.interest == PipeInterestV1::Writable && !item.endpoint->writable)
          return std::unexpected(detail::invalid_error("process pipe watch does not match a writable endpoint"));
        endpoint_descriptors[index] = item.endpoint->descriptor.get();
      }
    }

    auto waiter = detail::register_active_waiter(state, handle_state->record);
    if (!waiter)
      return std::unexpected(std::move(waiter.error()));

    std::shared_ptr<detail::AfterForkBeforeReleaseHook> channel_hook;
    {
      std::lock_guard state_lock(state->mutex);
      channel_hook = state->after_completion_channel_create_for_test;
    }

    int completion_descriptor = -1;
    bool channel_created = false;
    bool process_finished_before_poll = false;
    {
      std::lock_guard handle_lock(handle_state->mutex);
      auto completion = ensure_completion_channel_locked(*handle_state, channel_created);
      if (!completion)
        return std::unexpected(std::move(completion.error()));
      completion_descriptor = *completion;
      process_finished_before_poll = handle_state->final_status.has_value();
    }
    if (channel_created)
    {
      auto hook_result = invoke_completion_channel_hook(channel_hook);
      if (!hook_result)
        return std::unexpected(std::move(hook_result.error()));
    }

    std::array<pollfd, kMaxPipeWatchesV1 + 1> descriptors{};
    for (std::size_t index = 0; index < watches.size(); ++index)
    {
      descriptors[index].fd = endpoint_descriptors[index];
      descriptors[index].events = resolved[index].interest == PipeInterestV1::Readable ? POLLIN : POLLOUT;
    }
    descriptors[watches.size()].fd = completion_descriptor;
    descriptors[watches.size()].events = POLLIN;
    auto descriptor_span = std::span(descriptors.data(), watches.size() + 1);

    auto observed = poll_activity(descriptor_span, deadline, !process_finished_before_poll);
    if (!observed)
      return std::unexpected(std::move(observed.error()));
    bool const initial_deadline_expired = *observed == 0;

    std::array<short, kMaxPipeWatchesV1> endpoint_events{};
    for (std::size_t index = 0; index < watches.size(); ++index)
      endpoint_events[index] = descriptors[index].revents;
    short completion_events = descriptors[watches.size()].revents;

    for (auto& descriptor : descriptor_span)
      descriptor.revents = 0;
    auto final_observation = poll_activity(descriptor_span, deadline, false);
    if (!final_observation)
      return std::unexpected(std::move(final_observation.error()));
    for (std::size_t index = 0; index < watches.size(); ++index)
      endpoint_events[index] = static_cast<short>(endpoint_events[index] | descriptors[index].revents);
    completion_events = static_cast<short>(completion_events | descriptors[watches.size()].revents);

    ProcessActivityV1 result;
    result.ready.reserve(watches.size());
    for (std::size_t index = 0; index < watches.size(); ++index)
    {
      short const events = endpoint_events[index];
      if ((events & POLLNVAL) != 0)
        return std::unexpected(detail::invalid_error("process pipe watch endpoint became invalid"));
      bool const readable = (events & POLLIN) != 0;
      bool const writable = (events & POLLOUT) != 0;
      bool const peer_closed = (events & POLLHUP) != 0;
      bool const poll_error = (events & POLLERR) != 0;
      if (readable || writable || peer_closed || poll_error)
        result.ready.push_back(
            PipeReadyV1{.token = resolved[index].token, .readable = readable, .writable = writable, .peer_closed = peer_closed, .error = poll_error});
    }

    if ((completion_events & POLLNVAL) != 0)
      return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process completion channel became invalid"));
    {
      std::lock_guard handle_lock(handle_state->mutex);
      result.process_finished = handle_state->final_status.has_value();
    }
    if ((completion_events & (POLLIN | POLLHUP | POLLERR)) != 0 && !result.process_finished)
      return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process completion channel changed without a final value"));
    result.deadline_expired = initial_deadline_expired && !result.process_finished && result.ready.empty();
    return result;
  }
  catch (std::exception const&)
  {
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "failed to prepare a bounded process activity wait"));
  }
  catch (...)
  {
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "failed to prepare a bounded process activity wait"));
  }
#endif
}

}  // namespace ava::process
