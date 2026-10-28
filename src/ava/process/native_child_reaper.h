#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/Signals.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace ava::process::detail {

// Allocate before creating a child. Handoff itself cannot allocate, so an
// exhausted public cleanup deadline never discards native-child ownership.
struct NativeChildCleanup
{
  std::array<pid_t, 2> children{-1, -1};
  std::unique_ptr<NativeChildCleanup> next;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Application/Supervisor-owned, joinable reaper, independent of caller-visible
// settlement and the bounded monitor shutdown. It never signals and never uses
// waitpid(-1): an exact wait returning ECHILD retires an identity permanently.
// Destruction joins after the last handed-off child is reaped; shutdown(deadline)
// itself does not join this worker or extend its deadline for a delayed SIGKILL.
class NativeChildReaper
{
 public:
  NativeChildReaper() = default;
  NativeChildReaper(NativeChildReaper const&) = delete;
  NativeChildReaper& operator=(NativeChildReaper const&) = delete;
  NativeChildReaper(NativeChildReaper&&) = delete;
  NativeChildReaper& operator=(NativeChildReaper&&) = delete;
  ~NativeChildReaper()
  {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_one();
    if (worker_.joinable())
      worker_.join();
  }

  // Prepare the worker before fork so failure cannot strand an existing child.
  // An unused Supervisor creates no native thread.
  [[nodiscard]] bool start() noexcept
  {
    std::lock_guard lock(mutex_);
    if (worker_.joinable())
      return true;
    try
    {
      utils::Signal::BlockGuard scoped_signal_blocks{SIGINT, SIGTERM, SIGHUP};
      worker_ = std::thread([this] { run(); });
      return true;
    }
    catch (std::exception const&)
    {
      return false;
    }
  }

  void retain(std::unique_ptr<NativeChildCleanup> cleanup) noexcept
  {
    if (!cleanup || (cleanup->children[0] <= 1 && cleanup->children[1] <= 1))
      return;
    {
      std::lock_guard lock(mutex_);
      for (pid_t child : cleanup->children)
        pending_ += child > 1 ? 1U : 0U;
      cleanup->next = std::move(head_);
      head_ = std::move(cleanup);
    }
    changed_.notify_one();
  }

  [[nodiscard]] std::size_t pending() const noexcept
  {
    std::lock_guard lock(mutex_);
    return pending_;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void run() noexcept
  {
    std::unique_lock lock(mutex_);
    while (true)
    {
      changed_.wait(lock, [&] { return stopping_ || head_ != nullptr; });
      auto* link = &head_;
      while (*link)
      {
        auto& cleanup = **link;
        for (auto& child : cleanup.children)
        {
          if (child <= 1)
            continue;
          int status = 0;
          pid_t waited;
          do
            waited = ::waitpid(child, &status, WNOHANG);
          while (waited < 0 && errno == EINTR);
          if (waited == child || (waited < 0 && errno == ECHILD))
          {
            child = -1;
            --pending_;
          }
        }
        if (cleanup.children[0] <= 1 && cleanup.children[1] <= 1)
        {
          auto removed = std::move(*link);
          *link = std::move(removed->next);
        }
        else
          link = &cleanup.next;
      }
      if (stopping_ && !head_)
        return;
      if (head_)
      {
        // A finite cadence services all exact identities without one blocked
        // child preventing reaping of another. New handoffs wake this wait.
        auto const pending = pending_;
        static_cast<void>(changed_.wait_for(lock, std::chrono::milliseconds(10), [&] { return pending_ != pending; }));
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<NativeChildCleanup> head_;
  std::size_t pending_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace ava::process::detail
