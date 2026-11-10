// Reconciles diagram presentation with the application bridge using completion wakes and bounded Busy retries.
#pragma once

#include "TuiTimerHandle.h"
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

class RuntimePresentationState;

class RuntimeRenderer;
class TranscriptSearchController;
class TuiTimerRegistry;

// TUI-thread application-stage result; session control distinguishes a changed projection from latched renderer
// failure.
enum class MermaidServiceOutcome
{
  // No accepted presentation changed, so service adds no frame invalidation.
  Unchanged,
  // Accepted diagram projection changed and a full frame was successfully requested.
  Changed,
  // Projection changed but renderer failure prevented its frame request.
  RenderFailed
};

// Session-owned TUI-thread reconciliation of retained fences and accepted output.
// Renderer and search consume its published immutable projection; the application
// bridge owns worker/process lifetime. Results attach only when their configuration
// epoch and request identity match the retained record. Only shared readiness and a weak waiter cross into producer callbacks;
// all other members are TUI-thread-only and borrowed owners outlive this controller.
class RuntimeMermaidPresentationController final
{
 public:
  // TUI thread: borrow presentation, renderer, and search for this controller's lifetime; take bridge
  // callbacks whose application owner manages asynchronous rendering.
  RuntimeMermaidPresentationController(RuntimePresentationState& presentation, RuntimeRenderer& renderer, TranscriptSearchController& transcript_search,
                                       TuiMermaidRenderBridge bridge);
  // Not copyable: one controller submits/cancels these records and owns their notifier registration.
  RuntimeMermaidPresentationController(RuntimeMermaidPresentationController const&) = delete;
  RuntimeMermaidPresentationController& operator=(RuntimeMermaidPresentationController const&) = delete;
  // TUI thread: call idempotent shutdown while borrowed owners still live; shared/weak notifier captures may
  // safely outlive this object.
  ~RuntimeMermaidPresentationController();

  // The timer registry outlives this stable owner and its timer.
  // The already-constructed renderer outlives this owner and batches its frame requests.
  // A nonempty bridge must support notifier registration, including while disabled.
  void create_timers(TuiTimerRegistry& timer_registry);
  // TUI application stage: reconcile records, drain results before submitting work, update accepted diagram layout, refresh
  // search, and invalidate a frame.
  [[nodiscard]] MermaidServiceOutcome service();
  // TUI application stage: respond to completion/retry readiness or changed transcript/configuration; outstanding requests alone never
  // cause polling.
  [[nodiscard]] MermaidServiceOutcome service_if_changed();
  // TUI thread: disarm the retry timer, unregister notification, cancel records, and release bridge
  // callbacks. Already copied notifications may still run but touch only shared readiness and a weak waiter.
  void shutdown() noexcept;

  // Reconciliation records hold arbitrary diagram source/output, and borrowed
  // presentation/layout owners contain messages. Generated printing must not
  // expose these contents or data captured by the bridge callbacks.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Controller-owned TUI-thread record for one retained completed fence. Source identity survives item-index
  // shifts; only matching current-epoch completions may attach text. It owns source/output and no producer
  // borrows it.
  struct Record
  {
    // TUI-thread stable identity allocated when an item first gains a record; shared by newly discovered blocks
    // in that item.
    std::uint64_t item_identity = 0;
    // TUI-thread stable identity of this exact fence/source; accepted projection comparison uses it to detect
    // changes.
    std::uint64_t block_identity = 0;
    // TUI-thread opaque bridge identity, initially the block identity; paired with configuration epoch for
    // cancellation/completion.
    std::uint64_t request_identity = 0;
    // TUI-thread current retained transcript index; reconciliation updates it after movement/eviction.
    std::size_t item_index = 0;
    // Owned fence offsets and source captured by reconciliation; matching source/offsets allow a prior record
    // to survive.
    detail::MermaidFenceBlock block;
    // TUI-thread submission history; accepted/rejected requests are not repeatedly submitted for unchanged
    // source.
    bool submitted = false;
    // TUI-thread accepted request awaiting a completion; cleared when matching result arrives, regardless of
    // success.
    bool pending = false;
    // TUI-thread fallback latch for rejection, invalid output, or budget eviction; unchanged source remains
    // rendered as its original fence.
    bool failed = false;
    // Owned accepted helper text set by completion; TUI-thread byte-budget enforcement may discard it and latch
    // fallback.
    std::optional<std::string> accepted_text = std::nullopt;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  // TUI thread: reconcile session/configuration and bridge results, then return earliest possibly changed item
  // for incremental search.
  [[nodiscard]] std::optional<std::size_t> service_projection();
  // TUI thread: allocate a nonzero local identity for new item/block records; callers retain it through
  // reconciliation.
  [[nodiscard]] std::uint64_t next_identity() noexcept;
  // TUI thread: best-effort cancel only a pending record; bridge exceptions cannot escape teardown, and
  // outstanding bookkeeping remains until drained.
  void cancel_record(Record const& record) noexcept;
  // TUI thread: cancel retained pending records and release their source/output; callers decide whether old
  // outstanding slots still need drainage.
  void cancel_all() noexcept;
  // TUI thread: scan completed fences across the transcript, keep only the newest bounded candidate set,
  // reuse matching records, then cancel records no longer present.
  void reconcile(ComposerSnapshot const& snapshot);
  // TUI thread: reserve completion bookkeeping before submitting to the bridge; Busy arms retry, QueueFull awaits a
  // completion wake, rejection leaves fallback.
  void enqueue_waiting() noexcept;
  // TUI thread: treat nullopt as Busy; release outstanding identities before rejecting stale epochs and
  // attaching bounded current results.
  void drain_completions() noexcept;
  // TUI thread after drain: retain newer accepted results within the shared presentation byte budget and fall
  // back for older overflow.
  void enforce_result_byte_bound() noexcept;
  // TUI thread: copy only accepted records into renderer display data; rendering workers and processes
  // remain owned by the application bridge.
  [[nodiscard]] std::vector<detail::MermaidAcceptedPresentation> accepted_presentations() const;
  // TUI thread, after a bridge attempt. Only Busy arms a one-shot retry;
  // completion notifications wake accepted/queued work without polling.
  void synchronize_retry_timer();
  // Timer callback publishes readiness only; bridge calls wait for application servicing.
  void mark_retry_due(TuiTimerHandle::Clock::time_point now);

  // Borrowed session projection outlives this controller; TUI thread reads transcript/configuration and
  // publishes accepted results.
  RuntimePresentationState& presentation_;
  // Borrowed renderer outlives this controller and its service calls; coordinates viewport preservation and
  // frame invalidation.
  RuntimeRenderer& renderer_;
  // Borrowed search owner outlives this controller; accepted diagram geometry invalidates affected searchable
  // rows.
  TranscriptSearchController& transcript_search_;
  // Owned callback bundle retains application bridge lifetime; shutdown unregisters its notifier before
  // releasing callbacks.
  TuiMermaidRenderBridge bridge_;
  // TUI-thread bounded retained fences and accepted results; producers publish completions without touching
  // this container.
  std::vector<Record> records_;
  // TUI-thread configuration-epoch/request-ID pairs survive record removal: a canceled or old-session
  // request still occupies bridge capacity until its completion is drained.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> outstanding_requests_;
  // TUI-thread last reconciled configuration epoch; a changed epoch retires prior results and identities.
  std::uint64_t config_epoch_ = 0;
  // TUI-thread last reconciled diagram-display setting; disabled presentation still drains canceled same-epoch
  // results.
  bool enabled_ = false;
  // TUI-thread last observed session identity; a session change cancels retained records before discovering new
  // ones.
  std::string session_id_;
  // TUI-thread generation last reconciled; a changed generation triggers fence discovery even without a
  // completion wake.
  std::size_t observed_transcript_generation_ = 0;
  // TUI-thread first-service marker, ensuring an initially empty/equal snapshot still receives reconciliation.
  bool observed_snapshot_ = false;
  // TUI-thread allocator cursor retained across sessions; it is monotonic until uint64_t wrap.
  // Wrap skips zero but does not check for collisions with retained/outstanding identities.
  std::uint64_t next_identity_ = 1;
  // TUI-thread one-way teardown latch; service and retries stop after it is set.
  bool shutdown_ = false;
  // Shared because copied producer callbacks may outlive this controller. They capture only this atomic
  // and a weak wake handle, never the controller or screen; store-release precedes wake, and the TUI
  // exchanges with acquire ordering before draining results.
  std::shared_ptr<std::atomic_bool> completion_ready_ = std::make_shared<std::atomic_bool>(false);
  // TUI-thread Busy result from the last eligible bridge attempt; only Busy schedules a retry. Unrelated
  // input and painting must not shorten retry_at_.
  bool bridge_busy_ = false;
  // TUI timer publishes eligibility here; application-stage servicing consumes it before any bridge call.
  bool retry_due_ = false;
  // TUI-thread next Busy delay: 100 ms initially, 250 ms thereafter; successful/nonbusy service resets it.
  std::chrono::milliseconds retry_delay_{100};
  // TUI-thread absolute contention deadline; unrelated transcript/input changes cannot shorten this backoff.
  std::optional<TuiTimerHandle::Clock::time_point> retry_at_;
  // TUI-owned one-shot Busy wake; canceled before unregistering bridge callbacks so no retry targets a retired
  // controller.
  TuiTimerHandle retry_timer_;
};

}  // namespace ava::tui
