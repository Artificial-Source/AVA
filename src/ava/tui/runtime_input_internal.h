// Defines owned decoded input and the polling seam between terminal decoding and
// TuiSession's input budget, including replay of input captured during startup.
#pragma once

#include "InputPollResult.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace ava::tui::terminal {
class Context;
}

namespace ava::tui::runtime_input {

// TUI input stage: borrow an event and copy its first character as a jump candidate.
// Reject paste, control keys, ASCII controls and invalid UTF-8 byte shapes; this does
// not check full Unicode scalar validity. Returned bytes own their storage; no terminal read occurs.
[[nodiscard]] std::optional<std::string> printable_jump_target(RuntimeInput const& input);
// TUI thread with an initialized Application terminal: consume startup replay first,
// then decode using the current terminal timeout. Unknown conflates no input with
// discarded input here; the session uses poll_curses_input to tell them apart.
[[nodiscard]] RuntimeInput read_curses_input();
// TUI thread with an initialized Application terminal: take startup replay first,
// otherwise decode without waiting for an initial key. Restore the previous timeout
// before returning. Escape/paste assembly may still wait after the first byte; poll
// again after Discarded and apply the turn budget before deciding to block.
[[nodiscard]] InputPollResult poll_curses_input();
// TUI thread with an initialized Application terminal: take replay first or apply
// timeout to the initial read, restoring the prior setting on return. Return no value
// for both discarded and absent input; escape/paste continuation has its own timeout.
[[nodiscard]] std::optional<RuntimeInput> read_curses_input_with_timeout(std::chrono::milliseconds timeout);

// TUI thread: discard owned startup replay without changing native input, timeout
// or incomplete-paste tracking. Prompt input cleanup uses this while preserving
// the rule that delayed paste tails are discarded rather than handled as ordinary keys.
void clear_startup_input_queue();
// TUI thread at actual terminal-session entry/exit: clear both startup replay
// and incomplete-paste tracking. Never use this when opening or closing a prompt.
void reset_input_decoder();
// TUI thread: inspect preserved startup events without consuming them; capacity is
// 64 and no producer thread may read or mutate this unsynchronized queue.
[[nodiscard]] std::size_t startup_input_queue_size();
// TUI thread after decoding an actionable startup event: move it into FIFO replay
// for later normal reads. False means capacity is full and the passed event was not saved.
[[nodiscard]] bool enqueue_startup_input(RuntimeInput input);
// TUI startup probe with an initialized Application terminal: bypass startup replay
// to avoid consuming and re-enqueueing it. Unknown means absent or discarded native input.
[[nodiscard]] RuntimeInput read_curses_input_from_terminal();
// TUI thread with a live initialized Context: decode through that Context's canonical
// replay/native reader using its current timeout, bypassing this module's startup
// queue. The Context is borrowed only for the call; the returned event owns its text.
[[nodiscard]] RuntimeInput read_curses_input_from_terminal(terminal::Context& terminal_context);
// TUI startup probe with an initialized terminal: preserve complete events while
// consuming protocol replies until the requested duration elapses or 64 events are
// queued. Check the deadline between decodes; a decode already started can overrun it.
// Escape continuation uses config::default_terminal_escape_delay_ms (100 ms) per
// native read, in addition to ncurses ESCDELAY; paste uses a 1 s per-read timeout.
// These are not whole-operation time limits. Restore scoped timeouts and leave
// unread input for normal polling when capacity or the outer deadline is reached.
void drain_startup_probe_input(std::chrono::milliseconds deadline);

}  // namespace ava::tui::runtime_input
