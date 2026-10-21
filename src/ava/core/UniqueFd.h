#pragma once

#include "ava/debug/print_members_on.h"

#include <cerrno>
#include <utility>
#include <unistd.h>

namespace ava::core {

// Owns one Linux descriptor. release() transfers ownership; get() only borrows it.
class UniqueFd
{
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) { }
  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) { }
  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~UniqueFd() { static_cast<void>(close_checked()); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept
  {
    if (fd_ == fd)
      return;
    static_cast<void>(close_checked());
    fd_ = fd;
  }

  // Linux releases the descriptor even when close fails: never retry close on EINTR.
  // The owner is empty on every return; EINTR is treated as a completed close.
  [[nodiscard]] int close_checked() noexcept
  {
    int const fd = release();
    if (fd < 0 || ::close(fd) == 0)
      return 0;
    return errno == EINTR ? 0 : errno;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  int fd_ = -1;
};

}  // namespace ava::core
