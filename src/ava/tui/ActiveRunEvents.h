// Reduces active-run events while keeping command receipts separate from transcript authority.
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

// Exact receipt identity is the sole command-event conversation authority.
[[nodiscard]] bool command_event_request_has_conversation_authority(std::optional<std::string> const& event_request_id,
                                                                    std::span<std::string const> ordinary_turn_request_ids) noexcept;

} // namespace detail

// TUI-thread event reduction and receipt-gated transcript ownership. The public
// aggregate/baseline are immutable borrows valid until the next owner mutation.
// ActiveRun::Operation owns this reducer until post-join settlement; workers never
// access it. Only exact backend receipts release command conversation partitions.
class ActiveRunEvents final
{
 public:
  // TUI thread: capture the pre-submit transcript and todo baseline before events
  // arrive; the initial request identity remains separate from follow-up receipts.
  ActiveRunEvents(bool command, std::string submitted, std::string initial_request_id, std::vector<TranscriptItem> baseline,
                  std::vector<ava::agent::TodoItem> todos);

  // TUI thread: reduce in inbox order; non-command tools publish immediately while
  // command transcript partitions remain withheld until receipt authorization.
  void apply_batch(std::span<QueuedTuiEvent const> events, RuntimePresentationState& presentation);
  // TUI thread after worker completion: install backend-authorized ordinary request
  // identities and schedule projection even if the final batch contains no events.
  void authorize_receipts(std::vector<std::string> request_ids);
  // TUI thread: consume the projection-dirty flag only once eligible. Returned text
  // is owned by the caller; command text includes only receipt-authorized requests.
  [[nodiscard]] std::optional<std::vector<TranscriptItem>> take_transcript_projection();
  // TUI thread: combine baseline and current turn under the transcript cap; carry
  // leading evictions forward so repeated snapshots keep stable item offsets.
  [[nodiscard]] CappedTranscriptSnapshotUpdate apply_transcript_projection(std::vector<TranscriptItem>& destination, std::vector<TranscriptItem> turn);
  // TUI thread: borrow the pre-submit sequence until mutation/destruction; settlement
  // uses it to preserve a local command without adding conversation entries.
  [[nodiscard]] std::vector<TranscriptItem> const& baseline() const noexcept { return baseline_; }
  // TUI thread: borrow all-event status/sidebar authority until the next mutation.
  // Its transcript is not proof of command conversation authorization.
  [[nodiscard]] TuiEventState const& aggregate() const noexcept { return aggregate_; }
  // TUI thread: copy settlement evidence, replacing command transcript state with
  // authorized partitions and dropping pending text/tools that could bypass receipts.
  [[nodiscard]] TuiEventState settlement_state() const;
  // TUI thread: ordinary prompts always project; commands require an ordinary-turn receipt.
  [[nodiscard]] bool conversation_committed() const noexcept;
  // TUI thread for command submissions only: test transcript evidence in authorized
  // partitions to decide whether final-result fallback is needed.
  [[nodiscard]] bool authorized_events_received() const;
  // TUI thread: retain settled tools from non-conversation partitions in local
  // command output, merging matching identities so completion does not duplicate cards.
  [[nodiscard]] std::vector<ToolTimelineItem> merge_unauthorized_tools(std::vector<ToolTimelineItem> tools) const;
  // TUI thread on session transition: erase old-session transcript/event authority
  // before the frozen inbox is discarded and this submission is retired.
  void discard();

  // Submitted/baseline/event partitions contain arbitrary prompt and transcript
  // text plus receipt authority; none may enter generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: route an event into its exact request partition; absent identities
  // have no receipt authority. Returned reference lasts only until owner mutation.
  [[nodiscard]] TuiEventState& command_state(std::optional<std::string> const& request_id);
  // TUI thread: materialize only partitions named by ordinary-turn receipts.
  [[nodiscard]] std::vector<TranscriptItem> authorized_transcript() const;
  // TUI thread: preserve aggregate status while separating command transcript authority.
  void apply_runtime(QueuedRuntimeEvent const& event, RuntimePresentationState& presentation);
  // TUI thread: prompt-only audit envelopes update aggregate control state without
  // creating command conversation evidence.
  void apply_envelope(ava::event::EventEnvelope const& envelope, RuntimePresentationState& presentation);
  // TUI thread: correlate launch cards with request partitions before tool publication.
  void apply_subagent_launch(ava::agent::SubagentLaunchNotification const& notification, RuntimePresentationState& presentation);

  // Fixed at construction on the TUI thread; commands need receipt-gated projection.
  bool command_;
  // Owned invocation text used only to remove the literal command from projection.
  ava::core::NoLogString submitted_;
  // Construction-time identity separates the command itself from subsequent turns.
  std::string initial_request_id_;
  // TUI-owned pre-submit transcript restored for local-only command settlement.
  std::vector<TranscriptItem> baseline_;
  // TUI projection accumulates cap evictions so repeated snapshots preserve indices.
  std::size_t leading_evictions_ = 0;
  // TUI reducer of all accepted events; sidebar/control authority does not require receipts.
  TuiEventState aggregate_;
  // TUI-only initial-request partition, withheld unless its identity gets a receipt.
  TuiEventState initial_command_;
  // TUI-only events with no request identity; never authorized as conversation text.
  TuiEventState unattributed_;
  // TUI-only follow-up partitions in first-seen order, selected later by exact receipts.
  std::vector<std::pair<std::string, TuiEventState>> request_states_;
  // TUI installs backend completion receipts; these alone authorize command conversation.
  std::vector<std::string> ordinary_turn_request_ids_;
  // TUI authorization opens projection only when command receipts permit a turn.
  bool command_events_released_ = false;
  // TUI batch/receipt changes mark work; successful projection consumption clears it.
  bool projection_pending_ = false;
};

} // namespace ava::tui
