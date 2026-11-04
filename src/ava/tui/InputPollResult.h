// Distinguishes decoded input, discarded protocol bytes and an empty terminal read.
// TuiSession uses this result to bound each turn without mistaking discard for drainage.
#pragma once

#include "RuntimeInput.h"

namespace ava::tui::runtime_input {

// TUI poll classification keeps consumed protocol replies distinct from native
// emptiness, so TuiSession charges discarded replies to its budget and keeps draining.
enum class InputPollKind
{
  // No startup item and native read returned ERR (empty/timeout/read failure);
  // another source may still require a runnable turn before TuiSession can wait.
  Empty,
  // Consumed a protocol reply or unsupported key; charge the input budget and poll again.
  Discarded,
  // Owns an actionable decoded event; TuiSession may dispatch it before polling again.
  Input
};

// Value returned to TuiSession on the TUI thread; it owns its payload independently
// of ncurses. Only Input makes input meaningful, while Empty and Discarded guide
// further polling without dispatch. Destruction requires no terminal or decoder lifetime.
struct InputPollResult
{
  // Poller sets this on the TUI thread; TuiSession tests it before touching input
  // so a discarded key cannot masquerade as evidence that buffered input is exhausted.
  InputPollKind kind = InputPollKind::Empty;
  // Poller moves an owned event here only for Input; TUI dispatch borrows it until
  // the result is destroyed, and may copy it when a later stage must retain it.
  RuntimeInput input{};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::runtime_input
