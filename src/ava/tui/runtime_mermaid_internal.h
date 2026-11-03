// Reconciles diagram presentation with the application bridge using completion wakes and bounded Busy retries.
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

class RuntimePresentationState;

class RuntimeRenderer;
class TranscriptSearchController;
class TuiEventLoop;

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
// bridge owns worker/process lifetime. Current epoch and request identity gate result
// attachment. Only shared readiness and a weak waiter cross into producer callbacks;
// all other members are TUI-thread-only and borrowed owners outlive this controller.
class RuntimeMermaidPresentationController final
{
 public:
  // Snapshot, renderer and transcript search outlive this TUI-thread owner.
  // The bridge remains the external authority for asynchronous rendering work.
  RuntimeMermaidPresentationController(RuntimePresentationState& presentation, RuntimeRenderer& renderer, TranscriptSearchController& transcript_search,
                                       TuiMermaidRenderBridge bridge);
  // One controller owns record identities and notifier registration; copying would duplicate
  // admission/cancellation authority.
  RuntimeMermaidPresentationController(RuntimeMermaidPresentationController const&) = delete;
  // Preserve the lifetime-bound projection, renderer, and search dependencies.
  RuntimeMermaidPresentationController& operator=(RuntimeMermaidPresentationController const&) = delete;
  // TUI thread: call idempotent shutdown while borrowed owners still live; shared/weak notifier captures may
  // safely outlive this object.
  ~RuntimeMermaidPresentationController();

  // The attached loop outlives this stable owner and its registration.
  // Attach the renderer first so discovery requests only batch invalidation.
  void attach_loop(TuiEventLoop& loop);
  // TUI application stage: reconcile records, drain before admitting work, publish accepted geometry, refresh
  // search, and invalidate a frame.
  [[nodiscard]] MermaidServiceOutcome service();
  // TUI application stage: consume readiness or changed projection identity; outstanding requests alone never
  // cause polling.
  [[nodiscard]] MermaidServiceOutcome service_if_changed();
  // TUI thread: cancel retry registration, unregister notification, cancel records, and release bridge
  // callbacks. Already copied notifications may still run but touch only shared readiness and a weak waiter.
  void shutdown() noexcept;

  // Reconciliation records hold arbitrary diagram source/output, and borrowed
  // presentation/layout owners contain messages. Generated printing must not
  // expose these contents or the bridge's captured authority.
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
    // TUI-thread admission history; accepted/rejected requests are not repeatedly submitted for unchanged
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
  // TUI thread: scan bounded completed fences, retain equal source identities, then cancel records no longer
  // present.
  void reconcile(ComposerSnapshot const& snapshot);
  // TUI thread: reserve completion bookkeeping before bridge admission; Busy arms retry, QueueFull awaits a
  // completion wake, rejection leaves fallback.
  void enqueue_waiting() noexcept;
  // TUI thread: treat nullopt as Busy; release outstanding identities before rejecting stale epochs and
  // attaching bounded current results.
  void drain_completions() noexcept;
  // TUI thread after drain: retain newer accepted results within the shared presentation byte budget and fall
  // back for older overflow.
  void enforce_result_byte_bound() noexcept;
  // TUI thread: copy only accepted records into an immutable renderer projection; no bridge/process authority
  // crosses this boundary.
  [[nodiscard]] std::vector<detail::MermaidAcceptedPresentation> accepted_presentations() const;
  // TUI thread, after a bridge attempt. Only Busy arms a one-shot retry;
  // completion notifications wake accepted/queued work without polling.
  void synchronize_retry_timer();
  // Timer callback publishes readiness only; bridge calls wait for application servicing.
  void mark_retry_due(TuiTimer::Clock::time_point now);

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
  // A removed presentation record can still own a bridge slot until its
  // cancellation/completion is drained. Pairs retain config epoch and identity.
  // TUI-thread epoch/identity pairs survive record removal so canceled or old-session completions still release
  // bridge capacity.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> outstanding_requests_;
  // TUI-thread last reconciled configuration epoch; a changed epoch retires prior results and identities.
  std::uint64_t config_epoch_ = 0;
  // TUI-thread last reconciled display admission flag; disabled presentation still drains canceled same-epoch
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
  // TUI-thread one-way teardown latch; service and retry admission stop after it is set.
  bool shutdown_ = false;
  // A copied notifier may outlive this controller. Its shared atomic and weak
  // wake capability remain safe; the callback never captures this or a UI view.
  // Shared atomic ownership is required because producer callbacks may outlive this controller; producer stores
  // release before weak wake, TUI exchanges acquire before drainage.
  std::shared_ptr<std::atomic_bool> completion_ready_ = std::make_shared<std::atomic_bool>(false);
  // TUI-owned Busy state. Unrelated input/paint turns must not shorten retry_at_.
  // TUI-thread contention result from the last eligible bridge attempt; only this condition schedules retry
  // wakes.
  bool bridge_busy_ = false;
  // TUI timer publishes eligibility here; application-stage servicing consumes it before any bridge call.
  bool retry_due_ = false;  // Set by the timer, consumed by service_if_changed.
  // TUI-thread next Busy delay: 100 ms initially, 250 ms thereafter; successful/nonbusy service resets it.
  std::chrono::milliseconds retry_delay_{100};  // First contention 100 ms, subsequent 250 ms.
  // TUI-thread absolute contention deadline; unrelated transcript/input changes cannot shorten this backoff.
  std::optional<TuiTimer::Clock::time_point> retry_at_;  // Present until a retry is eligible.
  // TUI-owned one-shot Busy wake; canceled before unregistering bridge callbacks so no retry targets a retired
  // controller.
  TuiTimer retry_timer_;  // Canceled before bridge callbacks are unregistered at shutdown.
};

}  // namespace ava::tui
