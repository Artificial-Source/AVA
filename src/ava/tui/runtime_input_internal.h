#pragma once

#include "ava/tui/terminal.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include "debug.h"

namespace ava::tui::terminal {
class Context;
}

namespace ava::tui::runtime_input {

struct RuntimeInput
{
  InputEvent event;
  std::string text;
  bool bracketed_paste = false;
  bool resize = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::optional<std::string> printable_jump_target(RuntimeInput const& input);
[[nodiscard]] RuntimeInput read_curses_input();
[[nodiscard]] std::optional<RuntimeInput> poll_curses_input();
[[nodiscard]] std::optional<RuntimeInput> read_curses_input_with_timeout(std::chrono::milliseconds timeout);

// Session-scoped startup probe queue: complete RuntimeInput events observed while
// draining the OSC 11 background query are preserved here and drained before any
// later blocking or polling read. Cap is 64 events.
void clear_startup_input_queue();
[[nodiscard]] std::size_t startup_input_queue_size();
[[nodiscard]] bool enqueue_startup_input(RuntimeInput input);
// Raw terminal read path used by the startup probe. Does not drain the startup queue.
[[nodiscard]] RuntimeInput read_curses_input_from_terminal();
// Deterministic Context seam for canonical startup-buffer replay tests.
[[nodiscard]] RuntimeInput read_curses_input_from_terminal(terminal::Context& terminal_context);
// Drain ncurses input for up to `deadline`, enqueueing complete non-discard events.
// Escape assembly begun at the deadline edge may still use its ordinary 50 ms budget.
// A bracketed paste already begun may finish under its ordinary 1 s cap so the paste
// stays atomic. Stops early if the queue reaches capacity (remaining unread input is
// left for later normal reads).
void drain_startup_probe_input(std::chrono::milliseconds deadline);

}  // namespace ava::tui::runtime_input
