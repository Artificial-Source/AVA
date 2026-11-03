#pragma once

#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ava::tui {

class RuntimeRenderer;
class TranscriptSearchController;
class TuiEventLoop;

enum class MermaidServiceOutcome
{
  Unchanged,
  Changed,
  RenderFailed
};

// TUI-owned reconciliation state. This class performs no process work itself;
// it invokes only the neutral nonblocking bridge from a runtime update seam.
class RuntimeMermaidPresentationController final
{
 public:
  // Snapshot, renderer and transcript search outlive this TUI-thread owner.
  // The bridge remains the external authority for asynchronous rendering work.
  RuntimeMermaidPresentationController(ComposerSnapshot& snapshot, RuntimeRenderer& renderer, TranscriptSearchController& transcript_search,
                                       TuiMermaidRenderBridge bridge);
  RuntimeMermaidPresentationController(RuntimeMermaidPresentationController const&) = delete;
  RuntimeMermaidPresentationController& operator=(RuntimeMermaidPresentationController const&) = delete;
  ~RuntimeMermaidPresentationController();

  // The attached loop outlives this stable owner and its registration.
  // Attach the renderer first so discovery requests only batch invalidation.
  void attach_loop(TuiEventLoop& loop);
  [[nodiscard]] MermaidServiceOutcome service();
  [[nodiscard]] MermaidServiceOutcome service_if_changed();
  [[nodiscard]] MermaidServiceOutcome take_service_outcome() noexcept;
  void shutdown() noexcept;

  // Reconciliation records hold arbitrary diagram source/output, and borrowed
  // presentation/layout owners contain messages. Generated printing must not
  // expose these contents or the bridge's captured authority.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Record
  {
    std::uint64_t item_identity = 0;
    std::uint64_t block_identity = 0;
    std::uint64_t request_identity = 0;
    std::size_t item_index = 0;
    detail::MermaidFenceBlock block;
    bool submitted = false;
    bool pending = false;
    bool failed = false;
    std::optional<std::string> accepted_text = std::nullopt;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  [[nodiscard]] std::optional<std::size_t> service_projection();
  [[nodiscard]] std::uint64_t next_identity() noexcept;
  void cancel_record(Record const& record) noexcept;
  void cancel_all() noexcept;
  void reconcile(ComposerSnapshot const& snapshot);
  void enqueue_waiting() noexcept;
  void drain_completions() noexcept;
  void enforce_result_byte_bound() noexcept;
  [[nodiscard]] std::vector<detail::MermaidAcceptedPresentation> accepted_presentations() const;
  [[nodiscard]] bool has_pending_work() const noexcept;
  void synchronize_discovery_timer();
  void discover_progress(TuiTimer::Clock::time_point now);

  ComposerSnapshot& snapshot_;
  RuntimeRenderer& renderer_;
  TranscriptSearchController& transcript_search_;
  TuiMermaidRenderBridge bridge_;
  std::vector<Record> records_;
  // A removed presentation record can still own a bridge slot until its
  // cancellation/completion is drained. Pairs retain config epoch and identity.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> outstanding_requests_;
  std::uint64_t config_epoch_ = 0;
  bool enabled_ = false;
  std::string session_id_;
  std::size_t observed_transcript_generation_ = 0;
  bool observed_snapshot_ = false;
  std::uint64_t next_identity_ = 1;
  bool shutdown_ = false;
  MermaidServiceOutcome service_outcome_ = MermaidServiceOutcome::Unchanged;
  TuiTimer discovery_timer_;
};

}  // namespace ava::tui
