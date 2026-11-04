// Owns a real event loop and pipe for TUI tests whose ncurses input is a regular file.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <cerrno>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace tui_test_support {

// Test-thread owner of the readiness descriptors and scheduler. Construct it before
// every feature borrowing loop(); destroy those features first so their timers retire
// before the loop and pipe. It does not service callbacks or create a worker.
class TuiEventLoopFixture final
{
 public:
  // Open both pipe ends before the event loop borrows the read descriptor.
  TuiEventLoopFixture() : descriptors_(open_pipe()), loop_(descriptors_.first.get()) { }
  // Test-thread borrow valid until fixture destruction; feature timers may use it.
  [[nodiscard]] ava::tui::TuiEventLoop& loop() { return loop_; }
  // Test-support headers are outside generated printer source discovery.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Fail construction before exposing a loop with an invalid readiness descriptor.
  static std::pair<ava::core::UniqueFd, ava::core::UniqueFd> open_pipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      throw std::system_error(errno, std::generic_category(), "create TUI test event loop pipe");
    return {ava::core::UniqueFd(descriptors[0]), ava::core::UniqueFd(descriptors[1])};
  }
  // Owned test-thread descriptors; keeping the write end open prevents a false hangup.
  std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors_;
  // Destroyed before descriptors; all borrowed feature registrations must already be gone.
  ava::tui::TuiEventLoop loop_;
};

} // namespace tui_test_support
