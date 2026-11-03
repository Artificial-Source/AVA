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
class ActiveRunEvents final
{
 public:
  ActiveRunEvents(bool command, std::string submitted, std::string initial_request_id, std::vector<TranscriptItem> baseline,
                  std::vector<ava::agent::TodoItem> todos);

  void apply_batch(std::span<QueuedTuiEvent const> events, ComposerSnapshot& snapshot);
  void authorize_receipts(std::vector<std::string> request_ids);
  [[nodiscard]] std::optional<std::vector<TranscriptItem>> take_transcript_projection();
  [[nodiscard]] CappedTranscriptSnapshotUpdate apply_transcript_projection(std::vector<TranscriptItem>& destination, std::vector<TranscriptItem> turn);
  [[nodiscard]] std::vector<TranscriptItem> const& baseline() const noexcept { return baseline_; }
  [[nodiscard]] TuiEventState const& aggregate() const noexcept { return aggregate_; }
  [[nodiscard]] TuiEventState settlement_state() const;
  [[nodiscard]] bool conversation_committed() const noexcept;
  [[nodiscard]] bool authorized_events_received() const;
  [[nodiscard]] std::vector<ToolTimelineItem> merge_unauthorized_tools(std::vector<ToolTimelineItem> tools) const;
  void discard();

  // Submitted/baseline/event partitions contain arbitrary prompt and transcript
  // text plus receipt authority; none may enter generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] TuiEventState& command_state(std::optional<std::string> const& request_id);
  [[nodiscard]] std::vector<TranscriptItem> authorized_transcript() const;
  void apply_runtime(QueuedRuntimeEvent const& event, ComposerSnapshot& snapshot);
  void apply_envelope(ava::event::EventEnvelope const& envelope, ComposerSnapshot& snapshot);
  void apply_subagent_launch(ava::agent::SubagentLaunchNotification const& notification, ComposerSnapshot& snapshot);

  bool command_;
  ava::core::NoLogString submitted_;
  std::string initial_request_id_;
  std::vector<TranscriptItem> baseline_;
  std::size_t leading_evictions_ = 0;
  TuiEventState aggregate_;
  TuiEventState initial_command_;
  TuiEventState unattributed_;
  std::vector<std::pair<std::string, TuiEventState>> request_states_;
  std::vector<std::string> ordinary_turn_request_ids_;
  bool command_events_released_ = false;
  bool projection_pending_ = false;
  bool received_events_ = false;
};

} // namespace ava::tui
