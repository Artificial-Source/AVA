#pragma once

#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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
  // TUI thread, after a bridge attempt. Only Busy arms a one-shot retry;
  // completion notifications wake accepted/queued work without polling.
  void synchronize_retry_timer();
  // Timer callback publishes readiness only; bridge calls wait for application servicing.
  void mark_retry_due(TuiTimer::Clock::time_point now);

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
  // A copied notifier may outlive this controller. Its shared atomic and weak
  // wake capability remain safe; the callback never captures this or a UI view.
  std::shared_ptr<std::atomic_bool> completion_ready_ = std::make_shared<std::atomic_bool>(false);
  // TUI-owned Busy state. Unrelated input/paint turns must not shorten retry_at_.
  bool bridge_busy_ = false;
  bool retry_due_ = false;  // Set by the timer, consumed by service_if_changed.
  std::chrono::milliseconds retry_delay_{100};  // First contention100ms, subsequent250ms.
  std::optional<TuiTimer::Clock::time_point> retry_at_;  // Present until a retry is eligible.
  TuiTimer retry_timer_;  // Canceled before bridge callbacks are unregistered at shutdown.
};

}  // namespace ava::tui
