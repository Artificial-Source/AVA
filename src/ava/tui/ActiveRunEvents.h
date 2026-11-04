// Collects active-run events and filters command conversation text by backend request IDs.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/event_state.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ava::tui {
namespace detail {

// For a slash command, an event enters the conversation only when its nonempty
// request ID exactly matches a backend-returned ordinary_turn_request_ids entry,
// which confirms a real model turn.
[[nodiscard]] bool command_event_request_has_conversation_authority(std::optional<std::string> const& event_request_id,
                                                                    std::span<std::string const> ordinary_turn_request_ids) noexcept;

} // namespace detail

// ActiveRun::Operation owns this TUI-thread event reducer until completion cleanup
// after worker join. Workers never access it. aggregate()/baseline() return const
// borrows valid until the next mutation or destruction, for TUI-thread callers only.
// Command events are grouped by request ID; only groups named in the backend's
// ordinary_turn_request_ids may supply conversation text.
class ActiveRunEvents final
{
 public:
  // TUI thread: capture the pre-submit transcript and todo baseline before events
  // arrive; keep the initial request's events separate from follow-up request events.
  ActiveRunEvents(bool command, std::string submitted, std::string initial_request_id, std::vector<TranscriptItem> baseline,
                  std::vector<ava::agent::TodoItem> todos);

  // TUI thread: reduce in inbox order; non-command tools publish immediately while
  // command transcript groups wait for the backend's ordinary_turn_request_ids.
  void apply_batch(std::span<QueuedTuiEvent const> events, RuntimePresentationState& presentation);
  // TUI thread after worker completion: store TuiSubmitResult::ordinary_turn_request_ids,
  // confirming real model turns, and mark the transcript for update even without new events.
  void authorize_receipts(std::vector<std::string> request_ids);
  // TUI thread: take a pending transcript update only when eligible. The caller owns
  // the returned items; command text includes only matching ordinary-turn request IDs.
  [[nodiscard]] std::optional<std::vector<TranscriptItem>> take_transcript_projection();
  // TUI thread: combine baseline and current turn under the transcript cap; carry
  // leading evictions forward so repeated snapshots keep stable item offsets.
  [[nodiscard]] CappedTranscriptSnapshotUpdate apply_transcript_projection(std::vector<TranscriptItem>& destination, std::vector<TranscriptItem> turn);
  // TUI thread: borrow the pre-submit sequence until mutation/destruction; completion
  // uses it to preserve a local command without adding conversation entries.
  [[nodiscard]] std::vector<TranscriptItem> const& baseline() const noexcept { return baseline_; }
  // TUI thread: borrow status/sidebar data from all events until the next mutation.
  // For commands, its transcript has not been filtered by ordinary_turn_request_ids.
  [[nodiscard]] TuiEventState const& aggregate() const noexcept { return aggregate_; }
  // TUI thread: copy completion data, using only matching command transcript groups
  // and removing pending text/tools that have not passed the request-ID check.
  [[nodiscard]] TuiEventState settlement_state() const;
  // TUI thread: ordinary prompts enter the conversation; commands need at least one
  // backend-returned ordinary-turn request ID.
  [[nodiscard]] bool conversation_committed() const noexcept;
  // TUI thread for command submissions only: check matching request groups for
  // streamed transcript data before deciding whether to use final-result fallback.
  [[nodiscard]] bool authorized_events_received() const;
  // TUI thread: retain finished tools from non-conversation request groups in local
  // command output, merging matching identities so completion does not duplicate cards.
  [[nodiscard]] std::vector<ToolTimelineItem> merge_unauthorized_tools(std::vector<ToolTimelineItem> tools) const;
  // TUI thread on session transition: erase old-session transcript/events before
  // discarding the closed inbox and clearing this submission.
  void discard();

  // Submitted/baseline/event partitions contain arbitrary prompt and transcript
  // text and request IDs; none may enter generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: group an event by exact request ID; events without an ID cannot
  // supply command conversation text. The reference lasts until owner mutation.
  [[nodiscard]] TuiEventState& command_state(std::optional<std::string> const& request_id);
  // TUI thread: build transcript items only from groups matching ordinary-turn request IDs.
  [[nodiscard]] std::vector<TranscriptItem> authorized_transcript() const;
  // TUI thread: update combined status and keep command transcript events grouped by request ID.
  void apply_runtime(QueuedRuntimeEvent const& event, RuntimePresentationState& presentation);
  // TUI thread: prompt-only audit envelopes update aggregate control state without
  // entering command conversation groups.
  void apply_envelope(ava::event::EventEnvelope const& envelope, RuntimePresentationState& presentation);
  // TUI thread: group launch cards by request ID before updating displayed tools.
  void apply_subagent_launch(ava::agent::SubagentLaunchNotification const& notification, RuntimePresentationState& presentation);

  // Fixed at construction on the TUI thread; commands require transcript request-ID filtering.
  bool command_;
  // Owned invocation text used only to remove the literal command from the displayed transcript.
  ava::core::NoLogString submitted_;
  // Construction-time identity separates the command itself from subsequent turns.
  std::string initial_request_id_;
  // TUI-owned pre-submit transcript restored when a command produces only local output.
  std::vector<TranscriptItem> baseline_;
  // Counts items removed by the transcript size cap so repeated snapshots preserve indices.
  std::size_t leading_evictions_ = 0;
  // TUI reducer of all accepted events; sidebar/control updates use every request ID.
  TuiEventState aggregate_;
  // TUI-only initial-request events, withheld unless their ID matches an ordinary-turn request ID.
  TuiEventState initial_command_;
  // TUI-only events with no request identity; never authorized as conversation text.
  TuiEventState unattributed_;
  // TUI-only follow-up groups in first-seen order, filtered later by exact request ID.
  std::vector<std::pair<std::string, TuiEventState>> request_states_;
  // Backend-returned IDs confirming real model turns; only matching command events enter conversation.
  std::vector<std::string> ordinary_turn_request_ids_;
  // TUI sets this when a command result contains at least one ordinary-turn request ID.
  bool command_events_released_ = false;
  // Event batches or request-ID updates set this; taking the transcript update clears it.
  bool projection_pending_ = false;
};

} // namespace ava::tui
